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
#include "clib/widget.h"
#include "widgets/webview.h"
#include "common/ipc.h"
#include "common/luaserialize.h"

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
        { "inject", inject },
        { "terminate_process", terminate_process },
        { "request_navigation", request_navigation },
        { "confirm_response", confirm_response },
        { "confirm_navigation", confirm_navigation },
        { NULL, NULL },
    };
    luaL_openlib(L, "tests.broker_util", funcs, 0);
    return 1;
}

// vim: ft=c:et:sw=4:ts=8:sts=4:tw=80
