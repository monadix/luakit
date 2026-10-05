/*
 * tests/security/codec.c - hostile IPC codec tests
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

#include <math.h>
#include <lauxlib.h>
#include <lualib.h>
#include "common/luaserialize.h"

static void
reject(lua_State *L, GVariant *value)
{
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("av"));
    g_variant_builder_add(&b, "v", value);
    GVariant *args = g_variant_ref_sink(g_variant_builder_end(&b));
    int top = lua_gettop(L);
    g_assert_cmpint(lua_deserialize_range(L, g_variant_get_data(args), g_variant_get_size(args)), ==, -1);
    g_assert_cmpint(lua_gettop(L), ==, top);
    g_variant_unref(args);
}

static int
encode_for_test(lua_State *L)
{
    GByteArray *out = g_byte_array_new();
    lua_serialize_range(L, out, 1, 1);
    g_byte_array_unref(out);
    return 0;
}

int
main(void)
{
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    lua_pushcfunction(L, encode_for_test);
    lua_setglobal(L, "encode_for_test");
    g_assert_cmpint(luaL_dostring(L,
        "local t = {}; t.self = t; assert(not pcall(encode_for_test, t)); "
        "assert(not pcall(encode_for_test, function() end)); "
        "assert(not pcall(encode_for_test, io.stdout)); "
        "assert(not pcall(encode_for_test, string.rep('x', 1024 * 1024 + 1)))"), ==, 0);
    g_assert_cmpint(luaL_dostring(L,
        "return setmetatable({ raw = 'value' }, { "
        "__index = function() error('metamethod executed') end, "
        "__pairs = function() error('metamethod executed') end })"), ==, 0);
    GByteArray *snapshot = g_byte_array_new();
    lua_serialize_range(L, snapshot, 1, 1);
    lua_settop(L, 0);
    g_assert_cmpint(lua_deserialize_range(L, snapshot->data, snapshot->len), ==, 1);
    g_assert(!lua_getmetatable(L, 1));
    lua_getfield(L, 1, "raw");
    g_assert_cmpstr(lua_tostring(L, -1), ==, "value");
    lua_settop(L, 0);
    g_byte_array_unref(snapshot);
    lua_pushnil(L);
    lua_pushboolean(L, TRUE);
    lua_pushnumber(L, 1.25);
    lua_pushlstring(L, "a\0b", 3);
    lua_newtable(L);
    lua_pushstring(L, "key");
    lua_pushnumber(L, 42);
    lua_rawset(L, -3);
    lua_pushnumber(L, 3.5);
    lua_pushstring(L, "value");
    lua_rawset(L, -3);
    lua_pushnil(L);
    GByteArray *out = g_byte_array_new();
    lua_serialize_range(L, out, 1, 6);
    lua_settop(L, 0);
    g_assert_cmpint(lua_deserialize_range(L, out->data, out->len), ==, 6);
    g_assert(lua_isnil(L, 1) && lua_isnil(L, 6));
    g_assert(lua_toboolean(L, 2));
    g_assert_cmpfloat(lua_tonumber(L, 3), ==, 1.25);
    g_assert_cmpuint(lua_objlen(L, 4), ==, 3);
    lua_pushnumber(L, 3.5);
    lua_rawget(L, 5);
    g_assert_cmpstr(lua_tostring(L, -1), ==, "value");
    lua_settop(L, 0);
    g_byte_array_unref(out);
    reject(L, g_variant_new_uint64(0xdeadbeef));
    reject(L, g_variant_new_handle(1));
    reject(L, g_variant_new_string("wrong string variant"));
    reject(L, g_variant_new_double(INFINITY));
    reject(L, g_variant_new_double(NAN));
    GVariantBuilder table;
    g_variant_builder_init(&table, G_VARIANT_TYPE("a(vv)"));
    g_variant_builder_add(&table, "(vv)", g_variant_new_boolean(TRUE), g_variant_new_double(1));
    reject(L, g_variant_builder_end(&table));
    g_variant_builder_init(&table, G_VARIANT_TYPE("a(vv)"));
    g_variant_builder_add(&table, "(vv)", g_variant_new_tuple(NULL, 0), g_variant_new_double(1));
    reject(L, g_variant_builder_end(&table));
    GVariant *deep = g_variant_new_double(1);
    for (guint i = 0; i < IPC_DEPTH_LIMIT + 1; i++) {
        g_variant_builder_init(&table, G_VARIANT_TYPE("a(vv)"));
        g_variant_builder_add(&table, "(vv)", g_variant_new_double(1), deep);
        deep = g_variant_builder_end(&table);
    }
    reject(L, deep);
    guint8 *big = g_malloc0(IPC_STRING_LIMIT + 1);
    reject(L, g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, big, IPC_STRING_LIMIT + 1, 1));
    g_free(big);
    GVariantBuilder many;
    g_variant_builder_init(&many, G_VARIANT_TYPE("av"));
    for (guint i = 0; i < IPC_VALUE_LIMIT + 1; i++)
        g_variant_builder_add(&many, "v", g_variant_new_boolean(TRUE));
    GVariant *args = g_variant_ref_sink(g_variant_builder_end(&many));
    g_assert_cmpint(lua_deserialize_range(L, g_variant_get_data(args), g_variant_get_size(args)), ==, -1);
    g_variant_unref(args);
    /* The UI decoder rejects even correctly encoded bytecode tuples. */
    luaL_loadstring(L, "return function() return 123 end");
    lua_call(L, 0, 1);
    out = g_byte_array_new();
    lua_serialize_trusted_range(L, out, 1, 1);
    lua_settop(L, 0);
    g_assert_cmpint(lua_deserialize_range(L, out->data, out->len), ==, -1);
    g_assert_cmpint(lua_gettop(L), ==, 0);
#ifdef LUAKIT_WEB_EXTENSION
    g_assert_cmpint(lua_deserialize_trusted_range(L, out->data, out->len), ==, 1);
    g_assert(lua_isfunction(L, -1));
    lua_call(L, 0, 1);
    g_assert_cmpint(lua_tointeger(L, -1), ==, 123);
    lua_settop(L, 0);
#endif
    g_byte_array_unref(out);
    /* Random corrupt/truncated av input must never change the Lua stack. */
    for (guint size = 1; size < 256; size++) {
        guint8 fuzz[256];
        for (guint i = 0; i < size; i++) fuzz[i] = g_random_int_range(0, 256);
        int n = lua_deserialize_range(L, fuzz, size);
        if (n >= 0) lua_settop(L, 0);
        g_assert_cmpint(lua_gettop(L), ==, 0);
    }
    g_assert_cmpint(lua_deserialize_range(L, NULL, IPC_MESSAGE_LIMIT + 1), ==, -1);
    lua_close(L);
    g_print("IPC codec tests passed\n");
    return 0;
}

// vim: ft=c:et:sw=4:ts=8:sts=4:tw=80
