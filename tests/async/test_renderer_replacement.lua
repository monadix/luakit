--- Test IPC initialization during real cross-site process swaps.
-- @copyright 2026 Luakit contributors

local T = {}
local test = require("tests.lib")
local util = require("tests.util")
local hostile = require("tests.broker_util")
local broker = require("lousy.broker")
local path = assert(util.make_tmp_dir("luakit_test_renderer_XXXXXX"))
local f = assert(io.open(path .. "/renderer_wm.lua", "w"))
f:write([=[
local ui = ipc_channel("renderer_wm")
local instance = tostring(luakit.time()) .. tostring({})
ui:add_signal("ping", function (_, page)
    ui:emit_signal(page, "pong", instance)
end)
]=])
f:close()
luakit.add_path_to_sandbox(path)
package.path = path .. "/?.lua;" .. package.path
local channel = require_web_module("renderer_wm")
local notifications, loaded, pid = {}, {}, nil
luakit.add_signal("web-extension-created", function (view)
    notifications[view] = (notifications[view] or 0) + 1
end)
channel:add_web_signal("pong", broker.policy({ "string" }, function () return true end), function (_, _, value)
    pid = value
end)
uris = { "about:blank" }
require("config.rc")
local window = require("window")
local w = assert(select(2, next(window.bywidget)))
w.view:add_signal("web-extension-loaded", function (view) loaded[view] = (loaded[view] or 0) + 1 end)

local function ping()
    pid = nil
    channel:emit_signal(w.view, "ping")
    test.wait_until(function () return pid ~= nil end)
    return pid
end

T.test_cross_site_replacement_reinitializes_once_and_preserves_routes = function ()
    test.wait_until(function () return notifications[w.view] ~= nil and not w.view.is_loading end)
    assert(hostile.process_swap_enabled(w.view), "run this test with tests/process_swap.so preloaded")
    local port = hostile.http_server_start()
    local first = "http://127.0.0.1:" .. port .. "/first"
    local second = "http://localhost:" .. port .. "/second"
    w:navigate(first)
    test.wait_for_view(w.view)
    local old_instance, old_id = ping(), w.view.id
    local count, load_count = notifications[w.view], loaded[w.view]
    broker.state(w.view).replacement_test = true
    w:navigate(second)
    test.wait_for_view(w.view)
    assert(ping() ~= old_instance, "test navigation did not replace the renderer")
    assert(w.view.id ~= old_id)
    assert(notifications[w.view] == count + 1 and loaded[w.view] == load_count + 1)
    assert(broker.state(w.view).replacement_test == nil)
    local generation = hostile.endpoint_generation(w.view)
    hostile.inject(w.view, "page_created", { w.view.web_process_id })
    hostile.inject(w.view, "page_created", { w.view.web_process_id }, -1)
    assert(notifications[w.view] == count + 1 and hostile.endpoint_generation(w.view) == generation)
    w:navigate(second .. "/same-site")
    test.wait_for_view(w.view)
    assert(ping() ~= old_instance and notifications[w.view] == count + 1)
    hostile.http_server_stop()
    assert(os.remove(path .. "/renderer_wm.lua"))
    assert(require("lfs").rmdir(path))
end

return T

-- vim: et:sw=4:ts=8:sts=4:tw=80
