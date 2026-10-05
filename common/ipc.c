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

#include "common/lualib.h"
#include "common/luaserialize.h"
#include "common/ipc.h"
#ifdef LUAKIT_WEB_EXTENSION
#include "extension/extension.h"
#else
#include "web_context.h"
#endif

#define X(name) void ipc_recv_##name(ipc_endpoint_t *, const void *, guint);
IPC_TYPES
#undef X

static void
send_message(ipc_endpoint_t *ipc, WebKitUserMessage *message)
{
#ifdef LUAKIT_WEB_EXTENSION
    if (ipc->target && WEBKIT_IS_WEB_PAGE(ipc->target))
        webkit_web_page_send_message_to_view(WEBKIT_WEB_PAGE(ipc->target), message, NULL, NULL, NULL);
    else
        webkit_web_extension_send_message_to_context(extension.ext, message, NULL, NULL, NULL);
#else
    if (ipc->target)
        webkit_web_view_send_message_to_page(WEBKIT_WEB_VIEW(ipc->target), message, NULL, NULL, NULL);
    else
        webkit_web_context_send_message_to_all_extensions(web_context_get(), message);
#endif
}

void
ipc_send_variant(ipc_endpoint_t *ipc, ipc_type_t type, GVariant *args)
{
    args = g_variant_ref_sink(args);
    if (!ipc || ipc->status == IPC_ENDPOINT_FREED ||
        g_variant_get_size(args) > IPC_MESSAGE_LIMIT) {
        g_variant_unref(args);
        return;
    }
    GVariant *params = g_variant_new("(usttv)", 1u, ipc_type_name(type),
            ++ipc->next_request, ipc->generation, args);
    g_variant_unref(args);
    WebKitUserMessage *message = webkit_user_message_new("luakit-ipc-v1", params);
    g_object_ref_sink(message);
    if (ipc->status == IPC_ENDPOINT_CONNECTED)
        send_message(ipc, message);
    else if (g_queue_get_length(ipc->queue) < 256)
        g_queue_push_tail(ipc->queue, g_object_ref(message));
    g_object_unref(message);
}

void
ipc_send(ipc_endpoint_t *ipc, const ipc_header_t *header, const void *data)
{
    ipc_send_variant(ipc, header->type, g_variant_new_fixed_array(
                G_VARIANT_TYPE_BYTE, data, header->length, 1));
}

static void
send_lua(ipc_endpoint_t *ipc, ipc_type_t type, lua_State *L, int start, int end, gboolean trusted)
{
    GByteArray *buf = g_byte_array_new();
    if (trusted) lua_serialize_trusted_range(L, buf, start, end);
    else lua_serialize_range(L, buf, start, end);
    GBytes *bytes = g_byte_array_free_to_bytes(buf);
    GVariant *args = g_variant_new_from_bytes(G_VARIANT_TYPE("av"), bytes, TRUE);
    g_bytes_unref(bytes);
    ipc_send_variant(ipc, type, args);
}

void
ipc_send_lua(ipc_endpoint_t *ipc, ipc_type_t type, lua_State *L, int start, int end)
{
    send_lua(ipc, type, L, start, end, FALSE);
}

void
ipc_send_lua_trusted(ipc_endpoint_t *ipc, lua_State *L, int start, int end)
{
    send_lua(ipc, IPC_TYPE_lua_trusted, L, start, end, TRUE);
}

gboolean
ipc_receive(ipc_endpoint_t *ipc, gpointer data)
{
    WebKitUserMessage *message = data;
    if (g_strcmp0(webkit_user_message_get_name(message), "luakit-ipc-v1"))
        return FALSE;
    GVariant *params = webkit_user_message_get_parameters(message);
    if (!params || g_variant_get_size(params) > IPC_MESSAGE_LIMIT ||
        !g_variant_is_of_type(params, G_VARIANT_TYPE("(usttv)")) ||
        !g_variant_is_normal_form(params))
        return TRUE;
    guint version;
    const char *operation;
    guint64 request, generation;
    GVariant *args;
    g_variant_get(params, "(u&sttv)", &version, &operation, &request, &generation, &args);
    ipc_type_t type = 0;
#define X(name) if (!strcmp(operation, #name)) type = IPC_TYPE_##name;
    IPC_TYPES
#undef X
    gboolean valid = version == 1 && request > 0 && type != 0;
#ifdef LUAKIT_WEB_EXTENSION
    /* Only the UI chooses document generations. */
    if (valid && ipc->target && WEBKIT_IS_WEB_PAGE(ipc->target)) {
        if (generation < ipc->generation) valid = FALSE;
        else ipc->generation = generation;
    }
#else
    /* Context traffic is diagnostic-only. It has no originating view. */
    if (!ipc->owner && type != IPC_TYPE_log) valid = FALSE;
    if (type != IPC_TYPE_page_created && generation != ipc->generation) valid = FALSE;
    if (type == IPC_TYPE_page_created && ipc->status == IPC_ENDPOINT_CONNECTED) valid = FALSE;
    if (type == IPC_TYPE_lua_require_module || type == IPC_TYPE_lua_trusted || type == IPC_TYPE_lua_routes ||
        type == IPC_TYPE_extension_init || type == IPC_TYPE_crash) valid = FALSE;
#endif
    gboolean lua = type == IPC_TYPE_lua_ipc || type == IPC_TYPE_lua_trusted || type == IPC_TYPE_lua_routes ||
        type == IPC_TYPE_eval_js || type == IPC_TYPE_log || type == IPC_TYPE_scroll;
    if (!g_variant_is_of_type(args, lua ? G_VARIANT_TYPE("av") : G_VARIANT_TYPE("ay")))
        valid = FALSE;
    if (type == IPC_TYPE_log) {
        gint64 now = g_get_monotonic_time();
        if (now - ipc->log_window > G_USEC_PER_SEC) {
            ipc->log_window = now;
            ipc->log_count = 0;
        }
        if (++ipc->log_count > 100 || g_variant_get_size(args) > 16384)
            valid = FALSE;
    }
    if (valid) {
        ipc->received_request = request;
        const void *payload = g_variant_get_data(args);
        guint length = g_variant_get_size(args);
        /* lua handlers receive the serialized av; raw operations receive ay. */
        switch (type) {
#define X(name) case IPC_TYPE_##name: ipc_recv_##name(ipc, payload, length); break;
            IPC_TYPES
#undef X
          default: break;
        }
    }
    g_variant_unref(args);
    return TRUE;
}

ipc_endpoint_t *
ipc_endpoint_new(const gchar *name)
{
    ipc_endpoint_t *ipc = g_slice_new0(ipc_endpoint_t);
    ipc->name = g_strdup(name);
    ipc->queue = g_queue_new();
    ipc->refcount = 1;
    ipc->generation = 1;
    ipc->routes_revision_sent = G_MAXUINT64;
    return ipc;
}

void
ipc_endpoint_bind(ipc_endpoint_t *ipc, GObject *target, gpointer owner)
{
    ipc->target = target;
    ipc->owner = owner;
}

void
ipc_endpoint_activate(ipc_endpoint_t *ipc)
{
    ipc->status = IPC_ENDPOINT_CONNECTED;
    while (!g_queue_is_empty(ipc->queue)) {
        WebKitUserMessage *message = g_queue_pop_head(ipc->queue);
        send_message(ipc, message);
        g_object_unref(message);
    }
}

void
ipc_endpoint_invalidate(ipc_endpoint_t *ipc)
{
    ipc->generation++;
    ipc->routes_revision_sent = G_MAXUINT64;
    while (!g_queue_is_empty(ipc->queue))
        g_object_unref(g_queue_pop_head(ipc->queue));
}

WARN_UNUSED gboolean
ipc_endpoint_incref(ipc_endpoint_t *ipc)
{
    if (!ipc || ipc->refcount < 1) return FALSE;
    ipc->refcount++;
    return TRUE;
}

void
ipc_endpoint_disconnect(ipc_endpoint_t *ipc)
{
    ipc->status = IPC_ENDPOINT_DISCONNECTED;
    ipc_endpoint_invalidate(ipc);
}

void
ipc_endpoint_decref(ipc_endpoint_t *ipc)
{
    if (!ipc || --ipc->refcount) return;
    ipc_endpoint_invalidate(ipc);
    g_queue_free(ipc->queue);
    g_free(ipc->name);
    g_slice_free(ipc_endpoint_t, ipc);
}

// vim: ft=c:et:sw=4:ts=8:sts=4:tw=80
