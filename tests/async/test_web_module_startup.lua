--- Test page-created delivery after the page transport is initialized.
-- @copyright 2026 Luakit contributors

local T = {}
local test = require("tests.lib")
local util = require("tests.util")
local path = assert(util.make_tmp_dir("luakit_test_startup_XXXXXX"))
local f = assert(io.open(path .. "/startup_wm.lua", "w"))
f:write([=[
local ui = ipc_channel("startup_wm")
msg.error("startup_wm diagnostic before connection")
luakit.add_signal("page-created", function (page)
    ui:emit_signal(page, "created", luakit.web_process_id)
    ui:emit_signal("legacy-created", page.id, luakit.web_process_id)
end)
]=])
f:close()
luakit.add_path_to_sandbox(path)
package.path = path .. "/?.lua;" .. package.path
local channel = require_web_module("startup_wm")
local notifications = {}
local legacy_notifications = {}
local startup_diagnostic = false
msg.add_signal("log", function (_, _, _, text)
    if text == "startup_wm diagnostic before connection" then startup_diagnostic = true end
end)
channel:add_signal("legacy-created", function (_, id, pid)
    assert(not legacy_notifications[id])
    legacy_notifications[id] = pid
end, {
    legacy_page_arg = 1,
    validate = function (_, view, id, pid, ...)
        return id == view.id and type(pid) == "number" and select("#", ...) == 0
    end,
    authorize = function () return true end,
})
channel:add_web_signal("created", {
    validate = function (_, _, pid, ...) return type(pid) == "number" and select("#", ...) == 0 end,
    authorize = function () return true end,
}, function (_, view, pid)
    assert(not notifications[view])
    notifications[view] = pid
end)
uris = { "about:blank" }
require("config.rc")
local window = require("window")
local w = assert(select(2, next(window.bywidget)))

T.test_related_pages_can_send_from_page_created = function ()
    test.wait_for_view(w.view)
    test.wait_until(function () return startup_diagnostic end)
    local first = w.view
    test.wait_until(function () return notifications[first] ~= nil end)
    assert(notifications[first] > 0 and first.web_process_id == notifications[first])
    first.javascript_can_open_windows_automatically = true
    first:eval_js('window.open("about:blank")', { no_return = true })
    test.wait_until(function () return #w.tabs.children == 2 end)
    local second = w.tabs.children[1] == first and w.tabs.children[2] or w.tabs.children[1]
    test.wait_until(function () return notifications[second] ~= nil end)
    assert(second.web_process_id == notifications[second])
    assert(notifications[first] == notifications[second])
    test.wait_until(function () return legacy_notifications[second.id] ~= nil end)
    assert(legacy_notifications[first.id] == notifications[first])
    assert(legacy_notifications[second.id] == notifications[second])
    assert(os.remove(path .. "/startup_wm.lua"))
    assert(require("lfs").rmdir(path))
end

return T

-- vim: et:sw=4:ts=8:sts=4:tw=80
