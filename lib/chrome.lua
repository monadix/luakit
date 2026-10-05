--- Add custom luakit:// scheme rendering functions.
--
-- This module provides a convenient interface for other modules to add
-- `luakit://` chrome pages, with features like a shared theme, error reporting,
-- and Lua to JavaScript function bridge management.
--
-- @module chrome
-- @copyright 2010-2012 Mason Larobina <mason.larobina@gmail.com>
-- @copyright 2010 Fabian Streitel <karottenreibe@gmail.com>

local error_page = require("error_page")
local lousy = require("lousy")
local webview = require("webview")
local window = require("window")
local wm = require_web_module("chrome_wm")
local broker = require("lousy.broker")

local _M = {}

--- Common stylesheet that can be sourced from several chrome modules
-- for a consitent looking theme.
-- @type string
-- @readwrite
_M.stylesheet = [===[
    * {
        box-sizing: border-box;
    }
    body {
        background-color: white;
        color: #222;
        display: block;
        margin: 0;
        padding: 0;
        font-family: sans-serif;
    }
    #page-header {
        display: flex;
        -webkit-align-items: center;
        background-color: #eee;
        position: fixed;
        top: 0;
        left: 0;
        width: 100%;
        margin: 0;
        padding: 0 1.5em;
        height: 3.5em;
        border-bottom: 1px solid #ddd;
        -webkit-user-select: none;
        overflow-y: hidden;
        z-index: 100000;
    }
    #page-header > h1 {
        font-size: 1.4rem;
        margin: 0 1em;
        color: #445;
        cursor: default;
    }
    #page-header > h1:first-child {
        margin-left: 0;
    }
    .content-margin {
        padding: 3.5em 1.5em 0 1.5em;
    }
    h2 { font-size: 1.2rem; }
    h3 { font-size: 1.1rem; color: #666; }

    #page-header input {
        font-size: 0.8rem;
        padding: 0.5rem 0.75rem;
        border: none;
        outline: none;
        margin-top: 0;
        margin-bottom: 0;
        background-color: #fff;
    }

    #page-header #search-box {
        display: flex;
        padding: 0;
        background-color: #fff;
        border-radius: 0.25rem;
        box-shadow: 0 1px 1px #888;
    }

    #page-header #search {
        width: 20em;
        font-weight: normal;
        border-radius: 0.25rem 0 0 0.25rem;
        margin: 0;
        padding-right: 0;
    }

    #page-header #clear-button {
        margin: 0;
        padding: 0.5rem 0.55rem;
        border-radius: 0 0.25rem 0.25rem 0;
        box-shadow: none;
        font-size: 1rem;
        line-height: 1rem;
    }

    #page-header #clear-button:hover {
        color: #000;
    }

    #page-header #clear-button:active {
        background-color: #eee;
    }

    .button {
        box-shadow: 0 1px 1px #888;
        margin: 1rem 0 1rem 0.5rem;
        border-radius: 0.25em;
        color: #888;
        display: inline-block;
        line-height: 1.25;
        text-align: center;
        white-space: nowrap;
        vertical-align: middle;
        -webkit-user-select: none;
        border: 1px solid transparent;
        padding: .5rem 1rem;
        font-size: 1rem;
        border-radius: .25rem;
        transition: color .1s ease-in-out, background-color .1s ease-in-out;
        cursor: pointer;
    }

    #page-header .button:hover, .button:hover {
        color: #000;
    }

    #page-header .button:active, .button:active {
        background-color: #eee;
    }

    #page-header .button[disabled], .button[disabled] {
        color: #888;
        background-color: #eee;
        cursor: not-allowed;
    }

    #page-header .rhs {
        display: flex;
        -webkit-align-items: center;
        position: fixed;
        top: 0;
        right: 0;
        margin: 0;
        padding-right: 1.5em;
        height: 3.5em;
        margin: 0;
        background-color: inherit;
        box-shadow: -1em 0 1em #eee;
    }

    #page-header .rhs .button {
        margin-bottom: 0;
    }

    .license {
        font-family: monospace;
    }

    .hidden {
        display: none;
    }
]===]

-- luakit:// page handlers
local handlers = {}
local on_first_visual_handlers = {}
local page_funcs = {}
local page_validators = {}

--- Retrieve a list of the currently registered luakit:// handlers.
-- @treturn {string} A list of `luakit://` handler names, in alphabetical order.
function _M.available_handlers()
    return lousy.util.table.keys(handlers)
end

--- Register a chrome page URI with an associated handler function.
-- @tparam string page The name of the chrome page to register.
-- @tparam function func The handler function for the chrome page.
-- @tparam function on_first_visual_func An optional handler function
-- for the chrome page, called when the page first finishes loading.
-- @tparam table export_funcs An optional table of functions to
-- export to JavaScript.
-- @tparam[opt] table export_schemas Exact argument schemas; exports are disabled when omitted.
function _M.add(page, func, on_first_visual_func, export_funcs, export_schemas)
    -- Do some sanity checking
    assert(type(page) == "string",
        "invalid chrome page name (string expected, got "..type(page)..")")
    assert(string.match(page, "^[%w%-]+$"),
        "illegal characters in chrome page name: " .. page)
    assert(type(func) == "function",
        "invalid chrome handler (function expected, got "..type(func)..")")
    assert(type(on_first_visual_func) == "nil"
        or type(on_first_visual_func) == "function",
        "invalid chrome handler (function/nil expected, got "..type(on_first_visual_func)..")")

    for name, export_func in pairs(export_funcs or {}) do
        assert(type(name) == "string")
        assert(type(export_func) == "function")
        if export_schemas then
            assert(export_schemas[name], "chrome exports require argument schemas: " .. name)
        end
    end

    if export_funcs and next(export_funcs) and not export_schemas then
        msg.warn("Chrome page %s: exports disabled; add argument schemas as the fifth chrome.add argument", page)
        export_funcs = nil
    end

    handlers[page] = func
    on_first_visual_handlers[page] = on_first_visual_func
    page_funcs[page] = export_funcs
    page_validators[page] = {}
    for name, schema in pairs(export_schemas or {}) do
        page_validators[page][name] = broker.args(schema)
    end

    if page_funcs[page] then
        page_validators[page].reset_mode = broker.args({})
        page_funcs[page].reset_mode = function (view)
            for _, w in pairs(window.bywidget) do
                if w.view == view then
                    w:set_mode()
                end
            end
        end
    end
end

--- Remove a regeistered chrome page.
-- @tparam string page The name of the chrome page to remove.
function _M.remove(page)
    handlers[page] = nil
    on_first_visual_handlers[page] = nil
    page_funcs[page] = nil
    page_validators[page] = nil
end

luakit.register_scheme("luakit")

-- Catch all navigations to the luakit:// scheme
webview.add_signal("init", function (view)
    view:add_signal("scheme-request::luakit", function (v, uri, request)
        -- Match "luakit://page/path"
        local page, path = string.match(uri, "^luakit://([^/]+)/?(.*)")
        if not page then return end
        broker.state(v).chrome_page = page

        local func = handlers[page]
        if func then
            -- Give the handler function everything it may need
            local w = webview.window(v)
            local meta = { page = page, path = path, w = w,
                uri = "luakit://" .. page .. "/" .. path,
                request = request }

            -- Render error output in webview with traceback
            local function error_handler(err)
                error_page.show_error_page(v, {
                    heading = "Chrome handler error",
                    content = [==[
                        <div class="errorMessage">
                            <p>An error occurred in the <code>luakit://{page}/</code> handler function:
                            <pre>{traceback}</pre>
                        </div>
                    ]==],
                    buttons = {},
                    page = page,
                    traceback = debug.traceback(err, 2),
                    request = request,
                })
            end

            -- Call luakit:// page handler
            local ok, html, mime = xpcall(function () return func(v, meta) end,
                error_handler)
            if ok and not request.finished then request:finish(html, mime) end
            return
        end

        -- Load error page
        error_page.show_error_page(v, {
            heading = "Chrome handler error",
            content = [==[
                <div class="errorMessage">
                    <p>No chrome handler for <code>luakit://{page}/</code></p>
                </div>
            ]==],
            buttons = {},
            page = page,
            request = request,
        })
    end)

    view:add_signal("load-status", function (v, status)
        -- Wait for new page to be created
        if status ~= "finished" then return end

        -- Match "luakit://page/path"
        local page, path = string.match(v.uri, "^luakit://([^/]+)/?(.*)")
        if not page then return end

        -- Ensure we have a hook to call
        local on_first_visual_func = on_first_visual_handlers[page]
        if not on_first_visual_func then return end

        local w = webview.window(v)
        local meta = { page = page, path = path, w = w,
            uri = "luakit://" .. page .. "/" .. path }

        -- Call the supplied handler
        on_first_visual_func(v, meta)
    end)
    -- Always enable JavaScript on luakit:// pages; without this, chrome
    -- pages which depend upon javascript will break
    view:add_signal("enable-scripts", function (v)
        if v.uri:match("^luakit://") then return true end
    end)
end)

wm:add_web_signal("function-call", {
    validate = function (_, _, page_name, func_name, id, args, ...)
        if select("#", ...) ~= 0 or type(page_name) ~= "string" or type(func_name) ~= "string"
            or not broker.id(id) or type(args) ~= "table" or type(args.n) ~= "number" or not broker.id(args.n + 1)
            or args.n > 256 then return false end
        for k in pairs(args) do
            if k ~= "n" and (not broker.id(k) or k > args.n) then return false end
        end
        local validator = page_validators[page_name] and page_validators[page_name][func_name]
        return validator ~= nil and validator(unpack(args, 1, args.n)) == true
    end,
    authorize = function (_, view, page_name, func_name, id)
        local state = broker.state(view)
        return state.chrome_page == page_name and view.uri:match("^luakit://([^/]+)") == page_name
            and page_funcs[page_name] ~= nil and page_funcs[page_name][func_name] ~= nil
            and id > (state.chrome_last_id or 0)
    end,
}, function (_, view, page_name, func_name, id, args)
    broker.state(view).chrome_last_id = id
    local func = page_funcs[page_name][func_name]
    local ok, ret = xpcall(
        function () return func(view, unpack(args, 1, args.n)) end,
        function (err) msg.error(debug.traceback(err, 3)) end)
    wm:emit_signal(view, "function-return", id, ok, ret)
end)

luakit.add_signal("web-extension-created", function ()
    for page, export_funcs in pairs(page_funcs) do
        for name in pairs(export_funcs or {}) do
            wm:emit_signal("register-function", page, name)
        end
    end
end)

return _M

-- vim: et:sw=4:ts=8:sts=4:tw=80
