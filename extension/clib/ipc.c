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
#include "extension/ipc.h"
#include "extension/extension.h"
#include "extension/clib/page.h"
#include "common/clib/ipc.h"
#include "common/luaserialize.h"

#define REG_KEY "luakit.registry.ipc_channel"

#define ROUTES_KEY "luakit.registry.legacy_routes"
static guint64 routes_revision;
static WebKitWebPage *current_page;

WebKitWebPage *
ipc_channel_context_push(WebKitWebPage *page)
{
    WebKitWebPage *previous = current_page;
    current_page = page ? g_object_ref(page) : NULL;
    return previous;
}

void
ipc_channel_context_pop(WebKitWebPage *previous)
{
    g_clear_object(&current_page);
    current_page = previous;
}

void
ipc_recv_lua_routes(ipc_endpoint_t *UNUSED(ipc), const guint8 *msg, guint length)
{
    lua_State *L = common.L;
    int top = lua_gettop(L);
    int n = lua_deserialize_range(L, msg, length);
    if (n == 2 && lua_type(L, top + 1) == LUA_TNUMBER && lua_istable(L, top + 2)) {
        double revision = lua_tonumber(L, top + 1);
        if (revision >= routes_revision && revision <= 9007199254740991.0 && revision == (guint64)revision) {
            routes_revision = revision;
            lua_setfield(L, LUA_REGISTRYINDEX, ROUTES_KEY);
        }
    }
    lua_settop(L, top);
}

static guint
legacy_page_arg(lua_State *L, const char *channel, const char *signal)
{
    int top = lua_gettop(L);
    guint index = 0;
    lua_getfield(L, LUA_REGISTRYINDEX, ROUTES_KEY);
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, channel);
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, signal);
            double value = lua_tonumber(L, -1);
            if (lua_type(L, -1) == LUA_TNUMBER && isfinite(value) && value >= 1 &&
                value <= IPC_VALUE_LIMIT && value == (guint)value) index = value;
        }
    }
    lua_settop(L, top);
    return index;
}

gint
ipc_channel_send(lua_State *L)
{
    ipc_channel_t *channel = luaH_check_ipc_channel(L, 1);
    WebKitWebPage *page;
    if (lua_type(L, 2) == LUA_TSTRING) {
        const char *signal = lua_tostring(L, 2);
        page = current_page;
        guint index = legacy_page_arg(L, channel->name, signal);
        if (index) {
            int arg = index + 2;
            double id = lua_tonumber(L, arg);
            if (lua_type(L, arg) != LUA_TNUMBER || !isfinite(id) || id < 1 ||
                id > 9007199254740991.0 || id != (guint64)id)
                return luaL_error(L, "IPC %s/%s: invalid legacy page ID at payload argument %d",
                        channel->name, signal, index);
            WebKitWebPage *selected = webkit_web_extension_get_page(extension.ext, id);
            if (!selected || (page && page != selected))
                return luaL_error(L, "IPC %s/%s: legacy page ID is closed or conflicts with callback page",
                        channel->name, signal);
            page = selected;
        }
        if (!page) return luaL_error(L,
                "IPC %s/%s: ambiguous legacy send; pass a page or declare legacy_page_arg in the UI policy",
                channel->name, signal);
    } else {
        page = luaH_check_page(L, 2)->page;
        luaL_checkstring(L, 3);
        lua_remove(L, 2);
    }
    ipc_endpoint_t *ipc = web_page_get_endpoint(page);
    if (!ipc) return luaL_error(L, "IPC %s/%s: page transport is not initialized", channel->name, lua_tostring(L, 2));
    lua_pushstring(L, channel->name);
    ipc_send_lua(ipc, IPC_TYPE_lua_ipc, L, 2, lua_gettop(L));
    return 0;
}

static void
channel_recv(lua_State *L, const gchar *arg, guint arglen, gboolean trusted)
{
    gint top = lua_gettop(L);
    int n = trusted ? lua_deserialize_trusted_range(L, (guint8*)arg, arglen) :
        lua_deserialize_range(L, (guint8*)arg, arglen);
    if (n < 3 || lua_type(L, top + 1) != LUA_TSTRING ||
        lua_type(L, top + n - 1) != LUA_TSTRING || lua_type(L, top + n) != LUA_TNUMBER) { lua_settop(L, top); return; }

    /* Remove signal name, module_name and page_id from the stack */
    char *signame = g_strdup(lua_tostring(L, -n));
    lua_remove(L, -n);
    char *module_name = g_strdup(lua_tostring(L, -2));
    guint64 page_id = lua_tointeger(L, -1);
    lua_pop(L, 2);
    n -= 3;

    WebKitWebPage *page = NULL;
    /* Prepend the page object, or nil */
    if (page_id) {
        page = webkit_web_extension_get_page(extension.ext, page_id);
        if (!page) goto done;
        luaH_page_from_web_page(L, page);
    } else
        lua_pushnil(L);
    lua_insert(L, -n-1);
    n++;

    /* Push the right module object onto the stack */
    lua_pushstring(L, REG_KEY);
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_pushstring(L, module_name);
    lua_rawget(L, -2);
    lua_remove(L, -2);

    /* Move the module before arguments, and emit signal */
    if (!lua_isnil(L, -1)) {
        lua_insert(L, -n-1);
        WebKitWebPage *previous = ipc_channel_context_push(page);
        luaH_object_emit_signal(L, -n-1, signame, n, 0);
        ipc_channel_context_pop(previous);
    }
done:
    g_free(signame);
    g_free(module_name);
    lua_settop(L, top);
}

void
ipc_channel_recv(lua_State *L, const gchar *arg, guint length)
{
    channel_recv(L, arg, length, FALSE);
}

void
ipc_channel_recv_trusted(lua_State *L, const gchar *arg, guint length)
{
    channel_recv(L, arg, length, TRUE);
}

// vim: ft=c:et:sw=4:ts=8:sts=4:tw=80
