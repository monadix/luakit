/*
 * extension/clib/luakit.c - Generic functions for Lua scripts
 *
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

#include "extension/extension.h"
#include "extension/luajs.h"
#include "extension/clib/luakit.h"
#include "extension/clib/page.h"
#include "common/clib/luakit.h"
#include "common/resource.h"
#include "common/signal.h"

#include <glib.h>
#include <gtk/gtk.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>

/* lua luakit class for signals */
static lua_class_t luakit_class;

/* setup luakit module signals */
LUA_CLASS_FUNCS(luakit, luakit_class)

static gint
luaH_luakit_index(lua_State *L)
{
    if (luaH_usemetatable(L, 1, 2))
        return 1;

    const gchar *prop = luaL_checkstring(L, 2);
    luakit_token_t token = l_tokenize(prop);

    switch (token) {
        PI_CASE(WEB_PROCESS_ID, getpid())
        PS_CASE(RESOURCE_PATH, resource_path_get())

        case L_TK_WEBKIT_VERSION:
            lua_pushfstring(L, "%d.%d.%d", WEBKIT_MAJOR_VERSION,
                    WEBKIT_MINOR_VERSION, WEBKIT_MICRO_VERSION);
            return 1;

        default: return 0;
    }
}

static gint
luaH_luakit_newindex(lua_State *L)
{
    if (!lua_isstring(L, 2))
        return 0;
    luakit_token_t token = l_tokenize(lua_tostring(L, 2));

    switch (token) {
        case L_TK_RESOURCE_PATH:
            resource_path_set(luaL_checkstring(L, 3));
            break;
        default:
            return 0;
    }

    return 0;
}

static gint
luaH_luakit_register_function(lua_State *L)
{
    luaL_checkstring(L, 1);
    luaL_checkstring(L, 2);
    if (strlen(lua_tostring(L, 1)) == 0)
        return luaL_error(L, "pattern cannot be empty");
    if (strlen(lua_tostring(L, 2)) == 0)
        return luaL_error(L, "function name cannot be empty");
    luaH_checkfunction(L, 3);

    luaJS_register_function(L);

    return 0;
}

/** Setup luakit module.
 *
 * \param L The Lua VM state.
 */
void
luakit_lib_setup(lua_State *L)
{
    static const struct luaL_Reg luakit_lib[] =
    {
        LUA_CLASS_METHODS(luakit)
        LUAKIT_LIB_COMMON_METHODS
        { "__index",           luaH_luakit_index },
        { "__newindex",        luaH_luakit_newindex },
        { "register_function", luaH_luakit_register_function },
        { NULL,              NULL }
    };

    /* create signals array */
    luakit_class.signals = signal_new();

    /* export luakit lib */
    luaH_openlib(L, "luakit", luakit_lib, luakit_lib);

}

void
luakit_lib_emit_page_created(lua_State *L, ipc_endpoint_t *ipc)
{
    if (!ipc->target || !WEBKIT_IS_WEB_PAGE(ipc->target) || ipc->creation_notified) return;
    /* Every page waits for its own UI-selected generation and module load. */
    ipc->creation_notified = TRUE;
    luaH_page_from_web_page(L, WEBKIT_WEB_PAGE(ipc->target));
    signal_object_emit(L, luakit_class.signals, "page-created", 1, 0);
}

// vim: ft=c:et:sw=4:ts=8:sts=4:tw=80
