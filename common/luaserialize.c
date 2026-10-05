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

#include "common/luaserialize.h"
#include "common/lualib.h"
#include <lauxlib.h>

/* av preserves argument count and nil slots. Values are (), b, d, ay or
 * a(vv). Function tuples are accepted only by the trusted extension decoder. */
typedef struct {
    guint count;
    gsize bytes;
    gboolean trusted;
} codec_state_t;

static int
function_writer(lua_State *UNUSED(L), const void *data, size_t size, void *out)
{
    GByteArray *buf = out;
    if (size > IPC_STRING_LIMIT - buf->len)
        return 1;
    g_byte_array_append(buf, data, size);
    return 0;
}

static GVariant *encode_value(lua_State *, int, guint, codec_state_t *);

static GVariant *
encode_value(lua_State *L, int idx, guint depth, codec_state_t *state)
{
    if (depth > IPC_DEPTH_LIMIT || ++state->count > IPC_VALUE_LIMIT || !lua_checkstack(L, 4))
        return NULL;
    idx = luaH_absindex(L, idx);
    switch (lua_type(L, idx)) {
      case LUA_TNIL:
        return g_variant_new_tuple(NULL, 0);
      case LUA_TBOOLEAN:
        return g_variant_new_boolean(lua_toboolean(L, idx));
      case LUA_TNUMBER: {
        double n = lua_tonumber(L, idx);
        return isfinite(n) ? g_variant_new_double(n) : NULL;
      }
      case LUA_TSTRING: {
        size_t len;
        const char *s = lua_tolstring(L, idx, &len);
        if (len > IPC_STRING_LIMIT || (state->bytes += len) > IPC_MESSAGE_LIMIT)
            return NULL;
        return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, s, len, 1);
      }
      case LUA_TTABLE: {
        /* Match historical behavior: snapshot raw entries, never metamethods. */
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a(vv)"));
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            GVariant *key = encode_value(L, -2, depth + 1, state), *value = NULL;
            if (key)
                value = encode_value(L, -1, depth + 1, state);
            if (!key || !value) {
                if (key) g_variant_unref(g_variant_ref_sink(key));
                lua_pop(L, 2);
                g_variant_builder_clear(&b);
                return NULL;
            }
            g_variant_builder_add(&b, "(vv)", key, value);
            lua_pop(L, 1);
        }
        return g_variant_builder_end(&b);
      }
      case LUA_TFUNCTION: {
        if (!state->trusted || lua_iscfunction(L, idx))
            return NULL;
        GByteArray *buf = g_byte_array_new();
        lua_pushvalue(L, idx);
        int status = lua_dump(L, function_writer, buf);
        lua_pop(L, 1);
        if (status || (state->bytes += buf->len) > IPC_MESSAGE_LIMIT) {
            g_byte_array_unref(buf);
            return NULL;
        }
        GVariant *code = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,
                buf->data, buf->len, 1);
        g_byte_array_unref(buf);
        GVariantBuilder ups;
        g_variant_builder_init(&ups, G_VARIANT_TYPE("av"));
        for (int i = 1; lua_getupvalue(L, idx, i); i++) {
            GVariant *v = encode_value(L, -1, depth + 1, state);
            lua_pop(L, 1);
            if (!v) {
                g_variant_unref(g_variant_ref_sink(code));
                g_variant_builder_clear(&ups);
                return NULL;
            }
            g_variant_builder_add(&ups, "v", v);
        }
        return g_variant_new("(@ay@av)", code, g_variant_builder_end(&ups));
      }
      default:
        return NULL;
    }
}

static void
encode_range(lua_State *L, GByteArray *out, int start, int end, gboolean trusted)
{
    codec_state_t state = { .trusted = trusted };
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("av"));
    start = luaH_absindex(L, start);
    end = luaH_absindex(L, end);
    char context[400] = "message";
    if (lua_type(L, start) == LUA_TSTRING) {
        int channel = lua_type(L, end) == LUA_TNUMBER ? end - 1 : end;
        if (channel > start && lua_type(L, channel) == LUA_TSTRING)
            g_snprintf(context, sizeof(context), "%.*s/%.*s", 256, lua_tostring(L, channel),
                    128, lua_tostring(L, start));
    }
    for (int i = start; i <= end; i++) {
        GVariant *v = encode_value(L, i, 0, &state);
        if (!v) {
            g_variant_builder_clear(&b);
            g_byte_array_unref(out);
            luaL_error(L, "IPC %s: unsupported or oversized argument #%d; use bounded raw data%s",
                    context, i - start + 1, trusted ? " or serializable Lua functions" : " (no functions or pointers)");
            return;
        }
        g_variant_builder_add(&b, "v", v);
    }
    GVariant *args = g_variant_ref_sink(g_variant_builder_end(&b));
    gsize size = g_variant_get_size(args);
    if (size > IPC_MESSAGE_LIMIT) {
        g_variant_unref(args);
        g_byte_array_unref(out);
        luaL_error(L, "IPC %s: message exceeds 16 MiB", context);
        return;
    }
    g_byte_array_append(out, g_variant_get_data(args), size);
    g_variant_unref(args);
}

void
lua_serialize_range(lua_State *L, GByteArray *out, int start, int end)
{
    encode_range(L, out, start, end, FALSE);
}

void
lua_serialize_trusted_range(lua_State *L, GByteArray *out, int start, int end)
{
    encode_range(L, out, start, end, TRUE);
}

static gboolean
validate_value(GVariant *v, guint depth, codec_state_t *state, gboolean key)
{
    if (depth > IPC_DEPTH_LIMIT || ++state->count > IPC_VALUE_LIMIT)
        return FALSE;
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_DOUBLE))
        return isfinite(g_variant_get_double(v));
    if (g_variant_is_of_type(v, G_VARIANT_TYPE("ay")))
        return g_variant_n_children(v) <= IPC_STRING_LIMIT;
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN))
        return TRUE;
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_UNIT))
        return !key;
    gboolean table = g_variant_is_of_type(v, G_VARIANT_TYPE("a(vv)"));
    gboolean function = state->trusted && g_variant_is_of_type(v, G_VARIANT_TYPE("(ayav)"));
    if (!table && !function)
        return FALSE;
    GVariant *children = function ? g_variant_get_child_value(v, 1) : g_variant_ref(v);
    if (function) {
        GVariant *code = g_variant_get_child_value(v, 0);
        gboolean ok = g_variant_get_size(code) <= IPC_STRING_LIMIT;
        g_variant_unref(code);
        if (!ok || g_variant_n_children(children) > 255) {
            g_variant_unref(children);
            return FALSE;
        }
    }
    gboolean ok = TRUE;
    for (gsize i = 0; ok && i < g_variant_n_children(children); i++) {
        GVariant *entry = g_variant_get_child_value(children, i);
        for (guint j = 0; ok && j < (table ? 2u : 1u); j++) {
            GVariant *boxed = table ? g_variant_get_child_value(entry, j) : g_variant_ref(entry);
            GVariant *value = g_variant_get_variant(boxed);
            ok = validate_value(value, depth + 1, state, table && j == 0);
            g_variant_unref(value);
            g_variant_unref(boxed);
        }
        g_variant_unref(entry);
    }
    g_variant_unref(children);
    return ok;
}

static gboolean
decode_value(lua_State *L, GVariant *v)
{
    if (!lua_checkstack(L, 4))
        return FALSE;
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_UNIT))
        lua_pushnil(L);
    else if (g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN))
        lua_pushboolean(L, g_variant_get_boolean(v));
    else if (g_variant_is_of_type(v, G_VARIANT_TYPE_DOUBLE))
        lua_pushnumber(L, g_variant_get_double(v));
    else if (g_variant_is_of_type(v, G_VARIANT_TYPE("ay"))) {
        gsize size;
        const char *data = g_variant_get_fixed_array(v, &size, 1);
        lua_pushlstring(L, data ? data : "", size);
    } else if (g_variant_is_of_type(v, G_VARIANT_TYPE("a(vv)"))) {
        lua_newtable(L);
        for (gsize i = 0; i < g_variant_n_children(v); i++) {
            GVariant *entry = g_variant_get_child_value(v, i);
            gboolean ok = TRUE;
            for (guint j = 0; ok && j < 2; j++) {
                GVariant *boxed = g_variant_get_child_value(entry, j);
                GVariant *value = g_variant_get_variant(boxed);
                ok = decode_value(L, value);
                g_variant_unref(value);
                g_variant_unref(boxed);
            }
            g_variant_unref(entry);
            if (!ok) return FALSE;
            lua_rawset(L, -3);
        }
    } else {
#ifdef LUAKIT_WEB_EXTENSION
        GVariant *code = g_variant_get_child_value(v, 0);
        gsize size;
        const char *data = g_variant_get_fixed_array(code, &size, 1);
        int status = luaL_loadbuffer(L, data, size, "trusted UI function");
        g_variant_unref(code);
        if (status) return FALSE;
        GVariant *ups = g_variant_get_child_value(v, 1);
        for (gsize i = 0; i < g_variant_n_children(ups); i++) {
            GVariant *boxed = g_variant_get_child_value(ups, i);
            GVariant *value = g_variant_get_variant(boxed);
            gboolean ok = decode_value(L, value);
            g_variant_unref(value);
            g_variant_unref(boxed);
            if (!ok) { g_variant_unref(ups); return FALSE; }
            if (!lua_setupvalue(L, -2, i + 1)) {
                lua_pop(L, 1);
                g_variant_unref(ups);
                return FALSE;
            }
        }
        g_variant_unref(ups);
#else
        return FALSE;
#endif
    }
    return TRUE;
}

static int
decode_range(lua_State *L, const guint8 *in, guint length, gboolean trusted)
{
    int top = lua_gettop(L);
    if (length > IPC_MESSAGE_LIMIT)
        return -1;
    GBytes *bytes = g_bytes_new(in, length);
    GVariant *args = g_variant_ref_sink(g_variant_new_from_bytes(G_VARIANT_TYPE("av"), bytes, FALSE));
    g_bytes_unref(bytes);
    codec_state_t state = { .trusted = trusted };
    gboolean ok = g_variant_is_normal_form(args);
    gsize count = ok ? g_variant_n_children(args) : 0;
    if (count > IPC_VALUE_LIMIT || !lua_checkstack(L, count + 4))
        ok = FALSE;
    /* Validate the complete tree before allocating Lua tables or dispatching. */
    for (gsize i = 0; ok && i < count; i++) {
        GVariant *boxed = g_variant_get_child_value(args, i);
        GVariant *value = g_variant_get_variant(boxed);
        ok = validate_value(value, 0, &state, FALSE);
        g_variant_unref(value);
        g_variant_unref(boxed);
    }
    for (gsize i = 0; ok && i < count; i++) {
        GVariant *boxed = g_variant_get_child_value(args, i);
        GVariant *value = g_variant_get_variant(boxed);
        ok = decode_value(L, value);
        g_variant_unref(value);
        g_variant_unref(boxed);
    }
    g_variant_unref(args);
    if (!ok) lua_settop(L, top);
    return ok ? (int)count : -1;
}

int
lua_deserialize_range(lua_State *L, const guint8 *in, guint length)
{
    return decode_range(L, in, length, FALSE);
}

#ifdef LUAKIT_WEB_EXTENSION
int
lua_deserialize_trusted_range(lua_State *L, const guint8 *in, guint length)
{
    return decode_range(L, in, length, TRUE);
}
#endif

// vim: ft=c:et:sw=4:ts=8:sts=4:tw=80
