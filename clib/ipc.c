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

#include <math.h>
#include <webkit2/webkit2.h>
#include "clib/widget.h"
#include "common/ipc.h"
#include "widgets/webview.h"
#include "common/luauniq.h"

#include "common/clib/ipc.h"
#include "common/luaserialize.h"

#define REG_KEY "luakit.registry.ipc_channel"
static guint64 routes_revision;

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

    luaL_checkstring(L, 2);
    lua_pushstring(L, ipc_channel->name);
    lua_pushinteger(L, page_id);

    if (ipc) {
        if (!ipc_endpoint_can_send(ipc, ipc->routes_revision_sent == routes_revision ? 1 : 2))
            return luaL_error(L, "IPC queue is full");
        ipc_channel_send_routes(L, ipc);
        ipc_send_lua_trusted(ipc, L, 2, lua_gettop(L));
    } else {
        ipc_endpoint_t *broadcast = ipc_endpoint_new("broadcast");
        broadcast->status = IPC_ENDPOINT_CONNECTED;
        ipc_channel_send_routes(L, broadcast);
        ipc_send_lua_trusted(broadcast, L, 2, lua_gettop(L));
        ipc_endpoint_decref(broadcast);
    }

    return 0;
}

#define BROKER_KEY "luakit.registry.web_broker"

/* Leave the channel's broker table on the stack. */
static void
broker_table(lua_State *L, const char *channel)
{
    lua_getfield(L, LUA_REGISTRYINDEX, BROKER_KEY);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, BROKER_KEY);
    }
    lua_getfield(L, -1, channel);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, channel);
    }
    lua_remove(L, -2);
}

/* Copy policy callbacks: mutating the caller's table cannot remove a gate. */
static guint
push_policy(lua_State *L, int index)
{
    luaH_checktable(L, index);
    lua_newtable(L);
    const char *checks[] = { "validate", "authorize" };
    for (guint i = 0; i < LENGTH(checks); i++) {
        lua_getfield(L, index, checks[i]);
        luaH_checkfunction(L, -1);
        lua_setfield(L, -2, checks[i]);
    }
    lua_getfield(L, index, "legacy_page_arg");
    guint arg = 0;
    if (!lua_isnil(L, -1)) {
        double value = lua_tonumber(L, -1);
        if (lua_type(L, -1) != LUA_TNUMBER || !isfinite(value) || value < 1 || value > IPC_VALUE_LIMIT || value != (guint)value)
            luaL_error(L, "legacy_page_arg must be a positive payload argument index");
        arg = value;
    }
    lua_pop(L, 1);
    return arg;
}

void
ipc_channel_send_routes(lua_State *L, ipc_endpoint_t *ipc)
{
    if (ipc && ipc->routes_revision_sent == routes_revision) return;
    int top = lua_gettop(L);
    lua_pushnumber(L, routes_revision);
    lua_newtable(L);
    int routes = lua_gettop(L);
    lua_getfield(L, LUA_REGISTRYINDEX, BROKER_KEY);
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            lua_newtable(L);
            int signals = lua_gettop(L);
            lua_pushnil(L);
            while (lua_next(L, signals - 1)) {
                lua_getfield(L, -1, "legacy");
                gboolean legacy = lua_toboolean(L, -1);
                lua_pop(L, 1);
                if (legacy) {
                    lua_getfield(L, -1, "page_arg");
                    if (lua_tointeger(L, -1) > 0) {
                        lua_pushvalue(L, -3);
                        lua_pushvalue(L, -2);
                        lua_rawset(L, signals);
                    }
                    lua_pop(L, 1);
                }
                lua_pop(L, 1);
            }
            lua_pushvalue(L, signals - 2);
            lua_pushvalue(L, signals);
            lua_rawset(L, routes);
            lua_pop(L, 2);
        }
    }
    lua_pop(L, 1);
    ipc_endpoint_t *broadcast = NULL;
    if (!ipc) {
        broadcast = ipc = ipc_endpoint_new("legacy routes");
        ipc->status = IPC_ENDPOINT_CONNECTED;
    }
    ipc_send_lua(ipc, IPC_TYPE_lua_routes, L, top + 1, top + 2);
    ipc->routes_revision_sent = routes_revision;
    if (broadcast) ipc_endpoint_decref(broadcast);
    lua_settop(L, top);
}

gint
ipc_channel_add_web_signal(lua_State *L)
{
    ipc_channel_t *channel = luaH_check_ipc_channel(L, 1);
    const char *name = luaL_checkstring(L, 2);
    luaH_checkfunction(L, 4);
    if (push_policy(L, 3)) return luaL_error(L, "legacy_page_arg belongs to add_signal policies");
    int policy = lua_gettop(L);
    broker_table(L, channel->name);
    lua_getfield(L, -1, name);
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "legacy");
        if (lua_toboolean(L, -1)) return luaL_error(L, "cannot mix legacy and modern web handlers");
        lua_pop(L, 1);
        /* A dispatch snapshot must see replacement immediately. */
        lua_pushnil(L);
        lua_setfield(L, -2, "handler");
    }
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, policy);
    lua_setfield(L, -2, "policy");
    lua_pushvalue(L, 4);
    lua_setfield(L, -2, "handler");
    lua_setfield(L, -2, name);
    return 0;
}

gint
ipc_channel_add_signal(lua_State *L)
{
    ipc_channel_t *channel = luaH_check_ipc_channel(L, 1);
    const char *name = luaL_checkstring(L, 2);
    luaH_checkfunction(L, 3);
    if (lua_isnoneornil(L, 4)) return luaH_object_add_signal_simple(L);
    guint page_arg = push_policy(L, 4);
    int policy = lua_gettop(L);
    broker_table(L, channel->name);
    lua_getfield(L, -1, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushboolean(L, TRUE);
        lua_setfield(L, -2, "legacy");
        lua_pushinteger(L, page_arg);
        lua_setfield(L, -2, "page_arg");
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, name);
    } else {
        lua_getfield(L, -1, "legacy");
        if (!lua_toboolean(L, -1)) return luaL_error(L, "cannot mix legacy and modern web handlers");
        lua_pop(L, 1);
        lua_getfield(L, -1, "page_arg");
        if (lua_tointeger(L, -1) != page_arg) return luaL_error(L, "conflicting legacy_page_arg positions");
        lua_pop(L, 1);
    }
    int record = lua_gettop(L);
    lua_newtable(L);
    lua_pushvalue(L, policy);
    lua_setfield(L, -2, "policy");
    lua_pushvalue(L, 3);
    lua_setfield(L, -2, "handler");
    guint occurrence = 1;
    signal_array_t *local_handlers = signal_lookup(channel->signals, name);
    for (guint i = 0; local_handlers && i < local_handlers->len; i++)
        if (g_ptr_array_index(local_handlers, i) == lua_topointer(L, 3)) occurrence++;
    lua_pushinteger(L, occurrence);
    lua_setfield(L, -2, "occurrence");
    lua_rawseti(L, record, lua_objlen(L, record) + 1);
    luaH_object_add_signal(L, 1, name, 3);
    routes_revision++;
    ipc_channel_send_routes(L, NULL);
    return 0;
}

static void
remove_broker_signal(lua_State *L, gboolean all)
{
    ipc_channel_t *channel = luaH_check_ipc_channel(L, 1);
    const char *name = luaL_checkstring(L, 2);
    if (!all) luaH_checkfunction(L, 3);
    int top = lua_gettop(L);
    broker_table(L, channel->name);
    int signals = lua_gettop(L);
    lua_getfield(L, signals, name);
    if (lua_istable(L, -1)) {
        int record = lua_gettop(L);
        lua_getfield(L, record, "legacy");
        gboolean legacy = lua_toboolean(L, -1);
        lua_pop(L, 1);
        if (legacy) {
            guint count = lua_objlen(L, record), kept = 0;
            for (guint i = 1; i <= count; i++) {
                lua_rawgeti(L, record, i);
                lua_getfield(L, -1, "handler");
                gboolean match = !all && lua_rawequal(L, -1, 3);
                lua_pop(L, 1);
                lua_getfield(L, -1, "occurrence");
                guint occurrence = lua_tointeger(L, -1);
                lua_pop(L, 1);
                gboolean remove = all || (match && occurrence == 1);
                if (match && occurrence > 1) {
                    lua_pushinteger(L, occurrence - 1);
                    lua_setfield(L, -2, "occurrence");
                }
                if (!remove) lua_rawseti(L, record, ++kept);
                else {
                    /* A dispatch snapshot must also see removal immediately. */
                    lua_pushnil(L);
                    lua_setfield(L, -2, "handler");
                    lua_pop(L, 1);
                }
            }
            for (guint i = kept + 1; i <= count; i++) {
                lua_pushnil(L);
                lua_rawseti(L, record, i);
            }
            if (!kept) {
                lua_pushnil(L);
                lua_setfield(L, signals, name);
            }
            routes_revision++;
            ipc_channel_send_routes(L, NULL);
        } else {
            lua_getfield(L, record, "handler");
            gboolean remove = all || lua_rawequal(L, -1, 3);
            lua_pop(L, 1);
            if (remove) {
                lua_pushnil(L);
                lua_setfield(L, record, "handler");
                lua_pushnil(L);
                lua_setfield(L, signals, name);
            }
        }
    }
    lua_settop(L, top);
}

gint ipc_channel_remove_signal(lua_State *L)
{
    remove_broker_signal(L, FALSE);
    return luaH_object_remove_signal_simple(L);
}

gint ipc_channel_remove_signals(lua_State *L)
{
    remove_broker_signal(L, TRUE);
    return luaH_object_remove_signals_simple(L);
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
    lua_getfield(L, record, "legacy");
    gboolean legacy = lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (legacy) {
        lua_getfield(L, record, "page_arg");
        guint page_arg = lua_tointeger(L, -1);
        lua_pop(L, 1);
        if (page_arg && (page_arg > (guint)n - 2 || lua_type(L, top + 1 + page_arg) != LUA_TNUMBER ||
            lua_tonumber(L, top + 1 + page_arg) != webkit_web_view_get_page_id(WEBKIT_WEB_VIEW(w->widget))))
            goto done;
    }
    /* Snapshot opted-in records, never the ordinary signal-handler list. */
    lua_newtable(L);
    int records = lua_gettop(L);
    guint count = legacy ? lua_objlen(L, record) : 1;
    for (guint i = 1; i <= count; i++) {
        if (legacy) lua_rawgeti(L, record, i);
        else lua_pushvalue(L, record);
        lua_rawseti(L, records, i);
    }
    for (guint i = 1; i <= count; i++) {
        if (!ipc->owner || ipc->generation != generation) break;
        lua_rawgeti(L, records, i);
        int item = lua_gettop(L);
        lua_getfield(L, item, "handler");
        gboolean live = lua_isfunction(L, -1);
        lua_pop(L, 1);
        if (live && check_policy(L, item, "validate", channel, view, top + 2, n - 2) &&
            ipc->owner && ipc->generation == generation &&
            check_policy(L, item, "authorize", channel, view, top + 2, n - 2) &&
            ipc->owner && ipc->generation == generation) {
            /* Authorization may remove the handler. */
            lua_getfield(L, item, "handler");
            if (lua_isfunction(L, -1)) {
                lua_pop(L, 1);
                lua_pushvalue(L, channel);
                if (!legacy) lua_pushvalue(L, view);
                for (int j = 0; j < n - 2; j++) lua_pushvalue(L, top + 2 + j);
                lua_getfield(L, item, "handler");
                luaH_dofunction(L, n - (legacy ? 1 : 0), 0);
            } else lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
done:
    lua_settop(L, top);
}

// vim: ft=c:et:sw=4:ts=8:sts=4:tw=80
