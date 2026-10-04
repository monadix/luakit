/*
 * web_context.c - WebKit web context setup and handling
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

#include "globalconf.h"
#include "common/log.h"
#include "web_context.h"

#include <webkit2/webkit2.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "common/common.h"
#include "common/util.h"
#include "common/luautil.h"

/** WebKit context common to all web views */
static WebKitWebContext *web_context;
/** WebKit process count; default to unlimited */
static guint process_limit = 0;
/** Whether the web context startup function has been run */
static gboolean web_context_started = FALSE;

/** Defined in widgets/webview/downloads.c */
gboolean download_start_cb(WebKitWebContext *, WebKitDownload *, gpointer);

WebKitWebContext *
web_context_get(void)
{
    g_assert(web_context);
    return web_context;
}

guint
web_context_process_limit_get(void)
{
    return process_limit;
}

gboolean
web_context_process_limit_set(guint limit)
{
    if (web_context_started)
        return FALSE;
    process_limit = limit;
    return TRUE;
}

static gboolean
path_contains(const char *parent, const char *child)
{
    gsize len = strlen(parent);
    return !strncmp(parent, child, len) && (child[len] == '/' || child[len] == 0 || len == 1);
}

static gboolean
path_prohibited(const char *path)
{
    const char *protected[] = { g_get_home_dir(), g_get_tmp_dir(),
        globalconf.data_dir, globalconf.cache_dir, NULL };
    for (guint i = 0; protected[i]; i++) {
        if (path_contains(path, protected[i])) return TRUE;
        char *resolved = realpath(protected[i], NULL);
        gboolean denied = resolved && path_contains(path, resolved);
        free(resolved);
        if (denied) return TRUE;
    }
    if (!strcmp(path, "/etc") || !strcmp(path, "/usr") || !strcmp(path, "/var")) return TRUE;
    const char *special[] = { "/proc", "/sys", "/dev", NULL };
    for (guint i = 0; special[i]; i++)
        if (path_contains(special[i], path)) return TRUE;
    return FALSE;
}

gboolean
web_context_add_path_to_sandbox(const char *path, gboolean read_only, gchar **reason)
{
    if (web_context_started) {
        *reason = g_strdup("sandbox paths must be registered before the first WebView");
        return FALSE;
    }
    if (!g_path_is_absolute(path)) {
        *reason = g_strdup("sandbox path must be absolute");
        return FALSE;
    }
    char *canonical = g_canonicalize_filename(path, NULL);
    char *resolved = realpath(canonical, NULL);
    if (!resolved || path_prohibited(canonical) || path_prohibited(resolved)) {
        *reason = g_strdup("sandbox path does not exist or is prohibited");
        g_free(canonical);
        free(resolved);
        return FALSE;
    }
    webkit_web_context_add_path_to_sandbox(web_context, resolved, read_only);
    if (strcmp(canonical, resolved))
        webkit_web_context_add_path_to_sandbox(web_context, canonical, read_only);
    g_free(canonical);
    free(resolved);
    return TRUE;
}

static void
grant_default_path(const char *path, gboolean required)
{
    if (!required && !g_file_test(path, G_FILE_TEST_EXISTS)) return;
    char *absolute = g_canonicalize_filename(path, NULL);
    char *reason = NULL;
    if (!web_context_add_path_to_sandbox(absolute, TRUE, &reason))
        fatal("cannot grant sandbox access to '%s': %s", absolute, reason);
    g_free(absolute);
    g_free(reason);
}

static void
sandbox_default_paths(void)
{
    /* Preserve extension discovery order, and configure before WebView creation. */
    char *cwd = g_get_current_dir();
    const char *extension_dirs[] = { cwd, LUAKIT_LIB_PATH };
    const char *selected = NULL;
    for (guint i = 0; i < LENGTH(extension_dirs); i++) {
        char *file = g_build_filename(extension_dirs[i], "luakit.so", NULL);
        gboolean found = !access(file, R_OK);
        g_free(file);
        if (found) { selected = extension_dirs[i]; break; }
    }
    if (!selected) fatal("cannot find required Luakit extension 'luakit.so'");
    grant_default_path(selected, TRUE);
    webkit_web_context_set_web_extensions_directory(web_context, selected);
    g_free(cwd);

    char *lib = g_build_filename(LUAKIT_INSTALL_PATH, "lib", NULL);
    char *resources = g_build_filename(LUAKIT_INSTALL_PATH, "resources", NULL);
#ifdef DEVELOPMENT_PATHS
    grant_default_path("./lib", TRUE);
    grant_default_path("./config", TRUE);
    grant_default_path("./resources", TRUE);
    grant_default_path(lib, FALSE);
#else
    grant_default_path(lib, TRUE);
#endif
    grant_default_path(resources, FALSE);
    g_free(lib);
    g_free(resources);
    grant_default_path(globalconf.config_dir, TRUE);
    const char * const *dirs = g_get_system_config_dirs();
    for (; *dirs; dirs++) {
        char *dir = g_build_filename(*dirs, "luakit", NULL);
        grant_default_path(dir, FALSE);
        g_free(dir);
    }

    /* Grant exactly the native dependency file; do not grant cpath roots. */
    lua_State *L = common.L;
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "cpath");
    gchar **paths = g_strsplit(lua_tostring(L, -1), ";", -1);
    gboolean found = FALSE;
    for (guint i = 0; paths[i] && !found; i++) {
        gchar **parts = g_strsplit(paths[i], "?", -1);
        char *file = g_strjoinv("lfs", parts);
        g_strfreev(parts);
        if (*file && !access(file, R_OK) && g_file_test(file, G_FILE_TEST_IS_REGULAR)) {
            grant_default_path(file, TRUE);
            found = TRUE;
        }
        g_free(file);
    }
    g_strfreev(paths);
    lua_pop(L, 2);
    if (!found) fatal("cannot resolve required native Lua module 'lfs' for the sandbox");
}

static void
website_data_manager_init(void)
{
    WebKitWebsiteDataManager *data_mgr = webkit_website_data_manager_new(
            "base-cache-directory", globalconf.cache_dir,
            "base-data-directory", globalconf.data_dir,
            NULL);

    web_context = webkit_web_context_new_with_website_data_manager(data_mgr);

    verbose("base_data_directory:                 %s", webkit_website_data_manager_get_base_data_directory(data_mgr));
    verbose("base_cache_directory:                %s", webkit_website_data_manager_get_base_cache_directory(data_mgr));
}

static void
web_context_set_default_spelling_language(void)
{
    /* This seems to autodetect spell checking languages */
    const gchar *null = NULL;
    webkit_web_context_set_spell_checking_languages(web_context, &null);
    gchar **ret = (gchar**)webkit_web_context_get_spell_checking_languages(web_context);
    if (!ret)
        return;
    gchar *langs = g_strjoinv(", ", ret);
    verbose("setting spell check languages: %s", langs);
    g_free(langs);
}

void
web_context_init(void)
{
    website_data_manager_init();
    webkit_web_context_set_sandbox_enabled(web_context, TRUE);
    webkit_web_context_set_favicon_database_directory(web_context, NULL);
    g_signal_connect(G_OBJECT(web_context), "download-started",
            G_CALLBACK(download_start_cb), NULL);

    /* Set default cookie policy: must match default in clib/soup.c */
    WebKitCookieManager *cookie_mgr = webkit_web_context_get_cookie_manager(web_context);
    webkit_cookie_manager_set_accept_policy(cookie_mgr, WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY);

    web_context_set_default_spelling_language();
}

void
web_context_init_finish(void)
{
    if (web_context_started)
        return;

#if !WEBKIT_CHECK_VERSION(2,26,0)
    webkit_web_context_set_process_model(web_context, WEBKIT_PROCESS_MODEL_MULTIPLE_SECONDARY_PROCESSES);
    info("Web process count: %d", process_limit);
    webkit_web_context_set_web_process_count_limit(web_context, process_limit);
#endif

    sandbox_default_paths();
    web_context_started = TRUE;
}

// vim: ft=c:et:sw=4:ts=8:sts=4:tw=80
