/*
 * widgets/webview/javascript.c - webkit webview javascript functions
 *
 * Copyright © 2010-2012 Mason Larobina <mason.larobina@gmail.com>
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

#include <stdlib.h>

#include "common/luajs.h"
#include "common/ipc.h"
#include "common/luaserialize.h"

typedef struct {
    guint64 id;
    ipc_endpoint_t *ipc;
    guint64 generation;
    gpointer callback;
} javascript_request_t;

static GHashTable *javascript_requests;
static guint64 javascript_next_id;

static void
run_javascript_cancel(ipc_endpoint_t *ipc)
{
    if (!javascript_requests) return;
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, javascript_requests);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        javascript_request_t *request = value;
        if (request->ipc == ipc) {
            luaH_object_unref(common.L, request->callback);
            g_hash_table_iter_remove(&iter);
        }
    }
}

void
run_javascript_finished(ipc_endpoint_t *ipc, const guint8 *msg, guint length)
{
    lua_State *L = common.L;
    int top = lua_gettop(L);
    int n = lua_deserialize_range(L, msg, length);
    if (n < 2 || n > 4 || lua_type(L, top + 1) != LUA_TNUMBER ||
        lua_type(L, top + 2) != LUA_TNUMBER || !javascript_requests) goto done;
    double number = lua_tonumber(L, top + 2);
    if (number < 1 || number > 9007199254740991.0 || number != (guint64)number) goto done;
    guint64 id = number;
    javascript_request_t *request = g_hash_table_lookup(javascript_requests, &id);
    if (!request || request->ipc != ipc || request->generation != ipc->generation || !ipc->owner)
        goto done;
    widget_t *w = ipc->owner;
    if (lua_tonumber(L, top + 1) != webkit_web_view_get_page_id(WEBKIT_WEB_VIEW(w->widget))) goto done;
    if (n == 4 && (lua_type(L, top + 3) != LUA_TNIL || lua_type(L, top + 4) != LUA_TSTRING)) goto done;
    gpointer callback = request->callback;
    /* Consume before callback invocation: reentrant and duplicate replies fail. */
    g_hash_table_remove(javascript_requests, &id);
    if (n >= 3) {
        luaH_object_push(L, callback);
        luaH_dofunction(L, n - 2, 0);
    }
    luaH_object_unref(L, callback);
done:
    lua_settop(L, top);
}

static guint64
run_javascript_register(lua_State *L, ipc_endpoint_t *ipc, gpointer callback)
{
    if (!callback) return 0;
    if (!javascript_requests)
        javascript_requests = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, g_free);
    guint count = 0;
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, javascript_requests);
    while (g_hash_table_iter_next(&iter, NULL, &value))
        if (((javascript_request_t *)value)->ipc == ipc) count++;
    if (count >= 256 || javascript_next_id >= 9007199254740991ULL) {
        luaH_object_unref(L, callback);
        luaL_error(L, "too many pending JavaScript callbacks");
        return 0;
    }
    javascript_request_t *request = g_new0(javascript_request_t, 1);
    request->id = ++javascript_next_id;
    request->ipc = ipc;
    request->generation = ipc->generation;
    request->callback = callback;
    g_hash_table_insert(javascript_requests, &request->id, request);
    return request->id;
}

static gint
luaH_webview_eval_js(lua_State *L)
{
    gpointer cb = NULL;
    webview_data_t *d = luaH_checkwvdata(L, 1);
    size_t script_len;
    const gchar *script = luaL_checklstring(L, 2, &script_len);
    if (script_len > IPC_STRING_LIMIT) return luaL_error(L, "JavaScript source too large");
    const gchar *usr_source = NULL;
    gchar *source = NULL;
    bool no_return = false;

    luaH_checktable(L, 3);

    gint top = lua_gettop(L);
    /* source filename to use in error messages and webinspector */
    if (luaH_rawfield(L, 3, "source") && lua_isstring(L, -1))
        usr_source = lua_tostring(L, -1);
    if (usr_source && strlen(usr_source) > IPC_STRING_LIMIT)
        return luaL_error(L, "JavaScript source name too large");
    if (luaH_rawfield(L, 3, "no_return"))
        no_return = lua_toboolean(L, -1);
    if (luaH_rawfield(L, 3, "callback")) {
        luaH_checkfunction(L, -1);
        cb = luaH_object_ref(L, -1);
    }
    lua_settop(L, top);

    if (!usr_source)
        source = luaH_callerinfo(L);

    lua_pushboolean(L, no_return);
    lua_pushstring(L, script);
    lua_pushstring(L, usr_source ? usr_source : source);
    g_free(source);
    lua_pushinteger(L, webkit_web_view_get_page_id(d->view));
    lua_pushnumber(L, run_javascript_register(L, d->ipc, cb));
    ipc_send_lua(d->ipc, IPC_TYPE_eval_js, L, -5, -1);
    lua_pop(L, 5);

    return FALSE;
}

// vim: ft=c:et:sw=4:ts=8:sts=4:tw=80
