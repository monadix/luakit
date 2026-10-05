--- Test Linux sandbox grants with isolated user web modules and profiles.
-- @copyright 2026 Luakit contributors

local T = {}
local test = require("tests.lib")
local util = require("tests.util")
local lfs = require("lfs")

local function quote(s) return "'" .. s:gsub("'", "'\\''") .. "'" end
local function write(path, text)
    local f = assert(io.open(path, "w"))
    f:write(text)
    f:close()
end

local function run_profile(profile, symlink)
    local base = assert(util.make_tmp_dir("luakit-sandbox-XXXXXX"))
    local config = base .. "/config/luakit" .. (profile and "/" .. profile or "")
    assert(os.execute("mkdir -p " .. quote(config) .. " " .. quote(base .. "/writable")) == 0)
    if symlink then
        assert(os.rename(config, base .. "/real-config"))
        assert(os.execute("ln -s " .. quote(base .. "/real-config") .. " " .. quote(config)) == 0)
    end
    write(base .. "/unrelated", "must not be readable")
    local module = [=[
local ui = ipc_channel("isolated_wm")
local uri = require("lousy.uri")
local lfs = require("lfs")
local registered_called = false
luakit.register_function(".*", "isolated_registered", function (_, resolve)
    registered_called = true
    resolve("registered")
end)
ui:add_signal("ping", function (_, page)
    local denied = io.open(DENIED, "r")
    local readonly = io.open(READONLY, "a")
    local writable = io.open(WRITABLE, "w")
    local ok = denied == nil and readonly == nil and writable ~= nil and registered_called
    if denied then denied:close() end
    if readonly then readonly:close() end
    if writable then writable:write("explicit grant"); writable:close() end
    ui:emit_signal(page, "pong", ok, lfs.attributes(READONLY, "mode"), uri.parse("https://example.com/").host)
end)
]=]
    module = module:gsub("DENIED", string.format("%q", base .. "/unrelated"))
        :gsub("READONLY", string.format("%q", config .. "/isolated_wm.lua"))
        :gsub("WRITABLE", string.format("%q", base .. "/writable/result"))
    write(config .. "/isolated_wm.lua", module)
    local rc = [=[
local wm = require_web_module("isolated_wm")
for _, path in ipairs({"/", "relative", "/no-such-luakit-path", luakit.data_dir, luakit.cache_dir}) do
    assert(not pcall(luakit.add_path_to_sandbox, path))
end
luakit.add_path_to_sandbox(WRITEDIR, false)
-- WebKit already exposes /etc read-only; rebinding a symlink there can fail
-- when its target or parents are on the read-only NixOS store mount.
if require("lfs").attributes("/etc/os-release") then
    luakit.add_path_to_sandbox("/etc/os-release")
end
local view = widget{ type = "webview" }
local window = widget{ type = "window" }
window.child = view
assert(not pcall(luakit.add_path_to_sandbox, WRITEDIR))
local waiting = true
wm:add_web_signal("pong", {
    validate = function (_, _, ok, mode, host, ...)
        return select("#", ...) == 0 and type(ok) == "boolean" and type(mode) == "string" and type(host) == "string"
    end,
    authorize = function (_, originating) return waiting and originating == view end,
}, function (_, _, ok, mode, host)
    waiting = false
    assert(ok and mode == "file" and host == "example.com")
    local f = assert(io.open(RESULT, "w")); f:write("passed"); f:close()
    luakit.quit(0)
end)
view:add_signal("load-status", function (v, status)
    if status == "finished" then
        v:eval_js("isolated_registered(); typeof isolated_registered", { callback = function (value)
            assert(value == "function")
            wm:emit_signal(v, "ping")
        end })
    end
end)
view.uri = "about:blank"
window:show()
local timeout = timer{ interval = 10000 }
timeout:add_signal("timeout", function () luakit.quit(1) end)
timeout:start()
]=]
    rc = rc:gsub("WRITEDIR", string.format("%q", base .. "/writable"))
        :gsub("RESULT", string.format("%q", base .. "/passed"))
    write(config .. "/rc.lua", rc)
    local cmd = "env XDG_CONFIG_HOME=" .. quote(base .. "/config")
        .. " XDG_DATA_HOME=" .. quote(base .. "/data") .. " XDG_CACHE_HOME=" .. quote(base .. "/cache")
        .. " " .. quote(lfs.currentdir() .. "/luakit") .. " -U -c " .. quote(config .. "/rc.lua")
        .. (profile and " --profile " .. quote(profile) or "") .. " > " .. quote(base .. "/log") .. " 2>&1"
    luakit.spawn("sh -c " .. quote(cmd))
    test.wait_until(function () return lfs.attributes(base .. "/passed") ~= nil end, 20, 15000)
    assert(lfs.attributes(base .. "/writable/result") ~= nil)
    assert(os.execute("rm -rf " .. quote(base)) == 0)
end

T.test_isolated_user_module_roundtrip_and_grants = function () run_profile() end
T.test_named_profile_module_roundtrip_and_grants = function () run_profile("testing") end
T.test_symlinked_config_module_roundtrip_and_grants = function () run_profile(nil, true) end

return T

-- vim: et:sw=4:ts=8:sts=4:tw=80
