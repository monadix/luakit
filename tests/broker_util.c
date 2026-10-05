/*
 * tests/broker_util.c - hostile broker testing utilities
 *
 * Copyright © 2017 Aidan Holm <aidanholm@gmail.com>
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
#include <libsoup/soup.h>
#include "clib/widget.h"
#include "widgets/webview.h"
#include "common/ipc.h"
#include "common/luaserialize.h"
#include "common/clib/ipc.h"

static int
emit_local(lua_State *L)
{
    luaH_check_ipc_channel(L, 1);
    const char *name = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    guint count = lua_objlen(L, 3);
    luaL_checkstack(L, count + 2, "too many test arguments");
    for (guint i = 1; i <= count; i++) lua_rawgeti(L, 3, i);
    luaH_object_emit_signal(L, 1, name, count, 0);
    return 0;
}

static GVariant *
arguments(lua_State *L, int idx)
{
    lua_getfield(L, idx, "n");
    int count = lua_isnumber(L, -1) ? lua_tointeger(L, -1) : (lua_Integer)lua_objlen(L, idx);
    lua_pop(L, 1);
    luaL_argcheck(L, count >= 0 && count <= 256, idx, "invalid argument count");
    int start = lua_gettop(L) + 1;
    for (int i = 1; i <= count; i++) lua_rawgeti(L, idx, i);
    GByteArray *buf = g_byte_array_new();
    lua_serialize_range(L, buf, start, start + count - 1);
    lua_settop(L, start - 1);
    GBytes *bytes = g_byte_array_free_to_bytes(buf);
    GVariant *args = g_variant_new_from_bytes(G_VARIANT_TYPE("av"), bytes, TRUE);
    g_bytes_unref(bytes);
    return args;
}

static int
inject(lua_State *L)
{
    widget_t *w = luaH_checkwebview(L, 1);
    ipc_endpoint_t *ipc = webview_get_endpoint(w);
    const char *op = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    int offset = luaL_optint(L, 4, 0);
    const char *mode = luaL_optstring(L, 5, "data");
    GVariant *args;
    if (!strcmp(mode, "wrong")) args = g_variant_new_string("unexpected type");
    else if (!strcmp(mode, "pointer")) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("av"));
        g_variant_builder_add(&b, "v", g_variant_new_uint64(0xdeadbeef));
        args = g_variant_builder_end(&b);
    } else if (!strcmp(mode, "raw")) {
        args = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, NULL, 0, 1);
    } else args = arguments(L, 3);
    GVariant *params = !strcmp(mode, "envelope") ? g_variant_new_string("bad envelope") :
        g_variant_new("(usttv)", 1u, op, (guint64)1, ipc->generation + offset, args);
    if (!strcmp(mode, "envelope")) g_variant_unref(g_variant_ref_sink(args));
    WebKitUserMessage *message = webkit_user_message_new("luakit-ipc-v1", params);
    g_object_ref_sink(message);
    gboolean held = ipc_endpoint_incref(ipc);
    int top = lua_gettop(L);
    if (held) {
        ipc_receive(ipc, message);
        ipc_endpoint_decref(ipc);
    }
    g_object_unref(message);
    g_assert_cmpint(lua_gettop(L), ==, top);
    return 0;
}

static gboolean
respond_dialog(gpointer data)
{
    int response = GPOINTER_TO_INT(data);
    GList *windows = gtk_window_list_toplevels();
    for (GList *p = windows; p; p = p->next) {
        if (GTK_IS_MESSAGE_DIALOG(p->data) &&
            !g_strcmp0(gtk_window_get_title(p->data), "Luakit confirmation"))
            gtk_dialog_response(p->data, response);
    }
    g_list_free(windows);
    return G_SOURCE_REMOVE;
}

static int
confirm_response(lua_State *L)
{
    int response = lua_toboolean(L, 1) ? GTK_RESPONSE_ACCEPT : GTK_RESPONSE_CANCEL;
    g_timeout_add(100, respond_dialog, GINT_TO_POINTER(response));
    return 0;
}

static gboolean
navigate_view(gpointer view)
{
    webkit_web_view_load_uri(view, "about:blank");
    g_object_unref(view);
    return G_SOURCE_REMOVE;
}

static int
confirm_navigation(lua_State *L)
{
    widget_t *w = luaH_checkwebview(L, 1);
    g_timeout_add(10, navigate_view, g_object_ref(w->widget));
    g_timeout_add(200, respond_dialog, GINT_TO_POINTER(GTK_RESPONSE_ACCEPT));
    return 0;
}

static int
request_navigation(lua_State *L)
{
    widget_t *w = luaH_checkwebview(L, 1);
    webkit_web_view_load_uri(WEBKIT_WEB_VIEW(w->widget), luaL_checkstring(L, 2));
    return 0;
}

static int
encoded_size(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    GVariant *args = g_variant_ref_sink(arguments(L, 1));
    lua_pushinteger(L, g_variant_get_size(args));
    g_variant_unref(args);
    return 1;
}

static SoupServer *test_server;

static void
http_response(SoupServer *UNUSED(server), SoupServerMessage *message, const char *UNUSED(path),
        GHashTable *UNUSED(query), gpointer UNUSED(data))
{
    const char *body = "<html><title>Renderer test</title><body>local page</body></html>";
    soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
    soup_server_message_set_response(message, "text/html", SOUP_MEMORY_COPY, body, strlen(body));
}

static int
http_server_start(lua_State *L)
{
    g_assert(!test_server);
    test_server = soup_server_new(NULL, NULL);
    soup_server_add_handler(test_server, NULL, http_response, NULL, NULL);
    GError *error = NULL;
    if (!soup_server_listen_local(test_server, 0, 0, &error)) {
        char *message = g_strdup(error->message);
        g_clear_error(&error);
        g_clear_object(&test_server);
        lua_pushstring(L, message);
        g_free(message);
        return lua_error(L);
    }
    GSList *uris = soup_server_get_uris(test_server);
    lua_pushinteger(L, g_uri_get_port(uris->data));
    g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
    return 1;
}

static int
http_server_stop(lua_State *UNUSED(L))
{
    soup_server_disconnect(test_server);
    g_clear_object(&test_server);
    return 0;
}

static int
process_swap_enabled(lua_State *L)
{
    widget_t *w = luaH_checkwebview(L, 1);
    gboolean enabled = FALSE;
    g_object_get(webkit_web_view_get_context(WEBKIT_WEB_VIEW(w->widget)),
            "process-swap-on-cross-site-navigation-enabled", &enabled, NULL);
    lua_pushboolean(L, enabled);
    return 1;
}

static int
endpoint_generation(lua_State *L)
{
    lua_pushnumber(L, webview_get_endpoint(luaH_checkwebview(L, 1))->generation);
    return 1;
}

static int
endpoint_queue_size(lua_State *L)
{
    lua_pushinteger(L, g_queue_get_length(webview_get_endpoint(luaH_checkwebview(L, 1))->queue));
    return 1;
}

#define FINALIZER_OBJECT "tests.broker_util.finalizer"

static int
finalize_signal_object(lua_State *L)
{
    lua_object_t *obj = luaL_checkudata(L, 1, FINALIZER_OBJECT);
    int main_top = lua_gettop(common.L);
    if (obj->signals) {
        luaH_object_gc(L);
        obj->signals = NULL;
    }
    gboolean unchanged = lua_gettop(common.L) == main_top;
    /* Keep an old implementation's stack leak contained within this fixture. */
    if (L != common.L) lua_settop(common.L, main_top);
    lua_getfenv(L, 1);
    lua_pushnil(L);
    gboolean empty = !lua_next(L, -2);
    lua_pushboolean(L, empty);
    lua_pushboolean(L, unchanged);
    return 2;
}

static int
new_signal_object(lua_State *L)
{
    luaH_checkfunction(L, 1);
    luaH_checkfunction(L, 2);
    lua_settop(L, 2);
    lua_object_t *obj = lua_newuserdata(L, sizeof(*obj));
    obj->signals = signal_new();
    luaL_getmetatable(L, FINALIZER_OBJECT);
    lua_setmetatable(L, -2);
    lua_newtable(L);
    lua_newtable(L);
    lua_setmetatable(L, -2);
    lua_setfenv(L, -2);
    for (int i = 1; i <= 2; i++) {
        lua_pushvalue(L, i);
        luaH_object_add_signal(L, 3, i == 1 ? "first" : "second", -1);
    }
    return 1;
}

static int
terminate_process(lua_State *L)
{
    widget_t *w = luaH_checkwebview(L, 1);
    webkit_web_view_terminate_web_process(WEBKIT_WEB_VIEW(w->widget));
    return 0;
}

int
luaopen_tests_broker_util(lua_State *L)
{
    static const struct luaL_Reg funcs[] = {
        { "emit_local", emit_local },
        { "inject", inject },
        { "encoded_size", encoded_size },
        { "http_server_start", http_server_start },
        { "http_server_stop", http_server_stop },
        { "endpoint_generation", endpoint_generation },
        { "endpoint_queue_size", endpoint_queue_size },
        { "new_signal_object", new_signal_object },
        { "finalize_signal_object", finalize_signal_object },
        { "process_swap_enabled", process_swap_enabled },
        { "terminate_process", terminate_process },
        { "request_navigation", request_navigation },
        { "confirm_response", confirm_response },
        { "confirm_navigation", confirm_navigation },
        { NULL, NULL },
    };
    luaL_newmetatable(L, FINALIZER_OBJECT);
    lua_pushcfunction(L, finalize_signal_object);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    luaL_openlib(L, "tests.broker_util", funcs, 0);
    return 1;
}

// vim: ft=c:et:sw=4:ts=8:sts=4:tw=80
