--- Test explicit errors when a starting renderer's IPC queue is full.
-- @copyright 2026 Luakit contributors

local T = {}
local util = require("tests.broker_util")

T.test_full_queue_rejects_calls_without_retaining_callbacks = function ()
    local view = widget{ type = "webview" }
    local channel = ipc_channel("queue_test")
    for _ = 1, 255 do channel:emit_signal(view, "queued") end
    assert(util.endpoint_queue_size(view) == 256)
    local weak = setmetatable({}, { __mode = "v" })
    do
        local captured = {}
        weak[1] = captured
        local ok, err = pcall(view.eval_js, view, "1", {
            callback = function () return captured end,
        })
        assert(not ok and err:match("IPC queue is full"))
    end
    collectgarbage("collect")
    assert(weak[1] == nil)
    local ok, err = pcall(channel.emit_signal, channel, view, "overflow")
    assert(not ok and err:match("IPC queue is full"))
    assert(util.endpoint_queue_size(view) == 256)
    view:destroy()
end

T.test_channel_reserves_room_for_updated_routes = function ()
    local view = widget{ type = "webview" }
    local channel = ipc_channel("queue_routes_test")
    for _ = 1, 254 do channel:emit_signal(view, "queued") end
    assert(util.endpoint_queue_size(view) == 255)
    channel:add_signal("reply", function () end, {
        legacy_page_arg = 1,
        validate = function () return true end,
        authorize = function () return true end,
    })
    local ok, err = pcall(channel.emit_signal, channel, view, "overflow")
    assert(not ok and err:match("IPC queue is full"))
    assert(util.endpoint_queue_size(view) == 255)
    view:eval_js("1", { no_return = true })
    assert(util.endpoint_queue_size(view) == 256)
    channel:remove_signals("reply")
    view:destroy()
end

return T

-- vim: et:sw=4:ts=8:sts=4:tw=80
