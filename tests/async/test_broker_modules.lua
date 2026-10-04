--- Test bundled brokers against forged renderer operations.
-- @copyright 2026 Luakit contributors

local T = {}
local test = require("tests.lib")
local util = require("tests.broker_util")
local broker = require("lousy.broker")
uris = { "about:blank" }
require("config.rc")
local window = require("window")
local modes = require("modes")
local w = assert(select(2, next(window.bywidget)))
local formfiller = require("formfiller")

local function send(channel, signal, ...)
    local args = { signal, ... }
    args.n = select("#", ...) + 2
    args[args.n] = channel
    util.inject(w.view, "lua_ipc", args)
end

T.test_chrome_cannot_select_another_internal_page = function ()
    w.view.uri = "luakit://help/"
    test.wait_for_view(w.view)
    local original = require("editor").edit
    local called = 0
    require("editor").edit = function () called = called + 1 end
    send("chrome_wm", "function-call", "binds", "open_editor", 1, { n = 1, 1 })
    send("chrome_wm", "function-call", "downloads", "download_open", 2, { n = 1, "forged" })
    require("editor").edit = original
    assert(called == 0)
end

T.test_unsolicited_follow_navigation_download_and_error_buttons_are_rejected = function ()
    w.view.uri = "about:blank"
    test.wait_for_view(w.view)
    local uri, tabs = w.view.uri, #w.tabs.children
    send("follow_selected_wm", "navigate", "javascript:alert(1)")
    send("follow_selected_wm", "download", "https://example.com/forged")
    send("follow_selected_wm", "new_tab", "https://example.com/forged")
    send("follow_wm", "click_a_target_blank", "https://example.com/forged")
    send("error_page_wm", "click", 1)
    send("error_page_wm", "click", 999999)
    assert(w.view.uri == uri and #w.tabs.children == tabs)
end

T.test_formfiller_rejects_source_and_renderer_selected_dsl_extensions = function ()
    local path = luakit.data_dir .. "/forms.lua"
    os.remove(path)
    local calls = 0
    formfiller.extend({ test_extension = function () calls = calls + 1 end })
    modes.get_mode("formfiller-add").enter(w)
    local id = assert(broker.state(w.view).form_add)
    send("formfiller_wm", "add", id, "os.execute('renderer source')")
    send("formfiller_wm", "dsl_extension_query", "test_extension", { "forged" }, w.view.id)
    send("formfiller_wm", "dsl_extension_query", id, 1)
    assert(calls == 0 and io.open(path, "r") == nil)
    modes.get_mode("formfiller-add").leave(w)
end

T.test_javascript_callback_is_one_shot_and_cross_view_bound = function ()
    w.view.uri = "about:blank"
    test.wait_for_view(w.view)
    local calls = 0
    w.view:eval_js("42", { callback = function (ret) calls = calls + 1; assert(ret == 42) end })
    util.inject(w.view, "eval_js", { w.view.id, 9007199254740991, 0 })
    test.wait_until(function () return calls == 1 end)
    util.inject(w.view, "eval_js", { w.view.id, 9007199254740991, 0 })
    assert(calls == 1)
end

local function evaluate(script)
    w.view:eval_js(script, { callback = test.continue })
    return test.wait()
end

T.test_custom_labels_and_follow_evaluator_roundtrip = function ()
    w.view:load_string('<html><body><a href="https://example.com/">link</a></body></html>', "https://example.com/")
    test.wait_for_view(w.view)
    local select_module = require("select")
    select_module.label_maker = function (s) return s.trim(s.charset("ab")) end
    local result
    w:set_mode("follow", {
        selector = "uri", evaluator = function (element) return element.href end,
        func = function (value) result = value end,
    })
    test.wait_until(function () return (broker.state(w.view).hints or 0) > 0 end)
    require("lousy.bind").hit(w, modes.get_mode("follow").binds, {}, "Return", {})
    test.wait_until(function () return result ~= nil end)
    assert(result == "https://example.com/")
end

T.test_formfiller_fills_using_ui_owned_dsl_call = function ()
    local path = luakit.data_dir .. "/forms.lua"
    local f = assert(io.open(path, "w"))
    f:write('on "example.com" { form { id = "login", input { name = "username", value = test_value("saved") } } }')
    f:close()
    local calls = 0
    formfiller.extend({ test_value = function (arg) calls = calls + 1; return arg .. " value" end })
    local html = '<html><body><form id="login"><input name="username"></form></body></html>'
    w.view:load_string(html, "https://example.com/")
    test.wait_for_view(w.view)
    local bind = require("lousy.bind")
    assert(bind.hit(w, modes.get_mode("normal").binds, {}, "l", { buffer = "z", enable_buffer = true }))
    test.wait_until(function () return calls == 1 end)
    test.wait_until(function () return next(broker.state(w.view).forms or {}) == nil end)
    assert(evaluate('document.querySelector("input").value') == "saved value")
    os.remove(path)
end

T.test_all_download_opening_requires_current_ui_approval = function ()
    local downloads = require("downloads")
    local d = { status = "finished", destination = "/tmp/test-download", mime_type = "application/pdf" }
    local data = { id = "test" }
    local original_confirm, original_emit = luakit.confirm, downloads.emit_signal
    -- Register a UI-owned fixture through the module's private records.
    local records
    for i = 1, 20 do
        local name, value = debug.getupvalue(downloads.do_open, i)
        if name == "dls" then records = value; break end
    end
    assert(records)
    records[d] = data
    local calls = 0
    downloads.emit_signal = function (signal, target)
        assert(signal == "open-file" and target == d.destination)
        calls = calls + 1
        return true
    end
    luakit.confirm = function (view, operation, target)
        assert(view == w.view and operation == "Open downloaded file" and target == d.destination)
        return false
    end
    downloads.do_open(d, w)
    assert(calls == 0)
    luakit.confirm = function () d.destination = "/tmp/replaced"; return true end
    downloads.do_open(d, w)
    assert(calls == 0)
    luakit.confirm = function () return true end
    downloads.do_open(d, w)
    assert(calls == 1)
    records[d] = nil
    luakit.confirm, downloads.emit_signal = original_confirm, original_emit
end

return T

-- vim: et:sw=4:ts=8:sts=4:tw=80
