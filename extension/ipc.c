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

#include <jsc/jsc.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "extension/extension.h"
#include "extension/clib/luakit.h"
#include "extension/ipc.h"
#include "extension/scroll.h"
#include "common/util.h"
#include "common/luajs.h"
#include "common/luaserialize.h"
#include "common/clib/ipc.h"


IPC_NO_HANDLER(page_created)
IPC_NO_HANDLER(log)

void
ipc_recv_lua_require_module(ipc_endpoint_t *UNUSED(ipc), const void *msg, guint length)
{
    const char *module_name = msg;
    if (!length || length > 256 || module_name[length - 1] != 0 ||
        strlen(module_name) != length - 1) return;

    lua_pushstring(common.L, module_name);
    lua_getglobal(common.L, "require");
    luaH_dofunction(common.L, 1, 0);
}

void
ipc_recv_lua_ipc(ipc_endpoint_t *UNUSED(ipc), const void *msg, guint length)
{
    ipc_channel_recv(common.L, msg, length);
}

void
ipc_recv_extension_init(ipc_endpoint_t *ipc, gpointer UNUSED(msg), guint UNUSED(length))
{
    luakit_lib_emit_page_created(common.L, ipc);
}

void
ipc_recv_scroll(ipc_endpoint_t *UNUSED(ipc), const guint8 *msg, guint length)
{
    lua_State *L = common.L;
    gint n = lua_deserialize_range(L, msg, length);
    if (n != 3) return;

    guint64 page_id = lua_tointeger(L, -3);
    gint scroll_x = lua_tointeger(L, -2);
    gint scroll_y = lua_tointeger(L, -1);

    web_scroll_to(page_id, scroll_x, scroll_y);

    lua_pop(L, 3);
}

void
ipc_recv_eval_js(ipc_endpoint_t *ipc, const guint8 *msg, guint length)
{
    lua_State *L = common.L;
    gint top = lua_gettop(L);
    gint n = lua_deserialize_range(L, msg, length);
    if (n != 5) { lua_settop(L, top); return; }

    gboolean no_return = lua_toboolean(L, -5);
    const gchar *script = lua_tostring(L, -4);
    const gchar *source = lua_tostring(L, -3);
    guint64 page_id = lua_tointeger(L, -2);
    /* One-shot UI request ID is index -1. */

    WebKitWebPage *page = webkit_web_extension_get_page(extension.ext, page_id);
    if (!page) {
        /* Notify UI to free callback ref */
        ipc_send_lua(ipc, IPC_TYPE_eval_js, L, -2, -1);
        lua_settop(L, top);
        return;
    }

    WebKitFrame *frame = webkit_web_page_get_main_frame(page);
    WebKitScriptWorld *world = webkit_script_world_get_default();
    JSCContext *ctx = webkit_frame_get_js_context_for_script_world(frame, world);
    n = luajs_eval_js(L, ctx, script, source, 1, no_return);
    g_object_unref(ctx);
    /* Send [page_id, cb, ret] or [page_id, cb, nil, error] */
    ipc_send_lua(ipc, IPC_TYPE_eval_js, L, -n-2, -1);
    lua_settop(L, top);
}

void
ipc_recv_crash(ipc_endpoint_t *UNUSED(ipc), const guint8 *UNUSED(msg), guint UNUSED(length))
{
    raise(SIGKILL);
}

static gboolean
page_message_cb(WebKitWebPage *UNUSED(page), WebKitUserMessage *message, ipc_endpoint_t *ipc)
{
    return ipc_receive(ipc, message);
}

static void
page_destroy_cb(gpointer data, GObject *UNUSED(page))
{
    ipc_endpoint_decref(data);
}

ipc_endpoint_t *
web_page_get_endpoint(WebKitWebPage *page)
{
    return g_object_get_data(G_OBJECT(page), "luakit-ipc");
}

static void
web_page_created_cb(WebKitWebExtension *UNUSED(ext), WebKitWebPage *page, gpointer UNUSED(data))
{
    ipc_endpoint_t *ipc = ipc_endpoint_new("WebPage");
    ipc_endpoint_bind(ipc, G_OBJECT(page), NULL);
    ipc->generation = 0;
    ipc->status = IPC_ENDPOINT_CONNECTED;
    g_object_set_data(G_OBJECT(page), "luakit-ipc", ipc);
    g_object_weak_ref(G_OBJECT(page), page_destroy_cb, ipc);
    g_signal_connect(page, "user-message-received", G_CALLBACK(page_message_cb), ipc);
    ipc_header_t header = { .type = IPC_TYPE_page_created };
    ipc_send(ipc, &header, NULL);
}

static gboolean
extension_message_cb(WebKitWebExtension *UNUSED(ext), WebKitUserMessage *message, gpointer UNUSED(data))
{
    return ipc_receive(extension.ipc, message);
}

void
web_extension_connect(void)
{
    extension.ipc->status = IPC_ENDPOINT_CONNECTED;
    g_signal_connect(extension.ext, "user-message-received", G_CALLBACK(extension_message_cb), NULL);
    g_signal_connect(extension.ext, "page-created", G_CALLBACK(web_page_created_cb), NULL);
}

void
ipc_recv_lua_trusted(ipc_endpoint_t *UNUSED(ipc), const guint8 *msg, guint length)
{
    ipc_channel_recv_trusted(common.L, (const gchar *)msg, length);
}

// vim: ft=c:et:sw=4:ts=8:sts=4:tw=80
