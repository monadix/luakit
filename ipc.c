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

#include "globalconf.h"
#include "ipc.h"
#include "clib/web_module.h"
#include "clib/widget.h"
#include "common/luaserialize.h"
#include "common/clib/ipc.h"
#include "web_context.h"
#include "widgets/webview.h"

void webview_scroll_recv(void *, const ipc_scroll_t *);
void run_javascript_finished(ipc_endpoint_t *, const guint8 *, guint);

IPC_NO_HANDLER(lua_require_module)
IPC_NO_HANDLER(lua_trusted)
IPC_NO_HANDLER(lua_routes)
IPC_NO_HANDLER(extension_init)
IPC_NO_HANDLER(crash)

void
ipc_recv_lua_ipc(ipc_endpoint_t *ipc, const ipc_lua_ipc_t *msg, guint length)
{
    ipc_channel_recv_web(common.L, ipc, msg->arg, length);
}

void
ipc_recv_scroll(ipc_endpoint_t *ipc, const guint8 *msg, guint length)
{
    lua_State *L = common.L;
    int top = lua_gettop(L);
    int n = lua_deserialize_range(L, msg, length);
    if (ipc->owner && n == 3 && lua_type(L, -3) == LUA_TNUMBER &&
        lua_type(L, -2) == LUA_TNUMBER && lua_type(L, -1) == LUA_TNUMBER) {
        double h = lua_tonumber(L, -3), v = lua_tonumber(L, -2), subtype = lua_tonumber(L, -1);
        if (h >= 0 && h <= G_MAXINT && v >= 0 && v <= G_MAXINT &&
            h == (gint)h && v == (gint)v && subtype >= 0 && subtype <= 2 && subtype == (gint)subtype) {
            widget_t *w = ipc->owner;
            ipc_scroll_t scroll = { .h = h, .v = v, .subtype = subtype,
                .page_id = webkit_web_view_get_page_id(WEBKIT_WEB_VIEW(w->widget)) };
            webview_scroll_recv(w, &scroll);
        }
    }
    lua_settop(L, top);
}

void
ipc_recv_eval_js(ipc_endpoint_t *ipc, const guint8 *msg, guint length)
{
    run_javascript_finished(ipc, msg, length);
}

void
ipc_recv_page_created(ipc_endpoint_t *ipc, const void *UNUSED(msg), guint length)
{
    if (!ipc->owner || length != 0) return;
    /* Initialize the page's generation before releasing queued UI operations. */
    ipc->status = IPC_ENDPOINT_CONNECTED;
    /* Include requests made while this renderer was starting. require caches
     * each module, so already-loaded modules do not duplicate handlers. */
    ipc_channel_send_routes(common.L, ipc);
    web_module_load_modules_on_endpoint(ipc);
    if (ipc->generation > 0) {
        ipc_header_t header = { .type = IPC_TYPE_extension_init };
        ipc_send(ipc, &header, NULL);
    }
    webview_connect_to_endpoint(ipc->owner, ipc);
}

static gboolean
view_message_cb(WebKitWebView *UNUSED(view), WebKitUserMessage *message, widget_t *w)
{
    ipc_endpoint_t *ipc = webview_get_endpoint(w);
    if (!ipc_endpoint_incref(ipc)) return TRUE;
    gboolean handled = ipc_receive(ipc, message);
    ipc_endpoint_decref(ipc);
    return handled;
}

void
ipc_bind_webview(widget_t *w)
{
    ipc_endpoint_bind(webview_get_endpoint(w), G_OBJECT(w->widget), w);
    g_signal_connect(w->widget, "user-message-received", G_CALLBACK(view_message_cb), w);
}

static gboolean
context_message_cb(WebKitWebContext *UNUSED(context), WebKitUserMessage *message, ipc_endpoint_t *ipc)
{
    return ipc_receive(ipc, message);
}

static void
initialize_web_extensions_cb(WebKitWebContext *context, gpointer UNUSED(data))
{
    lua_State *L = common.L;
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "path");
    lua_getfield(L, -2, "cpath");
    GVariant *modules = web_module_get_names();
    GVariant *payload = g_variant_new("(ss@as)", lua_tostring(L, -2), lua_tostring(L, -1), modules);
    lua_pop(L, 3);
    webkit_web_context_set_web_extensions_initialization_user_data(context, payload);
}

void
ipc_init(void)
{
    ipc_endpoint_t *context = ipc_endpoint_new("context");
    context->status = IPC_ENDPOINT_CONNECTED;
    g_signal_connect(web_context_get(), "user-message-received", G_CALLBACK(context_message_cb), context);
    g_signal_connect(web_context_get(), "initialize-web-extensions", G_CALLBACK(initialize_web_extensions_cb), NULL);
}

// vim: ft=c:et:sw=4:ts=8:sts=4:tw=80
