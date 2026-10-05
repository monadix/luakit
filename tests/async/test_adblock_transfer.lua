--- Test bounded adblock snapshots, staging and view-bound acknowledgements.
-- @copyright 2026 Luakit contributors

local T = {}
local test = require("tests.lib")
local hostile = require("tests.broker_util")
local broker = require("lousy.broker")
local webview = require("webview")
uris = { "about:blank" }
require("config.rc")
local adblock = require("adblock")
local window = require("window")
local w = assert(select(2, next(window.bywidget)))

local function upvalue(func, wanted)
    for i = 1, 30 do
        local name, value = debug.getupvalue(func, i)
        if name == wanted then return value end
    end
    error("missing upvalue " .. wanted)
end
local sync = upvalue(adblock.load, "sync_rules")
local chunks_for = upvalue(sync, "rule_chunks")
local views = upvalue(sync, "views")

local function bucket()
    return { domains = {}, patterns = {}, plain = {}, ad_plain = {}, ad_patterns = {} }
end

local function snapshot()
    local black = bucket()
    black.domains["example.org"] = {}
    for i = 1, 18000 do
        black.domains["example.org"]["rule-" .. i] = {
            domain = { "example.org", "~other.org" }, ["third-party"] = false,
        }
    end
    black.plain["hello_world.html"] = {}
    return { subscription = { opts = { "Enabled" }, blacklist = black, whitelist = bucket() } }
end

local function ready()
    test.wait_until(function ()
        return views[w.view].initialized and broker.state(w.view).adblock_pending == nil
            and not webview.has_load_block(w.view)
    end, 20, 20000)
    assert(not webview.has_load_block(w.view))
end

T.test_large_snapshot_is_bounded_and_installed_atomically = function ()
    ready()
    local rules = snapshot()
    local chunks = chunks_for(rules)
    assert(#chunks > 1)
    local function values(value)
        local n = 1
        if type(value) == "table" then
            for k, v in pairs(value) do n = n + values(k) + values(v) end
        end
        return n
    end
    assert(values(rules) > 100000)
    local handlers, replies = {}, {}
    local channel = {
        add_signal = function (_, name, callback) handlers[name] = callback end,
        emit_signal = function (_, _, name, id, sequence) replies[#replies + 1] = { name, id, sequence } end,
    }
    local env = setmetatable({
        ipc_channel = function () return channel end,
        luakit = { add_signal = function () end },
    }, { __index = _G })
    setfenv(assert(loadfile("lib/adblock_wm.lua")), env)()
    local page = {}
    handlers.rules_begin(channel, page, 1)
    for i, chunk in ipairs(chunks) do
        local args = { "rules_chunk", 1, i, chunk, "adblock_wm", w.view.id }
        assert(values(args) - 1 - #args <= 4096)
        assert(hostile.encoded_size(args) <= 256 * 1024)
        handlers.rules_chunk(channel, page, 1, i, chunk)
        assert(next(upvalue(handlers.rules_commit, "rules")) == nil)
    end
    handlers.rules_commit(channel, page, 1, #chunks + 1)
    local installed = upvalue(handlers.rules_commit, "rules")
    require("luassert").are.same(rules, installed)
    assert(upvalue(handlers.rules_commit, "enabled_rules").subscription == installed.subscription)
    assert(replies[#replies][3] == #chunks + 1)
    -- An older transfer on another page cannot overwrite a newer snapshot.
    local older = {}
    handlers.rules_begin(channel, older, 1)
    handlers.rules_chunk(channel, older, 1, 1, { { {}, { obsolete = true } } })
    handlers.rules_begin(channel, page, 2)
    handlers.rules_chunk(channel, page, 2, 1, { { {}, {} } })
    handlers.rules_commit(channel, page, 2, 2)
    handlers.rules_commit(channel, older, 1, 2)
    assert(next(upvalue(handlers.rules_commit, "rules")) == nil)

    adblock.rules = rules
    sync()
    assert(broker.state(w.view).adblock_pending ~= nil)
    ready()
    local blocked = false
    local handler = function () blocked = true end
    w.view:add_signal("scheme-request::adblock-blocked", handler)
    w:navigate(test.http_server() .. "hello_world.html")
    test.wait_until(function () return blocked end)
    w.view:remove_signal("scheme-request::adblock-blocked", handler)
    adblock.rules = {}
    sync()
    ready()
end

T.test_stale_and_wrong_view_acknowledgements_do_not_advance = function ()
    ready()
    sync()
    local pending = assert(broker.state(w.view).adblock_pending)
    local other = widget{ type = "webview" }
    hostile.inject(w.view, "lua_ipc", { "rules_ack", pending.id + 1, 0, "adblock_wm" })
    hostile.inject(w.view, "lua_ipc", { "rules_ack", pending.id, 1, "adblock_wm" })
    hostile.inject(other, "lua_ipc", { "rules_ack", pending.id, 0, "adblock_wm" })
    assert(pending.sequence == 0 and webview.has_load_block(w.view))
    sync()
    assert(broker.state(w.view).adblock_pending.id ~= pending.id)
    hostile.inject(w.view, "lua_ipc", { "rules_ack", pending.id, 0, "adblock_wm" })
    assert(broker.state(w.view).adblock_pending.sequence == 0)
    ready()
    other:destroy()
end

T.test_timeout_and_oversized_record_remain_blocked_until_retry_or_disable = function ()
    ready()
    local original_error, errors = w.error, 0
    w.error = function () errors = errors + 1 end
    sync()
    local pending = assert(broker.state(w.view).adblock_pending)
    pending.timeout:emit_signal("timeout")
    assert(broker.state(w.view).adblock_pending == nil and webview.has_load_block(w.view))
    assert(errors == 1 and not pending.timeout.started)
    sync()
    ready()
    adblock.rules = { oversized = string.rep("x", 256 * 1024) }
    sync()
    assert(errors == 2 and webview.has_load_block(w.view))
    adblock.enabled = false
    assert(not webview.has_load_block(w.view))
    adblock.rules = {}
    adblock.enabled = true
    ready()
    w.error = original_error
end

T.test_navigation_and_process_loss_cancel_pending_transfers = function ()
    ready()
    sync()
    local first = assert(broker.state(w.view).adblock_pending)
    hostile.request_navigation(w.view, test.http_server() .. "scroll.html")
    test.wait_until(function ()
        local current = broker.state(w.view).adblock_pending
        return current == nil or current.id ~= first.id
    end)
    ready()
    assert(not first.timeout.started)
    sync()
    local interrupted = assert(broker.state(w.view).adblock_pending)
    local crashed = false
    local handler = function () crashed = true end
    w.view:add_signal("crashed", handler)
    hostile.terminate_process(w.view)
    test.wait_until(function () return crashed end)
    w.view:remove_signal("crashed", handler)
    assert(not interrupted.timeout.started and broker.state(w.view).adblock_pending == nil)
    w:navigate(test.http_server() .. "scroll.html")
    ready()
end

return T

-- vim: et:sw=4:ts=8:sts=4:tw=80
