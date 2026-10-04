--- Add {A,C,S,}-Return binds to follow selected link (or link in selection).
--
-- This module allows you to follow links that are part of the currently
-- selected text. This is useful as an alternative to the follow mode: search
-- for the text of the link, and then press `<Return>` to follow it.
--
-- @module follow_selected
-- @copyright 2010 Chris van Dijk <quigybo@hotmail.com>
-- @copyright 2010 Mason Larobina <mason.larobina@gmail.com>
-- @copyright 2010 Paweł Zuzelski <pawelz@pld-linux.org>
-- @copyright 2009 israellevin

local window = require("window")
local webview = require("webview")
local broker = require("lousy.broker")
local modes = require("modes")
local add_binds = modes.add_binds

local _M = {}

local wm = require_web_module("follow_selected_wm")

local function request(w, action)
    broker.state(w.view).selected = action
    wm:emit_signal(w.view, "follow_selected", action)
end

for _, action in ipairs({ "navigate", "new_tab", "new_window", "download" }) do
    wm:add_web_signal(action, broker.policy({ "uri" }, function (_, view)
        return broker.state(view).selected == action and webview.window(view) ~= nil
    end), function (_, view, uri)
        broker.state(view).selected = nil
        local w = webview.window(view)
        if action == "new_window" then window.new({uri})
        elseif action == "navigate" then w:navigate(uri)
        elseif action == "new_tab" then w:new_tab(uri)
        else w:download(uri) end
    end)
end

-- Add binding to normal mode to follow selected link
add_binds("normal", {
    { "<Return>", "Follow the selected link in the current tab.",
        function (w) request(w, "navigate") end },
    { "<Control-Return>", "Follow the selected link in a new tab.",
        function (w) request(w, "new_tab") end },
    { "<Shift-Return>", "Follow the selected link in a new window.",
        function (w) request(w, "new_window") end },
    { "<Mod1-Return>", "Download the selected link.",
        function (w) request(w, "download") end },
})

return _M

-- vim: et:sw=4:ts=8:sts=4:tw=80
