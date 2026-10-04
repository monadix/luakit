--- Test hostile renderer broker messages and document lifetimes.
-- @copyright 2026 Luakit contributors

local T = {}
local test = require("tests.lib")
local util = require("tests.broker_util")
local broker = require("lousy.broker")
local view = widget{ type = "webview" }
local other = widget{ type = "webview" }
local channel = ipc_channel("security_test")
local calls, ordinary = 0, 0
channel:add_signal("reply", function () ordinary = ordinary + 1 end)
channel:add_web_signal("reply", broker.policy({ "id", "string?" }, function (_, v, id)
    return broker.state(v).pending == id
end), function (_, v)
    broker.state(v).pending = nil
    calls = calls + 1
end)

T.test_broker_rejects_unsolicited_stale_duplicate_cross_view_and_wrong_types = function ()
    broker.state(view).pending = 7
    local args = { "reply", 7, "hello", "security_test" }
    util.inject(other, "lua_ipc", args)
    util.inject(view, "lua_ipc", args, -1)
    util.inject(view, "lua_ipc", args, 0, "wrong")
    util.inject(view, "lua_ipc", args, 0, "pointer")
    util.inject(view, "lua_ipc", args, 0, "envelope")
    util.inject(view, "unknown", args)
    util.inject(view, "lua_trusted", args)
    util.inject(view, "lua_ipc", { "reply", "7", "hello", "security_test" })
    util.inject(view, "lua_ipc", { "reply", 7, "hello", "extra", "security_test" })
    assert(calls == 0 and ordinary == 0)
    util.inject(view, "lua_ipc", args)
    assert(calls == 1 and ordinary == 0)
    util.inject(view, "lua_ipc", args)
    assert(calls == 1)
end

T.test_renderer_fatal_logs_are_nonfatal_and_bounded = function ()
    util.inject(view, "log", { 0, "renderer", "fatal attempt" })
    util.inject(view, "log", { 0, "", "empty group" })
    util.inject(view, "log", { 0, "x", "short group" })
    util.inject(view, "log", { 999, "renderer", "invalid level" })
    util.inject(view, "log", { {}, "renderer", "invalid type" })
    for _ = 1, 110 do util.inject(view, "log", { 5, "renderer", "bounded" }) end
end

T.test_unknown_javascript_callbacks_are_rejected = function ()
    util.inject(view, "eval_js", { view.id, 9007199254740991, "forged" })
    util.inject(other, "eval_js", { view.id, 1, "cross-view" })
    util.inject(view, "eval_js", { view.id, {}, "pointer" })
end

T.test_confirmation_requires_ui_approval_and_expires_on_navigation = function ()
    util.confirm_response(false)
    assert(luakit.confirm(view, "Test operation", "/immutable/target") == false)
    util.confirm_response(true)
    assert(luakit.confirm(view, "Test operation", "/immutable/target") == true)
    view.uri = test.http_server() .. "scroll.html"
    test.wait_for_view(view)
    util.confirm_navigation(view)
    assert(luakit.confirm(view, "Test operation", "/immutable/target") == false)
end

T.test_pending_authorization_expires_on_navigation = function ()
    broker.state(view).pending = 8
    view.uri = test.http_server() .. "scroll.html"
    test.wait_for_view(view)
    util.inject(view, "lua_ipc", { "reply", 8, "stale", "security_test" })
    assert(broker.state(view).pending == nil)
end

T.test_process_termination_invalidates_authorization_and_can_restart = function ()
    view.uri = test.http_server() .. "scroll.html"
    test.wait_for_view(view)
    broker.state(view).pending = 9
    local crashed = false
    local handler = function () crashed = true end
    view:add_signal("crashed", handler)
    util.terminate_process(view)
    test.wait_until(function () return crashed end)
    view:remove_signal("crashed", handler)
    assert(broker.state(view).pending == nil)
    view.uri = test.http_server() .. "scroll.html"
    test.wait_for_view(view)
    local result
    view:eval_js("42", { callback = function (value) result = value end })
    test.wait_until(function () return result ~= nil end)
    assert(result == 42)
end

return T

-- vim: et:sw=4:ts=8:sts=4:tw=80
