/*
 * tests/process_swap.c - enable cross-site swaps in the renderer regression
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

/* Preloaded only by the renderer replacement test. Preserve production context
 * defaults while exercising WebKit's construct-only process swap setting. */
WebKitWebContext *
webkit_web_context_new_with_website_data_manager(WebKitWebsiteDataManager *manager)
{
    return g_object_new(WEBKIT_TYPE_WEB_CONTEXT,
            "website-data-manager", manager,
            "process-swap-on-cross-site-navigation-enabled", TRUE, NULL);
}

// vim: ft=c:et:sw=4:ts=8:sts=4:tw=80
