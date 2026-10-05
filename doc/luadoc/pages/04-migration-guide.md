@name Migration Guide
#Migration Guide

## Migrating WebProcess modules to the sandbox broker

UI-only keybindings, settings and scripts retain their existing filesystem access.
`require_web_module()` still loads user modules from the active configuration
and profile directory. The sandbox applies to the WebProcess, not the UI.

### Compatibility adapters

UI-to-Web `channel:emit_signal(view, name, ...)` and broadcasts remain supported,
including serializable Lua functions and their upvalues. Functions run in the
WebProcess; they cannot carry UI userdata, C functions or pointer references.

Tables are copied as raw entries, even when they have metatables. Metatables and
metamethod behavior do not cross IPC. Receivers get ordinary tables. Values must
remain bounded: 16 MiB per message, 1 MiB per string, depth 32 and 100,000 values.
Numeric/string/boolean/table keys, finite numbers, booleans, binary strings and
nil arguments are supported. UI-to-Web messages also support serializable Lua
function keys. Cycles, userdata and pointers are rejected.

### UI handlers must explicitly authorize renderer traffic

A two-argument `channel:add_signal(name, handler)` is process-local. It never
receives renderer messages, even if another handler for that name opts in.
Old handler signatures can be preserved by adding a third policy argument:

```lua
-- UI module: bind this example to an existing UI-owned view.
local broker = require("lousy.broker")
local wm = require_web_module("example_wm")
wm:add_signal("uri", function (_, id, text)
    broker.state(view).awaiting_uri = nil
    w:notify(text)
end, {
    legacy_page_arg = 1,
    validate = function (_, originating, id, text, ...)
        return originating == view and id == originating.id
            and type(text) == "string" and #text <= 8192
            and select("#", ...) == 0
    end,
    authorize = function (_, originating)
        return broker.state(originating).awaiting_uri == true
    end,
})
-- Set this only when the UI initiates the operation.
broker.state(view).awaiting_uri = true
wm:emit_signal(view, "get-uri")
```

The existing web-side reply can remain:

```lua
local ui = ipc_channel("example_wm")
ui:add_signal("get-uri", function (_, page)
    ui:emit_signal("uri", page.id, page.uri)
end)
```

Both policy callbacks receive `(channel, originating_view, ...old_payload)`;
errors or anything except `true` reject dispatch. The legacy handler still
receives `(channel, ...old_payload)`. `legacy_page_arg` is a one-based payload
position, not a position including the signal name. Use `2` for an old reply
such as `emit_signal("uri", text, page.id)`, and adjust its handler/validator
argument order accordingly. Conflicting positions and mixing legacy and modern
broker registrations for the same signal are errors. Multiple opted-in handlers
are supported; existing signal removal methods remove their registrations too.

Alternatively, use `add_web_signal(name, policy, handler)`. Its handler receives
`(channel, originating_view, ...)`; pass a page explicitly from the web module:
`ui:emit_signal(page, "uri", text)`.

### Replies without a page ID

Legacy `emit_signal(name, ...)` can infer a page during a page-specific UI
channel callback, `page-created`, registered JavaScript-function callback,
`document-loaded`, `send-request`, or DOM event callback. This context ends when
the callback returns and is restored after nested callbacks and errors.

Timers, idle callbacks and process-wide broadcasts do not have an implicit
page. Preserve an existing page-ID payload and declare `legacy_page_arg`, or
capture a page explicitly:

```lua
luakit.idle_add(function () ui:emit_signal(page, "uri", page.uri) end)
```

A declared ID must identify a live page and agree with any current callback
page. There is no active-tab or single-tab fallback. Process-wide diagnostic
modules must choose an explicit originating page for broker notifications;
context traffic cannot invoke page-specific UI operations.

### Cases that cannot safely retain the old behavior

Every WebProcess message is hostile, including messages from internal pages and
user-authored modules. Loading a module is not an authorization grant.

- **Automatic UI dispatch:** add a validation/authorization policy. Never use
  an unconditional policy for a handler that launches programs, opens files,
  changes certificate trust or writes host state.
- **Web-to-UI functions or callback pointers:** send bounded data and an
  operation ID. Keep callbacks and operation records in the UI, bind them to
  the originating view/document, and consume requests before acting. Navigation
  and process replacement invalidate the broker generation.
- **Renderer-selected filenames, programs or form specifications:** resolve
  IDs against UI-owned records. Do not execute renderer-generated Lua source;
  send structured form data and generate quoted persistence text in the UI.
  Existing form DSL files remain usable; copied old formfiller web modules must
  adopt the new protocol.
- **Privileged requests:** keep the exact target immutable and use
  `luakit.confirm(view, operation, target)` before program launch, downloaded-file
  opening or certificate exceptions. Renderer navigation to internal/local-file
  pages requires UI approval too. UI-selected exact destinations remain allowed.
- **Arbitrary filesystem access:** grant specific existing absolute paths with
  `luakit.add_path_to_sandbox(path)` before the first WebView. The default is
  read-only; use `false` only for an intentional write grant. Search-path changes
  alone do not grant access. Move sensitive file operations into the UI instead
  of granting home, data/cache or temporary-directory roots.
- **Old bundled-module overrides:** merge the updated follow, formfiller,
  chrome and error-page protocols into copied modules. Adapters do not restore
  removed unsafe built-in operations.

### Custom internal pages

Legacy four-argument `chrome.add()` still registers and renders the page, but
exports without schemas are disabled with a migration warning. Restore each
export by adding a fifth argument:

```lua
chrome.add("example", render, nil, { search = search }, {
    search = { "string" },
})
```

Schemas contain type names or predicates; optional types end in `?`. An
explicit schema table must cover every export. Calls must match the UI-observed
internal page. Schemas validate data; privileged exports still need UI-owned
operation checks and confirmation.

See the @ref{ipc} API and
[WebProcess filesystem access](05-configuration.html#webprocess-filesystem-access).

## Migrating from version 2017-08-10

### Remove unique instance code from `rc.lua`

Unique instance support has been moved to a module. To update, follow these steps:

 1. Remove the two `if unique then ... end` blocks from your `rc.lua`.
 2. Add `require "unique_instance"` to your `rc.lua`, before all other `require` statements.

### Move `globals` settings to `rc.lua`

The `globals.lua` configuration file has been removed. In its place
is the new `settings` module, which provides a central place to set
settings with validation and domain-specific setting support.

All modified fields of the `globals` table need to be migrated to
equivalent settings. When starting luakit, a warning will be logged
for each setting that must be migrated, showing the new code for that
setting.

### Remove old configuration files

The `window.lua`, `webview.lua`, and `webview_wm.lua` configuration
files have been made core modules, and any configuration files are
ignored.

If you have made extensive changes to these files, please open an issue
on the luakit GitHub repository to discuss adding new APIs to support
your changes. As an interim solution, you can still override the core
modules by modifying the `package.path` variable; note that this is
_not_ officially supported, and unless you carefully keep your modified
files synchronized with new changes, unpredictable errors may result.

## Migrating from a pre-WebKit2 Version

The latest luakit release is built around the WebKit 2 APIs. Changes in the APIs
provided by WebKit have necessitated some corresponding changes in luakit's
APIs, and as a result there have been many breaking changes.

### Remove any existing personal configuration

Luakit's configuration file has seen many changes; as a result, you are likely
to encounter errors unless you remove any personal configuration for a previous
luakit version. You may wish to back up your configuration if you have made
extensive changes; such changes can then be merged in to the up-to-date
configuration.

**_Note:_** It is inadvisable to copy luakit's entire system configuration
directory to make a personal copy, as was previously encouraged. This
makes it harder for you to upgrade luakit, as you have more files to
merge upstream changes into, and you are more likely to experience
errors caused by such upstream changes (until those changes are merged with your
personal configuration).

### Migrate personal key bindings

The `binds.lua` and `modes.lua` configuration files have been removed.
It is no longer necessary to copy the entire set of key bindings to
your configuration to change or add just a few key bindings. Instead,
use @ref{modes/add_binds} and @ref{remove_binds} to add and remove key
bindings.

### Remove use of `module()` in scripts

Lua's `module()` function sets a global variable of the same name as the module,
which pollutes the global namespace for _all_ Lua code. A recent effort
was made to remove all usage of `module()` within Luakit. While `module()` is
still accessible, its use is not advised.

This has the side effect that many variables once assumed globally accessible
must now be retrieved with a call to `require()`.

### Remove use of `init_funcs`

The `window` and `webview` classes previously had a method of registering
functions to be called when a new window or webview was created, called
`init_funcs`. This has been removed entirely in favor of a signals-based
approach, which does not run the risk of name collision.

The `window` and `webview` classes now emit the `"init"` signal. To replace any
functions previously registered via `init_funcs`, add a signal handler:

	webview.add_signal("init", function (view)
	    -- Do something with new view
	end)

One important difference between the `"init"` signal and the
`init_funcs` method is that the former no longer passes the current
window table `w` as a parameter. This is because views may be moved
between windows (for example, with the `:tabdetach` command).

The `window` class also emits a `"build"` signal _before_ emitting the
init signal. This provides an opportunity for modules to change the layout and
arrangement of widgets within the window, before the `"init"` signal is emitted.

### Remove use of `local capi` table

Some old code wraps references to several builtin variables in a `capi` table.
This does `not` namespace the builtin variables wrapped in this manner; they are
still globally accessible (and otherwise could not be wrapped in this fashion),
and so all this accomplishes is the addition of more code, more indirection, and
confusion about luakit's and Lua's namespacing rules, for no actual advantage.

As such, it is advised to remove any such wrapper tables, and reference the
globally-accessible builtin luakit libraries directly.

### Use @ref{styles} module instead of `user_stylesheet_uri`

The @ref{styles} module offers a replacement for the old `user_stylesheet_uri`
property, which was used to set a stylesheet to customize web behavior. This
module has a number of advantages over the old method, including the ability to
enable/disable stylesheets at runtime, the ability to have multiple stylesheets,
and the ability to use `@-moz-document` rules in your stylesheets.

1. Ensure the @ref{styles} module is enabled in your `rc.lua`.
2. Locate the @ref{styles} sub-directory within luakit's data storage directory.
   Normally, this is located at `~/.local/share/luakit/styles/`. Create the
   directory if it does not already exist.
3. Move any CSS rules to a new file within that directory. In order for the
   @ref{styles} module to load the stylesheet, the filename must end in `.css`.
4. Make sure you specify which sites your stylesheet should apply to. The way to
   do this is to use `@-moz-document` rules. The Stylish wiki page [Applying styles to specific sites](https://github.com/stylish-userstyles/stylish/wiki/Applying-styles-to-specific-sites) may be helpful.
5. Run `:styles-reload` to detect new stylesheet files and reload any changes to
   existing stylesheet files; it isn't necessary to restart luakit.
