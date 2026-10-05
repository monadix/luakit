--- Optional-window download opening retains exact trusted confirmation.
-- @copyright 2026 Luakit contributors

local T = {}
uris = { "about:blank" }
require("config.rc")
local downloads = require("downloads")
local window = require("window")
local w = assert(select(2, next(window.bywidget)))

local function upvalue(func, wanted)
    for i = 1, 30 do
        local name, value = debug.getupvalue(func, i)
        if name == wanted then return value end
    end
    error("missing upvalue " .. wanted)
end

T.test_optional_window_opening_confirms_finished_and_deferred_downloads = function ()
    local records = upvalue(downloads.do_open, "dls")
    local status_timer = upvalue(downloads.add, "status_timer")
    local d = { status = "finished", destination = "/tmp/windowless-download", mime_type = "application/pdf" }
    local data = { id = "windowless-test" }
    records[d] = data
    local original_confirm, original_emit = luakit.confirm, downloads.emit_signal
    local approved, prompts, opens = false, 0, 0
    luakit.confirm = function (view, operation, target)
        assert(view == w.view and operation == "Open downloaded file" and target == d.destination)
        prompts = prompts + 1
        return approved
    end
    downloads.emit_signal = function (signal, target, mime, opening_window)
        if signal == "open-file" then
            assert(target == d.destination and mime == d.mime_type and opening_window == w)
            opens = opens + 1
            return true
        end
    end
    downloads.do_open(d)
    assert(prompts == 1 and opens == 0)
    approved = true
    downloads.open(data.id)
    assert(prompts == 2 and opens == 1)
    -- A vanished originating window also falls back to a current live window.
    data.window = {}
    downloads.do_open(d)
    assert(prompts == 3 and opens == 2)
    downloads.do_open(d, { win = {}, view = w.view })
    assert(prompts == 3 and opens == 2)
    for _, decision in ipairs({ false, true }) do
        approved = decision
        d.status, data.last_status = "started", "started"
        downloads.open(data.id)
        assert(data.opening)
        d.status = "finished"
        status_timer:start()
        status_timer:emit_signal("timeout")
        assert(not data.opening and data.opening_window == nil)
    end
    assert(prompts == 5 and opens == 3)
    records[d] = nil
    luakit.confirm, downloads.emit_signal = original_confirm, original_emit
end

return T

-- vim: et:sw=4:ts=8:sts=4:tw=80
