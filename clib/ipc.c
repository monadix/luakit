/*
 * Copyright © 2016 Aidan Holm <aidanholm@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <webkit2/webkit2.h>
#include "clib/widget.h"
#include "common/ipc.h"
#include "widgets/webview.h"
#include "common/luauniq.h"

#include "common/clib/ipc.h"
#include "common/luaserialize.h"

#define REG_KEY "luakit.registry.ipc_channel"

gint
ipc_channel_send(lua_State *L)
{
    ipc_channel_t *ipc_channel = luaH_check_ipc_channel(L, 1);
    guint64 page_id = 0;
    ipc_endpoint_t *ipc = NULL;

    /* Optional first argument: view or view id to send message to */
    if (lua_isuserdata(L, 2)) {
        widget_t *w = luaH_checkwebview(L, 2);
        page_id = webkit_web_view_get_page_id(WEBKIT_WEB_VIEW(w->widget));
        if (!w) return luaL_error(L, "unknown webview");
        ipc = webview_get_endpoint(w);
        lua_remove(L, 2);
    } else if (lua_isnumber(L, 2)) {
        page_id = lua_tointeger(L, 2);
        widget_t *w = webview_get_by_id(page_id);
        if (!w) return luaL_error(L, "unknown webview");
        ipc = webview_get_endpoint(w);
        lua_remove(L, 2);
    }

    const char *signal = luaL_checkstring(L, 2);
    gboolean trusted = (!strcmp(ipc_channel->name, "select_wm") && !strcmp(signal, "set_label_maker")) ||
        (!strcmp(ipc_channel->name, "follow_wm") && !strcmp(signal, "enter"));
    lua_pushstring(L, ipc_channel->name);
    lua_pushinteger(L, page_id);

    if (ipc) {
        if (trusted) ipc_send_lua_trusted(ipc, L, 2, lua_gettop(L));
        else ipc_send_lua(ipc, IPC_TYPE_lua_ipc, L, 2, lua_gettop(L));
    } else {
        ipc_endpoint_t *broadcast = ipc_endpoint_new("broadcast");
        broadcast->status = IPC_ENDPOINT_CONNECTED;
        if (trusted) ipc_send_lua_trusted(broadcast, L, 2, lua_gettop(L));
        else ipc_send_lua(broadcast, IPC_TYPE_lua_ipc, L, 2, lua_gettop(L));
        ipc_endpoint_decref(broadcast);
    }

    return 0;
}

#define BROKER_KEY "luakit.registry.web_broker"

gint
ipc_channel_add_web_signal(lua_State *L)
{
    ipc_channel_t *channel = luaH_check_ipc_channel(L, 1);
    const char *name = luaL_checkstring(L, 2);
    luaH_checktable(L, 3);
    luaH_checkfunction(L, 4);
    lua_getfield(L, 3, "validate");
    luaH_checkfunction(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 3, "authorize");
    luaH_checkfunction(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, BROKER_KEY);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, BROKER_KEY);
    }
    lua_getfield(L, -1, channel->name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, channel->name);
    }
    lua_newtable(L);
    lua_pushvalue(L, 3);
    lua_setfield(L, -2, "policy");
    lua_pushvalue(L, 4);
    lua_setfield(L, -2, "handler");
    lua_setfield(L, -2, name);
    return 0;
}

static gboolean
check_policy(lua_State *L, int record, const char *check, int channel, int view, int start, int count)
{
    lua_getfield(L, record, "policy");
    lua_getfield(L, -1, check);
    lua_remove(L, -2);
    lua_pushvalue(L, channel);
    lua_pushvalue(L, view);
    for (int i = 0; i < count; i++) lua_pushvalue(L, start + i);
    int status = lua_pcall(L, count + 2, 1, 0);
    gboolean ok = !status && lua_type(L, -1) == LUA_TBOOLEAN && lua_toboolean(L, -1);
    lua_pop(L, 1);
    return ok;
}

void
ipc_channel_recv_web(lua_State *L, ipc_endpoint_t *ipc, const gchar *arg, guint length)
{
    int top = lua_gettop(L);
    int n = lua_deserialize_range(L, (const guint8 *)arg, length);
    if (!ipc->owner || n < 2 || lua_type(L, top + 1) != LUA_TSTRING ||
        lua_type(L, top + n) != LUA_TSTRING) goto done;
    if (!lua_checkstack(L, n + 12)) goto done;
    const char *signal = lua_tostring(L, top + 1);
    const char *name = lua_tostring(L, top + n);
    if (strlen(signal) != lua_objlen(L, top + 1) ||
        strlen(name) != lua_objlen(L, top + n) || strlen(signal) > 128 || strlen(name) > 256)
        goto done;
    lua_getfield(L, LUA_REGISTRYINDEX, BROKER_KEY);
    if (!lua_istable(L, -1)) goto done;
    lua_getfield(L, -1, name);
    if (!lua_istable(L, -1)) goto done;
    lua_getfield(L, -1, signal);
    if (!lua_istable(L, -1)) goto done;
    int record = lua_gettop(L);
    lua_pushstring(L, name);
    if (!luaH_uniq_get(L, REG_KEY, -1)) goto done;
    int channel = lua_gettop(L);
    widget_t *w = ipc->owner;
    luaH_object_push(L, w->ref);
    int view = lua_gettop(L);
    guint64 generation = ipc->generation;
    if (!check_policy(L, record, "validate", channel, view, top + 2, n - 2) ||
        !ipc->owner || ipc->generation != generation) goto done;
    if (!check_policy(L, record, "authorize", channel, view, top + 2, n - 2) ||
        !ipc->owner || ipc->generation != generation) goto done;
    lua_pushvalue(L, channel);
    lua_pushvalue(L, view);
    for (int i = 0; i < n - 2; i++) lua_pushvalue(L, top + 2 + i);
    lua_getfield(L, record, "handler");
    luaH_dofunction(L, n, 0);
done:
    lua_settop(L, top);
}

// vim: ft=c:et:sw=4:ts=8:sts=4:tw=80
