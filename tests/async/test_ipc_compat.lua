--- Test compatibility adapters without granting ordinary handlers renderer access.
-- @copyright 2026 Luakit contributors

local T = {}
local test = require("tests.lib")
local util = require("tests.util")
local hostile = require("tests.broker_util")
local broker = require("lousy.broker")
local path = assert(util.make_tmp_dir("luakit_test_compat_XXXXXX"))
local f = assert(io.open(path .. "/compat_wm.lua", "w"))
f:write([=[
local ui = ipc_channel("compat_wm")
local last_page
luakit.add_signal("page-created", function (page)
    last_page = page
    ui:emit_signal("created", page.id)
end)
luakit.register_function(".*", "compat_context", function (page, resolve)
    ui:emit_signal("context", "javascript")
    resolve(true)
end)
ui:add_signal("run", function (_, page, evaluator)
    local ok = pcall(ui.emit_signal, ui, "first", page.id + 100000, "wrong")
    assert(not ok)
    ui:emit_signal("first", page.id, evaluator())
    ui:emit_signal("last", "last", page.id)
    ui:emit_signal("context", "callback")
    luakit.idle_add(function ()
        local ambiguous = pcall(ui.emit_signal, ui, "context", "idle")
        assert(not ambiguous)
        ui:emit_signal("first", page.id, "idle")
        ui:emit_signal("last", "idle", page.id)
    end)
end)
ui:add_signal("failure", function () error("intentional callback error") end)
ui:add_signal("broadcast", function (_, page)
    assert(page == nil)
    assert(not pcall(ui.emit_signal, ui, "context", "leaked"))
end)
ui:add_signal("broadcast-function", function (_, page, evaluator)
    assert(page == nil)
    ui:emit_signal(last_page, "broadcast-result", evaluator())
end)
ui:add_signal("snapshot", function (_, page, data)
    assert(getmetatable(data) == nil and data.raw == "raw")
    assert(not pcall(ui.emit_signal, ui, page, "snapshot", function () end))
    ui:emit_signal(page, "snapshot", setmetatable(data, { __index = function () error("metamethod") end }))
end)
ui:add_signal("late", function (_, page, index)
    luakit.idle_add(function ()
        ui:emit_signal("late-reply", index == 1 and page.id or "late", index == 1 and "late" or page.id)
    end)
end)
]=])
f:close()
luakit.add_path_to_sandbox(path)
package.path = path .. "/?.lua;" .. package.path
local channel = require_web_module("compat_wm")
local created = {}
local received = {}
local allowed
local function policy(index)
    return {
        legacy_page_arg = index,
        validate = function (_, view, a, b, ...)
            if select("#", ...) ~= 0 then return false end
            if index == 1 then return a == view.id and (b == nil or type(b) == "string") end
            return type(a) == "string" and b == view.id
        end,
        authorize = function (_, view) return allowed == view end,
    }
end
channel:add_signal("created", function (_, id) created[id] = (created[id] or 0) + 1 end, {
    legacy_page_arg = 1,
    validate = function (_, view, id, ...) return id == view.id and select("#", ...) == 0 end,
    authorize = function () return true end,
})
channel:add_signal("first", function (_, id, value) received[#received + 1] = { id, value } end, policy(1))
channel:add_signal("last", function (_, value, id) received[#received + 1] = { id, value } end, policy(2))
channel:add_signal("context", function (_, value) received[#received + 1] = { allowed.id, value } end, {
    validate = function (_, _, value, ...) return type(value) == "string" and select("#", ...) == 0 end,
    authorize = function (_, view) return allowed == view end,
})
local view = widget{ type = "webview" }
local other = widget{ type = "webview" }

T.test_legacy_routes_context_and_trusted_custom_functions = function ()
    view.uri = test.http_server() .. "scroll.html"
    test.wait_for_view(view)
    test.wait_until(function () return created[view.id] ~= nil end)
    assert(created[view.id] >= 1)
    allowed = view
    received = {}
    local value = "upvalue"
    channel:emit_signal(view, "run", function () return value end)
    test.wait_until(function () return #received == 5 end)
    assert(received[1][2] == "upvalue" and received[2][2] == "last" and received[3][2] == "callback")
    assert(received[4][2] == "idle" and received[5][2] == "idle")
    for _, result in ipairs(received) do assert(result[1] == view.id) end
    view:eval_js("compat_context()", { no_return = true })
    test.wait_until(function () return #received == 6 end)
    assert(received[6][2] == "javascript")
    channel:emit_signal(view, "failure")
    channel:emit_signal("broadcast")
    test.delay(100)
    assert(#received == 6)
    local broadcast_result
    channel:add_web_signal("broadcast-result", broker.policy({ "string" }, function (_, v) return v == view end),
        function (_, _, result_value) broadcast_result = result_value end)
    local broadcast_value = "broadcast upvalue"
    channel:emit_signal("broadcast-function", function () return broadcast_value end)
    test.wait_until(function () return broadcast_result ~= nil end)
    assert(broadcast_result == broadcast_value)
end

T.test_opt_in_handlers_removal_and_fail_closed_policies = function ()
    local c = ipc_channel("compat_security")
    local local_calls, calls = 0, 0
    local ordinary = function () local_calls = local_calls + 1 end
    local handler = function () calls = calls + 1 end
    local p = broker.policy({ "string" }, function () return true end)
    c:add_signal("reply", ordinary)
    c:add_signal("reply", handler, p)
    c:add_signal("reply", function () error("should reject") end,
        broker.policy({ "string" }, function () return false end))
    c:add_signal("reply", function () error("should reject") end,
        broker.policy({ "string" }, function () error("policy error") end))
    hostile.inject(view, "lua_ipc", { "reply", "ok", "compat_security" })
    assert(calls == 1 and local_calls == 0)
    assert(not pcall(c.add_web_signal, c, "reply", p, handler))
    assert(not pcall(c.add_signal, c, "reply", handler, policy(1)))
    c:remove_signal("reply", handler)
    hostile.inject(view, "lua_ipc", { "reply", "ok", "compat_security" })
    assert(calls == 1 and local_calls == 0)
    c:remove_signals("reply")
    c:add_signal("reply", ordinary)
    hostile.emit_local(c, "reply", { "ok" })
    assert(local_calls == 1)
    hostile.inject(view, "lua_ipc", { "reply", "ok", "compat_security" })
    assert(local_calls == 1)
    c:add_web_signal("modern", p, handler)
    assert(not pcall(c.add_signal, c, "modern", handler, p))
    c:add_signal("ids", handler, policy(1))
    hostile.inject(view, "lua_ipc", { "ids", other.id, "ok", "compat_security" })
    hostile.inject(other, "lua_ipc", { "ids", view.id, "ok", "compat_security" })
    hostile.inject(view, "lua_ipc", { "ids", view.id, "ok", "compat_security" }, -1)
    hostile.inject(view, "lua_routes", { 999, {} })
    hostile.inject(view, "lua_trusted", { "ids", view.id, "ok", "compat_security" })
    assert(calls == 1)
end

T.test_snapshot_preserves_raw_entries_without_metamethods = function ()
    view.uri = test.http_server() .. "scroll.html"
    test.wait_for_view(view)
    allowed = view
    local result
    channel:add_web_signal("snapshot", broker.policy({ "table" }, function (_, v) return v == view end),
        function (_, _, data) result = data end)
    channel:emit_signal(view, "snapshot", setmetatable({ raw = "raw" }, {
        __index = function () error("metamethod") end,
        __pairs = function () error("metamethod") end,
    }))
    test.wait_until(function () return result ~= nil end)
    assert(getmetatable(result) == nil and result.raw == "raw")
end

T.test_late_route_registration_and_replacement = function ()
    view.uri = test.http_server() .. "scroll.html"
    test.wait_for_view(view)
    allowed = view
    local count = 0
    local handler = function () count = count + 1 end
    channel:add_signal("late-reply", handler, policy(1))
    channel:emit_signal(view, "late", 1)
    test.wait_until(function () return count == 1 end)
    channel:remove_signal("late-reply", handler)
    channel:add_signal("late-reply", handler, policy(2))
    channel:emit_signal(view, "late", 2)
    test.wait_until(function () return count == 2 end)
    channel:remove_signals("late-reply")
end

T.test_duplicate_and_reentrant_handler_removal = function ()
    local c = ipc_channel("compat_remove")
    local calls = 0
    local handler = function () calls = calls + 1 end
    local p = broker.policy({}, function () return true end)
    local function inject() hostile.inject(view, "lua_ipc", { "reply", "compat_remove" }) end
    c:add_signal("reply", handler)
    c:add_signal("reply", handler, p)
    c:add_signal("reply", handler, p)
    c:remove_signal("reply", handler)
    inject()
    assert(calls == 2)
    c:remove_signal("reply", handler)
    inject()
    assert(calls == 3)
    c:remove_signal("reply", handler)
    inject()
    assert(calls == 3)
    c:add_signal("reply", handler, {
        validate = function () c:remove_signal("reply", handler); return true end,
        authorize = function () return true end,
    })
    inject()
    assert(calls == 3)
    c:add_signal("reply", handler, {
        validate = function () return true end,
        authorize = function () c:remove_signal("reply", handler); return true end,
    })
    inject()
    assert(calls == 3)
    c:add_signal("reply", function () c:remove_signal("reply", handler) end, p)
    c:add_signal("reply", handler, p)
    inject()
    assert(calls == 3)
    c:remove_signals("reply")
end

T.test_compatibility_routes_survive_renderer_restart = function ()
    view.uri = test.http_server() .. "scroll.html"
    test.wait_for_view(view)
    local crashed = false
    local handler = function () crashed = true end
    view:add_signal("crashed", handler)
    hostile.terminate_process(view)
    test.wait_until(function () return crashed end)
    view:remove_signal("crashed", handler)
    created = {}
    view.uri = test.http_server() .. "scroll.html"
    test.wait_for_view(view)
    test.wait_until(function () return created[view.id] ~= nil end)
    allowed = view
    received = {}
    channel:emit_signal(view, "run", function () return "restart" end)
    test.wait_until(function () return #received == 5 end)
    assert(received[1][2] == "restart")
end

T.test_navigation_stops_remaining_compat_handlers = function ()
    local c = ipc_channel("compat_navigation")
    local calls = 0
    local p = broker.policy({}, function () return true end)
    c:add_signal("reply", function ()
        calls = calls + 1
        hostile.confirm_navigation(view)
        luakit.confirm(view, "test", "target")
    end, p)
    c:add_signal("reply", function () calls = calls + 1 end, p)
    hostile.inject(view, "lua_ipc", { "reply", "compat_navigation" })
    assert(calls == 1)
    test.wait_until(function () return not view.is_loading end)
end

return T

-- vim: et:sw=4:ts=8:sts=4:tw=80
