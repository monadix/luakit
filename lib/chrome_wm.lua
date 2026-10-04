-- Add custom luakit:// scheme rendering functions.
-- @submodule chrome
-- @copyright 2017 Aidan Holm <aidanholm@gmail.com>

local ui = ipc_channel("chrome_wm")

local _M = {}

local pending = {}
local next_id = 1
local registered = {}

ui:add_signal("function-return", function (_, _, id, ok, ret)
    local callbacks = pending[id]
    if not callbacks then return end
    pending[id] = nil
    (callbacks[ok and "resolve" or "reject"])(ret)
end)

ui:add_signal("register-function", function (_, _, page_name, func_name)
    local key = page_name .. ":" .. func_name
    if registered[key] then return end
    registered[key] = true
    local pattern = "^luakit://" .. page_name .. "/?(.*)"
    luakit.register_function(pattern, func_name, function (page, resolve, reject, ...)
        if next_id > 9007199254740991 then reject("request ID exhausted"); return end
        local args = { n = select("#", ...), ... }
        pending[next_id] = { resolve = resolve, reject = reject }
        ui:emit_signal(page, "function-call", page_name, func_name, next_id, args)
        next_id = next_id + 1
    end)
end)

return _M

-- vim: et:sw=4:ts=8:sts=4:tw=80
