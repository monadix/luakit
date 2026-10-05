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
    w:navigate("luakit://help/")
    test.wait_for_view(w.view)
    local original = require("editor").edit
    local called = 0
    require("editor").edit = function () called = called + 1 end
    send("chrome_wm", "function-call", "binds", "open_editor", 1, { n = 1, 1 })
    send("chrome_wm", "function-call", "downloads", "download_open", 2, { n = 1, "forged" })
    require("editor").edit = original
    assert(called == 0)
end

T.test_legacy_chrome_pages_render_with_exports_disabled_until_schematized = function ()
    local chrome = require("chrome")
    local called = 0
    local render = function () return "<html><title>Compatibility</title><body>legacy</body></html>" end
    local exported = function () called = called + 1 end
    chrome.add("compat-legacy", render, nil, { legacy_export = exported })
    assert(not pcall(chrome.add, "compat-invalid", render, nil, { legacy_export = exported }, {}))
    w:navigate("luakit://compat-legacy/")
    test.wait_for_view(w.view)
    test.wait_until(function () return w.view.uri == "luakit://compat-legacy/" and w.view.title == "Compatibility" end)
    send("chrome_wm", "function-call", "compat-legacy", "legacy_export", 1, { n = 0 })
    assert(called == 0)
    chrome.add("compat-legacy", render, nil, { legacy_export = exported }, { legacy_export = {} })
    send("chrome_wm", "function-call", "compat-legacy", "legacy_export", 2, { n = 0 })
    assert(called == 1)
    w:navigate("luakit://help/")
    test.wait_for_view(w.view)
    send("chrome_wm", "function-call", "compat-legacy", "legacy_export", 3, { n = 0 })
    assert(called == 1)
    chrome.remove("compat-legacy")
end

T.test_unsolicited_follow_navigation_download_and_error_buttons_are_rejected = function ()
    w:navigate("about:blank")
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
    w:navigate("about:blank")
    test.wait_for_view(w.view)
    local calls = 0
    w.view:eval_js("42", { callback = function (ret) calls = calls + 1; assert(ret == 42) end })
    util.inject(w.view, "eval_js", { w.view.id, 9007199254740991, 0 })
    test.wait_until(function () return calls == 1 end)
    util.inject(w.view, "eval_js", { w.view.id, 9007199254740991, 0 })
    assert(calls == 1)
end

T.test_renderer_navigation_to_internal_pages_requires_ui_confirmation = function ()
    local origin = test.http_server() .. "hello_world.html"
    w:navigate(origin)
    test.wait_for_view(w.view)
    local original = luakit.confirm
    local prompts = 0
    luakit.confirm = function (view, operation, target)
        assert(view == w.view and operation == "Open privileged page" and target == "luakit://binds/")
        prompts = prompts + 1
        return original(view, operation, target)
    end
    util.confirm_response(false)
    w.view:eval_js('window.location = "luakit://binds/"', { no_return = true })
    test.delay(400)
    assert(prompts == 1 and w.view.uri == origin, tostring(prompts) .. ": " .. w.view.uri)
    util.confirm_response(true)
    w.view:eval_js('window.location = "luakit://binds/"', { no_return = true })
    test.wait_for_view(w.view)
    assert(prompts == 2 and w.view.uri == "luakit://binds/")
    luakit.confirm = original
end

T.test_file_navigation_requires_approval_but_exact_ui_load_is_allowed = function ()
    local origin = test.http_server() .. "hello_world.html"
    w:navigate(origin)
    test.wait_for_view(w.view)
    local target = "file://" .. require("lfs").currentdir() .. "/tests/html/broker.html"
    local original, prompts = luakit.confirm, 0
    luakit.confirm = function (view, operation, uri)
        assert(view == w.view and operation == "Open privileged page" and uri == target)
        prompts = prompts + 1
        return false
    end
    -- Bypass the UI URI setter to exercise an unselected WebKit policy request.
    util.request_navigation(w.view, target)
    test.wait_until(function () return prompts == 1 end)
    assert(w.view.uri == origin)
    w.view.uri = target
    test.wait_for_view(w.view)
    assert(w.view.uri == target and prompts == 1)
    luakit.confirm = original
end

local function evaluate(script)
    w.view:eval_js(script, { callback = test.continue })
    return test.wait()
end

T.test_untransferable_javascript_results_report_errors_without_crashing = function ()
    w:navigate("about:blank")
    test.wait_for_view(w.view)
    local crashed = false
    local on_crash = function () crashed = true end
    w.view:add_signal("crashed", on_crash)
    for _, script in ipairs({ "NaN", "Infinity", "-Infinity", "({ value: NaN })", "'x'.repeat(1048577)" }) do
        local value, err = evaluate(script)
        assert(value == nil and type(err) == "string" and #err > 0)
        assert(not crashed)
    end
    assert(evaluate("42") == 42)
    w.view:remove_signal("crashed", on_crash)
end

T.test_payload_limit_excludes_the_protocol_envelope = function ()
    local channel = ipc_channel("payload_limit_test")
    local calls = 0
    channel:add_web_signal("reply", {
        validate = function () return true end,
        authorize = function () return true end,
    }, function () calls = calls + 1 end)
    local args = { "reply" }
    for i = 2, 16 do args[i] = string.rep("x", 1048576) end
    args[17], args[18] = string.rep("x", 1024), "payload_limit_test"
    local length = 16777216 - util.encoded_size(args) + 1024
    args[17] = string.rep("x", length)
    while util.encoded_size(args) > 16777216 do
        length = length - 1
        args[17] = string.rep("x", length)
    end
    assert(util.encoded_size(args) >= 16777200)
    util.inject(w.view, "lua_ipc", args)
    assert(calls == 1)
    channel:remove_signals("reply")
end

T.test_downloads_page_renders_the_bundled_speed_filter = function ()
    local downloads = require("downloads")
    local records
    for i = 1, 20 do
        local name, value = debug.getupvalue(downloads.get_all, i)
        if name == "dls" then records = value; break end
    end
    assert(records)
    local d = {
        status = "started", destination = "/tmp/speed-fixture", uri = "https://example.com/file",
        current_size = 1024, total_size = 2048,
    }
    records[d] = { id = "speed-fixture", created = luakit.time(), speed = 1024 }
    w:navigate("luakit://downloads/")
    test.wait_for_view(w.view)
    test.wait_until(function ()
        return evaluate('document.querySelector(\'.download[data-id="speed-fixture"]\') !== null')
    end)
    records[d] = nil
    w:navigate("about:blank")
    test.wait_for_view(w.view)
end

T.test_custom_labels_and_follow_evaluator_roundtrip = function ()
    w:navigate(test.http_server() .. "broker.html")
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

local function follow_fixture(evaluator, all)
    w:navigate(test.http_server() .. "broker.html")
    test.wait_for_view(w.view)
    local results = {}
    w:set_mode("follow", {
        selector = "uri", evaluator = evaluator,
        func = function (value) results[#results + 1] = value end,
    })
    test.wait_until(function () return broker.state(w.view).hints == 2 end)
    require("lousy.bind").hit(w, modes.get_mode("follow").binds, all and { "Shift" } or {}, "Return", {})
    local operation = assert(broker.state(w.view).follow_operation)
    test.wait_until(function () return #results == (all and 2 or 1) end)
    test.wait_until(function () return broker.state(w.view).follow_operation == nil end)
    return results, operation
end

T.test_follow_all_consumes_the_whole_authorized_batch = function ()
    local results, operation = follow_fixture("uri", true)
    table.sort(results)
    assert(results[1] == "https://example.com/" and results[2] == "https://example.com/second")
    send("follow_wm", "follow_func", operation.id, "https://example.com/forged")
    assert(#results == 2)
end

T.test_custom_follow_evaluators_preserve_plain_data_results = function ()
    local results = follow_fixture(function () return 42 end)
    assert(results[1] == 42)
    results = follow_fixture(function () return { [3.5] = "a\0b", enabled = true, value = 1.25 } end)
    assert(results[1][3.5] == "a\0b" and results[1].enabled == true and results[1].value == 1.25)
end

T.test_follow_replies_are_bound_to_the_current_ui_operation = function ()
    w:navigate(test.http_server() .. "broker.html")
    test.wait_for_view(w.view)
    local calls = 0
    w:set_mode("follow", { selector = "uri", evaluator = "uri", func = function () calls = calls + 1 end })
    test.wait_until(function () return broker.state(w.view).hints == 2 end)
    modes.get_mode("follow").changed(w, "unmatched-one")
    local old = assert(broker.state(w.view).follow_operation).id
    modes.get_mode("follow").changed(w, "unmatched-two")
    local current = assert(broker.state(w.view).follow_operation).id
    send("follow_wm", "follow_func", old, "https://example.com/forged")
    send("follow_wm", "follow_func", current, { forged = true })
    send("follow_wm", "follow_func", current, "javascript:alert(1)")
    local tabs = #w.tabs.children
    send("follow_wm", "click_a_target_blank", current, "https://example.com/forged")
    assert(#w.tabs.children == tabs)
    assert(calls == 0)
    w:set_mode()
    send("follow_wm", "follow_func", current, "https://example.com/forged")
    assert(calls == 0 and broker.state(w.view).follow_operation == nil)
end

T.test_formfiller_fills_using_ui_owned_dsl_call = function ()
    local path = luakit.data_dir .. "/forms.lua"
    local f = assert(io.open(path, "w"))
    f:write('on "broker%.html" { form { id = "login", input { name = "username", value = test_value("saved") } } }')
    f:close()
    local calls = 0
    formfiller.extend({ test_value = function (arg) calls = calls + 1; return arg .. " value" end })
    w:navigate(test.http_server() .. "broker.html")
    test.wait_for_view(w.view)
    local action
    for _, binding in ipairs(modes.get_mode("normal").binds) do
        if binding[2].desc == "Load formfiller form (use first profile)." then action = binding[2].func end
    end
    assert(action)(w)
    assert(next(broker.state(w.view).forms or {}) ~= nil, "no form operation at " .. w.view.uri)
    test.wait_until(function () return calls == 1 end)
    test.wait_until(function () return next(broker.state(w.view).forms or {}) == nil end)
    assert(evaluate('document.querySelector("input").value') == "saved value")
    os.remove(path)
end

T.test_formfiller_errors_release_initial_and_resumed_operations = function ()
    local path = luakit.data_dir .. "/forms.lua"
    local action
    for _, binding in ipairs(modes.get_mode("normal").binds) do
        if binding[2].desc == "Load formfiller form (use first profile)." then action = binding[2].func end
    end
    assert(action)
    local original_error, errors, calls = w.error, 0, 0
    w.error = function (_, text) assert(type(text) == "string"); errors = errors + 1 end
    formfiller.extend({ failure_value = function () calls = calls + 1; return "value" end })
    for i, value in ipairs({ '"value"', 'failure_value()' }) do
        local f = assert(io.open(path, "w"))
        f:write('on "broker%.html" { form { id = "login", submit = true, '
            .. 'input { name = "username", value = ' .. value .. ' } } }')
        f:close()
        w:navigate(test.http_server() .. "broker.html")
        test.wait_for_view(w.view)
        action(w)
        assert(next(broker.state(w.view).forms or {}) ~= nil)
        test.wait_until(function () return errors == i end)
        assert(next(broker.state(w.view).forms or {}) == nil)
    end
    assert(calls == 1)
    w.error = original_error
    os.remove(path)
end

T.test_formfiller_extension_errors_release_operations_and_allow_retry = function ()
    local path = luakit.data_dir .. "/forms.lua"
    local f = assert(io.open(path, "w"))
    f:write('on "broker%.html" { form { id = "login", input { name = "username", value = failure_value() } } }')
    f:close()
    w:navigate(test.http_server() .. "broker.html")
    test.wait_for_view(w.view)
    local action
    for _, binding in ipairs(modes.get_mode("normal").binds) do
        if binding[2].desc == "Load formfiller form (use first profile)." then action = binding[2].func end
    end
    assert(action)
    local original_error, errors, calls = w.error, 0, 0
    w.error = function (_, text)
        assert(type(text) == "string" and #text > 0 and #text <= 8192)
        errors = errors + 1
    end
    for i, extension in ipairs({
        function () error("extension failed") end,
        function () error(setmetatable({}, { __tostring = function () error("nested error") end })) end,
        function () error(setmetatable({}, { __tostring = function () return {} end })) end,
        function () return string.rep("x", 1048577) end,
        function () local cycle = {}; cycle.self = cycle; return cycle end,
    }) do
        formfiller.extend({ failure_value = function () calls = calls + 1; return extension() end })
        action(w)
        test.wait_until(function () return errors == i end)
        assert(calls == i and next(broker.state(w.view).forms or {}) == nil)
    end
    formfiller.extend({ failure_value = function () calls = calls + 1; return "retry value" end })
    action(w)
    test.wait_until(function () return next(broker.state(w.view).forms or {}) == nil end)
    assert(calls == 6 and evaluate('document.querySelector("input").value') == "retry value")
    w.error = original_error
    os.remove(path)
end

T.test_formfiller_extension_results_do_not_cross_documents = function ()
    local channel = require_web_module("formfiller_wm")
    local mt = debug.getmetatable(channel)
    local original_emit, calls, replies = mt.emit_signal, 0, 0
    mt.emit_signal = function (self, view, signal, ...)
        if signal == "dsl_extension_reply" then replies = replies + 1 end
        return original_emit(self, view, signal, ...)
    end
    formfiller.extend({ changed_document = function ()
        calls = calls + 1
        w.view:emit_signal("load-status", "provisional")
        return "old document credential"
    end })
    broker.state(w.view).forms = {
        [123456] = { calls = { { key = "changed_document", args = {} } } },
    }
    send("formfiller_wm", "dsl_extension_query", 123456, 1)
    mt.emit_signal = original_emit
    assert(calls == 1 and replies == 0)
    assert(broker.state(w.view).forms == nil)
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

T.test_deferred_download_open_preserves_explicit_window = function ()
    local downloads = require("downloads")
    local function upvalue(func, wanted)
        for i = 1, 30 do
            local name, value = debug.getupvalue(func, i)
            if name == wanted then return value end
        end
        error("missing upvalue " .. wanted)
    end
    local records = upvalue(downloads.do_open, "dls")
    local status_timer = upvalue(downloads.add, "status_timer")
    local d = { status = "started", destination = "/tmp/deferred-download", mime_type = "application/pdf" }
    local data = { id = "deferred-test", last_status = "started" }
    records[d] = data
    local original_confirm, original_emit = luakit.confirm, downloads.emit_signal
    local confirmations, opens = 0, 0
    luakit.confirm = function (view)
        assert(view == w.view)
        confirmations = confirmations + 1
        return true
    end
    downloads.emit_signal = function (signal, _, _, opening_window)
        if signal == "open-file" then
            assert(opening_window == w)
            opens = opens + 1
            return true
        end
    end
    local function finish(opening_window)
        d.status, data.last_status = "started", "started"
        downloads.open(data.id, opening_window)
        assert(opens == confirmations)
        d.status = "finished"
        status_timer:start()
        status_timer:emit_signal("timeout")
        assert(not data.opening and data.opening_window == nil)
    end
    finish(w) -- No opts.window: completion must retain open(id, w).
    assert(opens == 1 and confirmations == 1)
    finish({ win = {}, view = w.view }) -- A closed/unregistered explicit window.
    assert(opens == 1 and confirmations == 1)
    data.window = w.win
    finish(nil) -- Existing originating-window fallback.
    assert(opens == 2 and confirmations == 2)
    downloads.do_open(d, w)
    assert(opens == 3)
    records[d] = nil
    luakit.confirm, downloads.emit_signal = original_confirm, original_emit
end

T.test_yank_link_preserves_mailto_uri_support = function ()
    w:navigate(test.http_server() .. "broker.html")
    test.wait_for_view(w.view)
    evaluate('document.body.innerHTML = \'<a href="mailto:user@example.com">mail</a>\';')
    w:set_mode("ex-follow")
    require("lousy.bind").hit(w, modes.get_mode("ex-follow").binds, {}, "y", {})
    test.wait_until(function () return broker.state(w.view).hints == 1 end)
    require("lousy.bind").hit(w, modes.get_mode("follow").binds, {}, "Return", {})
    test.wait_until(function () return broker.state(w.view).follow_operation == nil end)
    assert(luakit.selection.primary == "user@example.com")
end

T.test_formfiller_preserves_checked_and_unchecked_inputs = function ()
    local path = luakit.data_dir .. "/forms.lua"
    os.remove(path)
    w:navigate(test.http_server() .. "broker.html")
    test.wait_for_view(w.view)
    evaluate([=[
        document.querySelector('form').innerHTML =
            '<input name="yes" type="checkbox" checked><input name="no" type="checkbox">' +
            '<input name="radio" type="radio">';
    ]=])
    local editor = require("editor")
    local original_edit, edited = editor.edit, false
    editor.edit = function (file) assert(file == path); edited = true end
    w:set_mode("formfiller-add")
    -- Wait for the renderer to create the form hint before selecting it.
    evaluate("true")
    for _, binding in ipairs(modes.get_mode("formfiller-add").binds) do
        if binding[2].desc == "Add the currently focused form to the formfiller file." then
            binding[2].func(w)
        end
    end
    test.wait_until(function () return edited end)
    editor.edit = original_edit
    local f = assert(io.open(path))
    local saved = f:read("*a")
    f:close()
    assert(saved:find("checked = true", 1, true) and saved:find("checked = false", 1, true))
    saved = saved:gsub("submit = true", "submit = false"):gsub("autofill = true", "autofill = false")
    f = assert(io.open(path, "w"))
    f:write(saved)
    f:close()
    local function fill()
        for _, binding in ipairs(modes.get_mode("normal").binds) do
            if binding[2].desc == "Load formfiller form (use first profile)." then binding[2].func(w) end
        end
        test.wait_until(function () return next(broker.state(w.view).forms or {}) == nil end)
        assert(evaluate([=[
            document.querySelector('[name=yes]').checked && !document.querySelector('[name=no]').checked
                && !document.querySelector('[name=radio]').checked
        ]=]))
    end
    fill() -- Already correct: filling must not toggle the checked box off.
    evaluate([=[
        document.querySelector('[name=yes]').checked = false;
        document.querySelector('[name=no]').checked = true;
        document.querySelector('[name=radio]').checked = true;
    ]=])
    fill() -- Both true and false saved values must be restored.
end

return T

-- vim: et:sw=4:ts=8:sts=4:tw=80
