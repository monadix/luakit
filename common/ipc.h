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

#ifndef LUAKIT_COMMON_IPC_H
#define LUAKIT_COMMON_IPC_H

#include <glib.h>
#include <glib-object.h>
#include "common/util.h"

#define IPC_TYPES \
    X(lua_require_module) \
    X(lua_ipc) \
    X(lua_trusted) \
    X(lua_routes) \
    X(scroll) \
    X(extension_init) \
    X(eval_js) \
    X(log) \
    X(page_created) \
    X(crash) \

#define X(name) IPC_TYPE_EXPONENT_##name,
typedef enum { IPC_TYPES } _ipc_type_exponent_t;
#undef X

/* Automatically defines all IPC_TYPE_foo as powers of two */
#define X(name) IPC_TYPE_##name = (1 << IPC_TYPE_EXPONENT_##name),
typedef enum { IPC_TYPES } ipc_type_t;
#undef X

#define IPC_TYPE_ANY (-1)
#define IPC_QUEUE_LIMIT 256

/** Internal description of a raw operation; never sent as a wire header. */
typedef struct _ipc_header_t {
    /** The length of the message in bytes, not including the header */
    guint length;
    /** The type of the message, fairly self-explanatory... */
    ipc_type_t type;
} ipc_header_t;

/* Structure of messages for all message types */

typedef struct _ipc_lua_require_module_t {
    gchar module_name[0];
} ipc_lua_require_module_t;

typedef struct _ipc_lua_ipc_t {
    gchar arg[0];
} ipc_lua_ipc_t;

typedef enum {
    IPC_SCROLL_TYPE_docresize,
    IPC_SCROLL_TYPE_winresize,
    IPC_SCROLL_TYPE_scroll
} ipc_scroll_subtype_t;

typedef struct _ipc_scroll_t {
    gint h, v;
    guint64 page_id;
    ipc_scroll_subtype_t subtype;
} ipc_scroll_t;

/* Message names */
static inline const char *
ipc_type_name(ipc_type_t type)
{
    switch (type) {
#define X(name) case IPC_TYPE_##name: return #name;
        IPC_TYPES
#undef X
        default:
            return "UNKNOWN";
    }
}

typedef enum {
    IPC_ENDPOINT_DISCONNECTED,
    IPC_ENDPOINT_CONNECTED,
    IPC_ENDPOINT_FREED,
} ipc_endpoint_status_t;

typedef struct _ipc_endpoint_t {
    gchar *name;
    ipc_endpoint_status_t status;
    GObject *target; /* WebView, WebPage, or WebExtension; borrowed */
    GQueue *queue;
    gint refcount;
    gboolean creation_notified;
    guint64 initialized_page_id; /* UI-observed WebKit page, not a renderer claim */
    guint64 generation;
    guint64 next_request;
    guint64 received_request;
    guint64 routes_revision_sent;
    gpointer owner; /* UI-owned widget; never supplied by the renderer */
    gint64 log_window;
    guint log_count;
    gboolean confirming;
} ipc_endpoint_t;

void ipc_endpoint_bind(ipc_endpoint_t *, GObject *, gpointer);
void ipc_endpoint_activate(ipc_endpoint_t *);
gboolean ipc_endpoint_can_send(ipc_endpoint_t *, guint);
gboolean ipc_receive(ipc_endpoint_t *, gpointer);
void ipc_send_variant(ipc_endpoint_t *, ipc_type_t, GVariant *);
void ipc_send_lua_trusted(ipc_endpoint_t *, lua_State *, gint, gint);
void ipc_endpoint_invalidate(ipc_endpoint_t *);

ipc_endpoint_t *ipc_endpoint_new(const gchar *name);
void ipc_endpoint_disconnect(ipc_endpoint_t *ipc);

WARN_UNUSED gboolean ipc_endpoint_incref(ipc_endpoint_t *ipc);
void ipc_endpoint_decref(ipc_endpoint_t *ipc);

void ipc_send_lua(ipc_endpoint_t *ipc, ipc_type_t type, lua_State *L, gint start, gint end);
void ipc_send(ipc_endpoint_t *ipc, const ipc_header_t *header, const void *data);

#define IPC_NO_HANDLER(type) \
void \
ipc_recv_##type(ipc_endpoint_t *ipc, const gpointer UNUSED(msg), guint UNUSED(length)) \
{ \
    warn("process '%s': should never receive message of type %s", ipc->name, #type); \
} \

#endif

// vim: ft=c:et:sw=4:ts=8:sts=4:tw=80
