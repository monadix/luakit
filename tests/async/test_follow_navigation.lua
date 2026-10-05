--- Navigation destinations returned by bundled follow brokers.
-- @copyright 2026 Luakit contributors

local T = {}
local test = require("tests.lib")
local util = require("tests.broker_util")
local broker = require("lousy.broker")
uris = { "about:blank" }
require("config.rc")
require("follow_selected")
local window = require("window")
local modes = require("modes")
local bind = require("lousy.bind")
local w = assert(select(2, next(window.bywidget)))
local serial = 0
local initial_document_ready = false
w.view:add_signal("load-status", function (_, status)
    if status == "finished" then initial_document_ready = true end
end)

local function wait_for_initial_document()
    test.wait_until(function () return initial_document_ready end)
end

local function send(channel, signal, ...)
    local args = { signal, ... }
    args.n = select("#", ...) + 2
    args[args.n] = channel
    util.inject(w.view, "lua_ipc", args)
end

local function follow(uri, callback, count)
    wait_for_initial_document()
    serial = serial + 1
    local operation = {
        id = serial, evaluator = "src", window = w, remaining = count or 1,
        mode = { func = callback }, clicks = 1,
    }
    broker.state(w.view).follow_operation = operation
    send("follow_wm", "follow_func", serial, uri)
    return operation
end

T.test_embedded_and_generated_images_remain_followable = function ()
    local original = luakit.confirm
    luakit.confirm = function () error("safe destination prompted") end
    for _, uri in ipairs({ "data:image/png;base64," .. string.rep("A", 9000),
        "blob:https://example.com/image", "about:blank" }) do
        local result
        follow(uri, function (value) result = value end)
        assert(result == uri)
    end
    local calls = 0
    follow("javascript:alert(1)", function () calls = calls + 1 end)
    follow("custom:target", function () calls = calls + 1 end)
    follow("http:target", function () calls = calls + 1 end)
    assert(calls == 0)
    broker.state(w.view).follow_operation = nil
    luakit.confirm = original
end

T.test_privileged_follow_requires_exact_approval_and_blocks_modal_replay = function ()
    local original = luakit.confirm
    for _, uri in ipairs({ "file:///tmp/image.png", "luakit://downloads/", "custom://target" }) do
        for _, approved in ipairs({ false, true }) do
            local calls, prompts = 0, 0
            luakit.confirm = function (view, operation, target)
                assert(view == w.view and operation == "Open privileged page" and target == uri)
                prompts = prompts + 1
                send("follow_wm", "follow_func", serial, uri)
                send("follow_wm", "finished", serial)
                return approved
            end
            follow(uri, function () calls = calls + 1 end, 2)
            assert(prompts == 1 and calls == (approved and 1 or 0))
            broker.state(w.view).follow_operation = nil
        end
    end
    luakit.confirm = original
end

T.test_follow_approval_does_not_survive_document_invalidation = function ()
    local original = luakit.confirm
    local calls = 0
    luakit.confirm = function ()
        w.view:emit_signal("load-status", "provisional")
        return true
    end
    follow("file:///tmp/image.png", function () calls = calls + 1 end)
    assert(calls == 0)
    luakit.confirm = original
end

T.test_selected_link_consumes_permit_before_confirmation = function ()
    wait_for_initial_document()
    local original_confirm, original_navigate = luakit.confirm, w.navigate
    local calls, prompts = 0, 0
    local uri = "file:///tmp/selected.html"
    w.navigate = function (_, target) assert(target.uri == uri); calls = calls + 1 end
    luakit.confirm = function (_, _, target)
        assert(target == uri)
        prompts = prompts + 1
        send("follow_selected_wm", "navigate", uri)
        return true
    end
    broker.state(w.view).selected = "navigate"
    send("follow_selected_wm", "navigate", uri)
    assert(prompts == 1 and calls == 1)
    luakit.confirm = function ()
        w.view:emit_signal("load-status", "provisional")
        return true
    end
    broker.state(w.view).selected = "navigate"
    send("follow_selected_wm", "navigate", uri)
    assert(calls == 1)
    w.navigate, luakit.confirm = original_navigate, original_confirm
end

T.test_bundled_image_follow_navigates_to_the_exact_data_uri = function ()
    wait_for_initial_document()
    w:navigate(test.http_server() .. "broker.html")
    test.wait_for_view(w.view)
    local uri = "data:image/svg+xml,<svg xmlns='http:%2F%2Fwww.w3.org/2000/svg' width='32' height='32'></svg>"
    w.view:eval_js("document.body.innerHTML = '<img>'; document.querySelector('img').src = "
        .. string.format("%q", uri), { callback = test.continue })
    local _, err = test.wait()
    assert(not err, err)
    test.wait_until(function ()
        w.view:eval_js("document.querySelector('img').complete && document.querySelector('img').naturalWidth > 0",
            { callback = test.continue })
        return test.wait()
    end)
    -- A string passed through search_open would split this data URI on spaces.
    local original = w.search_open
    w.search_open = function () error("renderer URI passed through search_open") end
    w:set_mode("ex-follow")
    bind.hit(w, modes.get_mode("ex-follow").binds, {}, "i", {})
    test.wait_until(function () return broker.state(w.view).hints == 1 end)
    bind.hit(w, modes.get_mode("follow").binds, {}, "Return", {})
    test.wait_for_view(w.view)
    assert(w.view.uri == uri)
    w.search_open = original
end

return T

-- vim: et:sw=4:ts=8:sts=4:tw=80
