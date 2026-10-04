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

#ifndef LUAKIT_COMMON_LUASERIALIZE_H
#define LUAKIT_COMMON_LUASERIALIZE_H

#include <lua.h>
#include <glib.h>

#define IPC_MESSAGE_LIMIT (16 * 1024 * 1024)
#define IPC_STRING_LIMIT (1024 * 1024)
#define IPC_DEPTH_LIMIT 32
#define IPC_VALUE_LIMIT 100000

void lua_serialize_trusted_range(lua_State *, GByteArray *, gint, gint);
#ifdef LUAKIT_WEB_EXTENSION
int lua_deserialize_trusted_range(lua_State *, const guint8 *, guint);
#endif

void lua_serialize_range(lua_State *L, GByteArray *out, gint start, gint end);
int lua_deserialize_range(lua_State *L, const guint8 *in, guint length);

#endif

// vim: ft=c:et:sw=4:ts=8:sts=4:tw=80
