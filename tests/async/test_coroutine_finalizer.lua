--- Object finalization cleans every signal in the calling coroutine.
-- @copyright 2026 Luakit contributors

local T = {}
local util = require("tests.broker_util")

T.test_finalizer_uses_coroutine_stack_and_releases_all_handlers = function ()
    local weak = setmetatable({}, { __mode = "v" })
    local obj
    do
        local first, second = function () end, function () end
        weak[1], weak[2] = first, second
        obj = util.new_signal_object(first, second)
    end
    local co = coroutine.create(function ()
        local empty, unchanged = util.finalize_signal_object(obj)
        assert(empty and unchanged)
    end)
    local ok, err = coroutine.resume(co)
    assert(ok, err)
    collectgarbage("collect")
    assert(weak[1] == nil and weak[2] == nil)
    -- The fixture's later automatic finalization must be harmless.
    obj = nil
    collectgarbage("collect")
end

return T

-- vim: et:sw=4:ts=8:sts=4:tw=80
