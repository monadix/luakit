--- Test synchronous and deferred permission decisions without capture hardware.
-- @copyright 2026 Luakit contributors

local T = {}
local test = require("tests.lib")
local util = require("tests.broker_util")
local window = widget { type = "window" }
window:show()

local function request(handler)
    local port = util.http_server_start()
    local view = widget { type = "webview" }
    window.child = view
    util.mock_geolocation(view)
    if handler then view:add_signal("permission-request", handler) end
    view.uri = "http://127.0.0.1:" .. port .. "/permission"
    test.wait_for_view(view)
    view:eval_js([[
        window.permissionResult = "pending";
        navigator.geolocation.getCurrentPosition(function () {
            window.permissionResult = "granted";
        }, function (error) {
            window.permissionResult = error.code === 1 ? "denied" : "error";
        });
    ]], { no_return = true })
    return view
end

local function result(view, expected)
    local value
    test.wait_until(function ()
        view:eval_js("window.permissionResult", { callback = function (v) value = v end })
        return value == expected
    end)
end

local function close(view)
    view:destroy()
    util.http_server_stop()
end

T.test_synchronous_and_default_decisions = function ()
    for _, decision in ipairs { true, false } do
        local view = request(function (_, kind)
            assert(kind == "geolocation")
            return decision
        end)
        result(view, decision and "granted" or "denied")
        close(view)
    end
    local view = request()
    result(view, "denied")
    close(view)
end

T.test_deferred_decisions_are_one_shot = function ()
    for _, decision in ipairs { true, false } do
        local respond
        local view = request(function ()
            return function (callback) respond = callback end
        end)
        test.wait_until(function () return respond ~= nil end)
        result(view, "pending")
        assert(not pcall(respond, "yes"))
        assert(respond(decision))
        assert(not respond(not decision))
        result(view, decision and "granted" or "denied")
        close(view)
    end
end

T.test_immediate_responder = function ()
    local view = request(function ()
        return function (respond) assert(respond(true)) end
    end)
    result(view, "granted")
    close(view)
end

T.test_abandoned_responder_denies = function ()
    local called = false
    local view = request(function ()
        return function () called = true end
    end)
    test.wait_until(function () return called end)
    collectgarbage("collect")
    result(view, "denied")
    close(view)
end

T.test_error_denies_even_if_responder_is_retained = function ()
    local respond
    local view = request(function ()
        return function (callback)
            respond = callback
            error("expected permission callback failure")
        end
    end)
    result(view, "denied")
    assert(respond and not respond(true))
    close(view)
end

T.test_navigation_expires_responder = function ()
    local respond
    local view = request(function ()
        return function (callback) respond = callback end
    end)
    test.wait_until(function () return respond ~= nil end)
    view.uri = "about:blank"
    test.wait_for_view(view)
    assert(not respond(true))
    close(view)
end

T.test_destroy_expires_responder = function ()
    local respond
    local view = request(function ()
        return function (callback) respond = callback end
    end)
    test.wait_until(function () return respond ~= nil end)
    close(view)
    collectgarbage("collect")
    assert(not respond(true))
end

T.test_renderer_termination_expires_responder = function ()
    local respond
    local view = request(function ()
        return function (callback) respond = callback end
    end)
    test.wait_until(function () return respond ~= nil end)
    local crashed = false
    view:add_signal("crashed", function () crashed = true end)
    util.terminate_process(view)
    test.wait_until(function () return crashed end)
    assert(not respond(true))
    close(view)
end

T.test_view_destroyed_during_handler = function ()
    for _, deferred in ipairs { false, true } do
        local destroyed, continued = false, false
        request(function (view)
            view:destroy()
            destroyed = true
            if deferred then
                return function () continued = true end
            end
            return true
        end)
        test.wait_until(function () return destroyed end)
        assert(not continued)
        util.http_server_stop()
        collectgarbage("collect")
    end
end

T.test_view_destroyed_during_deferred_setup = function ()
    local done = false
    request(function (view)
        return function (respond)
            view:destroy()
            assert(not respond(true))
            done = true
        end
    end)
    test.wait_until(function () return done end)
    util.http_server_stop()
    collectgarbage("collect")
end

return T

-- vim: et:sw=4:ts=8:sts=4:tw=80
