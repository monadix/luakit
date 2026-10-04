--- Link hinting for luakit.
--
-- Link hints allow interacting with web pages without the use of a
-- mouse. When `follow` mode is entered, all clickable elements are
-- highlighted and labeled with a short number. Typing either an element
-- number or part of the element text will "follow" that hint, issuing a
-- mouse click. This is most commonly used to click links without using
-- the mouse and focus text input boxes. In addition, the `ex-follow`
-- mode offers several variations on this behavior. For example, instead
-- of clicking, the URI of a followed link can be copied into the clipboard.
-- Another example would be hinting all images on the page, and opening the
-- followed image in a new tab.
--
-- # Customizing hint labels
--
-- If you prefer to use letters instead of numbers for hint labels (useful if
-- you use a non-qwerty keyboard layout), this can be done by replacing the
-- @ref{label_maker} function:
--
--     local select = require "select"
--
--     select.label_maker = function (s)
--         local chars = s.charset("asdfqwerzxcv")
--         return s.trim(s.sort(s.reverse(chars)))
--     end
--
-- Here, the `charset()` function generates hints using the specified letters.
-- For a full explanation of what the `trim(sort(reverse(...)))` construction
-- does, see the @ref{select} module documentation; the short explanation is
-- that it makes hints as short as possible, saving you typing.
--
-- Note: this requires modifying the @ref{select} module because the actual
-- link hinting interface is implemented in the `select` module; the
-- `follow` module provides the `follow` and `ex-follow` user interface on top
-- of that.
--
-- ## Hinting with non-latin letters
--
-- If you use a keyboard layout with non-latin keys, you may prefer to use
-- non-latin letters to hint. For example, using the Cyrillic alphabet, the
-- above code could be changed to the following:
--
--     ...
--     local chars = s.charset("ФЫВАПРОЛДЖЭ")
--     ...
--
-- ## Hint text direction
--
-- Hints consisting entirely of characters which are drawn Left-to-Right
-- (eg Latin, Cyrillic) or characters drawn Right-to-Left (eg Arabic, Hebrew),
-- will render intuitively in the appropriate direction.
-- Hints will be drawn non-intuitively if they contain a mix of Left-to-Right
-- and Right-to-Left characters.
--
-- Punctuation characters do not have an intrinsic direction, and will be drawn
-- using the direction specified by the HTML/CSS context in which they appear.
-- This leads to corner cases if the hint charset contains punctuation characters,
-- for example:
--
--     ...
--     local chars = s.charset("fjdksla;ghutnvir")
--     ...
--
-- In this case, hints will display intuitively if used on pages which are
-- drawn Left-to-Right, but not on pages drawn Right-to-Left.
--
-- To guard against this, it is recommended that if punctuation characters
-- are used in hints, a clause should be added to a user stylesheet giving
-- an explicit text direction eg:
--
--     ...
--     #luakit_select_overlay .hint_label { direction: ltr; }
--     ...
--
-- ## Alternating between left- and right-handed letters
--
-- To make link hints easier to type, you may prefer to have them alternate
-- between letters on the left and right side of your keyboard. This is easy to
-- do with the `interleave()` label composer function.
--
--     ...
--     local chars = s.interleave("qwertasdfgzxcvb", "yuiophjklnm")
--     ...
--
-- # Matching only hint labels, not element text
--
-- If you prefer not to match element text, and wish to select hints only by
-- their label, this can be done by specifying the @ref{pattern_maker}:
--
--     -- Match only hint label text
--     follow.pattern_maker = follow.pattern_styles.match_label
--
-- # Ignoring element text case
--
-- To ignore element text case when filtering hints, set the following option:
--
--     -- Uncomment if you want to ignore case when matching
--     follow.ignore_case = true
--
-- @module follow
-- @copyright 2010-2012 Mason Larobina <mason.larobina@gmail.com>
-- @copyright 2010-2011 Fabian Streitel <karottenreibe@gmail.com>

local window = require("window")
local new_mode = require("modes").new_mode
local modes = require("modes")
local add_binds = modes.add_binds
local lousy = require("lousy")
local theme = lousy.theme.get()

local _M = {}

local broker = require("lousy.broker")

local follow_wm = require_web_module("follow_wm")

--- Duration to ignore keypresses after following a hint. 200ms by default.
--
-- After each follow ignore all keys pressed by the user to prevent the
-- accidental activation of other key bindings.
-- @type number
-- @readwrite
_M.ignore_delay = 200

--- CSS applied to the follow mode overlay.
-- @type string
-- @readwrite
_M.stylesheet = [[
#luakit_select_overlay {
    position: absolute;
    left: 0;
    top: 0;
    z-index: 2147483647; /* Maximum allowable on WebKit */
}

#luakit_select_overlay .hint_overlay {
    display: block;
    position: absolute;
    background-color: ]] .. (theme.hint_overlay_bg     or "rgba(255,255,153,0.3)") .. [[;
    border:           ]] .. (theme.hint_overlay_border or "1px dotted #000")       .. [[;
    opacity:          ]] .. (theme.hint_opacity        or "0.3")                   .. [[;
}

#luakit_select_overlay .hint_label {
    display: block;
    position: absolute;
    background-color: ]] .. (theme.hint_bg     or "#000088")                             .. [[;
    border:           ]] .. (theme.hint_border or "1px dashed #000")                     .. [[;
    color:            ]] .. (theme.hint_fg     or "#fff")                                .. [[;
    font:             ]] .. (theme.hint_font   or "10px monospace, courier, sans-serif") .. [[;
}

#luakit_select_overlay .hint_selected {
    background-color: ]] .. (theme.hint_overlay_selected_bg     or "rgba(0,255,0,0.3)") .. [[ !important;
    border:           ]] .. (theme.hint_overlay_selected_border or "1px dotted #000")   .. [[;
}
]]

-- Lua regex escape function
local function regex_escape(s)
    local escape_chars = "%^$().[]*+-?"
    local escape_pat = '([' .. escape_chars:gsub("(.)", "%%%1") .. '])'
    return s:gsub(escape_pat, "%%%1")
end

local re_match_text = function (text) return nil, text end
local re_match_both = function (text) return text, text end
local match_label_re_text = function (text)
    return #text > 0 and "^"..regex_escape(text) or "", text
end
local match_label = function (text)
    return #text > 0 and "^"..regex_escape(text) or "", nil
end

--- Table of functions used to select a hint matching style.
-- @type {[string]=function}
-- @readonly
_M.pattern_styles = {
    re_match_text = re_match_text, -- Regex match target text only.
    re_match_both = re_match_both, -- Regex match both hint label or target text
    match_label_re_text = match_label_re_text, -- String match hint label & regex match text
    match_label = match_label, -- String match hint label only
}

--- Hint matching style functions.
-- @type function
-- @readwrite
_M.pattern_maker = _M.pattern_styles.match_label_re_text

--- Whether text case should be ignored in follow mode. True by default.
-- @type boolean
-- @readwrite
_M.ignore_case = true

local function focus(w, step)
    follow_wm:emit_signal(w.view, "focus", step)
end

local hit_nop = function () return true end

local function ignore_keys(w)
    local delay = _M.ignore_delay
    if not delay or delay == 0 then return end
    -- Replace w:hit(..) with a no-op
    w.hit = hit_nop
    local timer = timer{ interval = delay }
    timer:add_signal("timeout", function (t)
        t:stop()
        w.hit = nil
    end)
    timer:start()
end

local function do_follow(w, all)
    local state = broker.state(w.view)
    state.follow_replies = all and math.min(state.hints or 1, 1000) or 1
    state.follow_clicks = state.follow_replies
    follow_wm:emit_signal(w.view, "follow", all)
end

local function follow_all_hints(w)
    do_follow(w, true)
end

local function follow_func_cb(w, ret)
    local mode = w.follow_state.mode

    if mode.func then mode.func(ret) end

    -- don't set mode if func() changed it (e.g. to command mode)
    if w:is_mode("follow") or w:is_mode("ex-follow") then
        if mode.persist then
            w:set_input("")
            w:set_mode("follow", mode)
        elseif ret ~= "form-active" and ret ~= "root-active" then
            w:set_mode()
        end
    end

    ignore_keys(w)
end

local function matches_cb(w, n)
    w:set_ibar_theme(n > 0 and "ok" or "error")
end

local function active_follow(_, view)
    local w = require("webview").window(view)
    return w ~= nil and w.follow_state ~= nil and w.follow_state.view == view
        and broker.state(view).follow_active == true
end

follow_wm:add_web_signal("follow_func", broker.policy({ function (ret)
    return ret == nil or type(ret) == "string" and #ret <= 8192 and not ret:find("%z")
end }, function (channel, view, ret)
    if not active_follow(channel, view) or (broker.state(view).follow_replies or 0) < 1 then return false end
    local evaluator = require("webview").window(view).follow_state.evaluator
    if evaluator == "uri" or evaluator == "src" or evaluator == "parent_href" then
        return ret == nil or broker.uri(ret)
    end
    return true
end), function (_, view, ret)
    local state = broker.state(view)
    state.follow_replies = state.follow_replies - 1
    follow_func_cb(require("webview").window(view), ret)
end)
follow_wm:add_web_signal("matches", broker.policy({ function (n)
    return type(n) == "number" and n >= 0 and n <= 100000 and n == math.floor(n)
end }, active_follow), function (_, view, n)
    broker.state(view).hints = n
    matches_cb(require("webview").window(view), n)
end)
follow_wm:add_web_signal("click_a_target_blank", broker.policy({ "uri" }, function (channel, view)
    return active_follow(channel, view) and (broker.state(view).follow_clicks or 0) > 0
end), function (_, view, href)
    local state = broker.state(view)
    state.follow_clicks = state.follow_clicks - 1
    require("webview").window(view):new_tab(href, { private = view.private })
end)

new_mode("follow", {
    enter = function (w, mode)
        assert(type(mode) == "table", "invalid follow mode")

        if mode.label_maker then
            msg.warn("Custom label maker not yet implemented!")
        end

        assert(type(mode.pattern_maker or _M.pattern_maker) == "function",
            "invalid pattern_maker function")

        local view = w.view

        local selector = mode.selector_func or _M.selectors[mode.selector]
        assert(type(selector) == "string", "invalid follow selector")

        -- Append site-specific selector
        local domain = lousy.uri.parse(view.uri).host
        local sss = _M.site_specific_selectors[domain]
        if sss and sss[mode.selector] then
            selector = selector .. ", " .. sss[mode.selector]
        end
        mode.selector = selector

        local stylesheet = mode.stylesheet or _M.stylesheet
        assert(type(stylesheet) == "string", "invalid stylesheet")
        mode.stylesheet = stylesheet

        if w.follow_persist then
            mode.persist = true
            w.follow_persist = nil
        end

        broker.state(view).follow_active = true
        w.follow_state = {
            mode = mode, view = view,
            evaluator = mode.evaluator,
        }

        if mode.prompt then
            w:set_prompt(string.format("Follow (%s):", mode.prompt))
        else
            w:set_prompt("Follow:")
        end

        w:set_input("")
        w:set_ibar_theme()

        follow_wm:emit_signal(w.view, "enter", {
            selector = mode.selector, stylesheet = mode.stylesheet,
            evaluator = mode.evaluator, persist = mode.persist,
        }, _M.ignore_case)
    end,

    changed = function (w, text)
        local mode = w.follow_state.mode

        -- Make the hint label/text matching patterns
        local pattern_maker = mode.pattern_maker or _M.pattern_maker
        local hint_pat, text_pat = pattern_maker(text)

        if text ~= "" then
            broker.state(w.view).follow_replies = 1
            broker.state(w.view).follow_clicks = 1
        end
        follow_wm:emit_signal(w.view, "changed", hint_pat, text_pat, text)
    end,

    leave = function (w)
        w:set_ibar_theme()
        broker.state(w.view).follow_active = nil
        follow_wm:emit_signal(w.view, "leave")
    end,
})

add_binds("follow", {
    { "<Tab>",    "Focus the next element hint.",
        function (w) focus(w, 1) end },
    { "<Shift-Tab>",    "Focus the previous element hint.",
        function (w) focus(w, -1)        end },
    { "<Return>", "Activate the currently focused element hint.",
        function (w) do_follow(w)        end },
    { "<Shift-Return>", "Activate all currently visible element hints.",
        function (w) follow_all_hints(w) end },
})

--- Element selectors used to filter elements to follow.
-- @type {[string]=string}
-- @readwrite
_M.selectors = {
    clickable = 'a, area, textarea, select, input:not([type=hidden]), button, label, summary',
    -- Elements that can be clicked.
    focus = 'a, area, textarea, select, input:not([type=hidden]), button, body, applet, object',
    -- Elements that can be given input focus.
    uri = 'a, area',
    -- Elements that have a URI (e.g. hyperlinks).
    desc = '*[title], img[alt], applet[alt], area[alt], input[alt]',
    -- Elements that can have a description.
    image = 'img, input[type=image]',
    -- Image elements.
    thumbnail = "a img",
    -- Image elements within a hyperlink.
}

--- Site specific element selectors used to extend @ref{selectors}.
-- Table keys should be website domains. Values are tables with the same
-- structure as @ref{selectors}.
-- @type {[string]=table}
-- @readwrite
_M.site_specific_selectors = {
    ["github.com"] = {
        clickable = "svg.js-menu-close, div.select-menu-item"
    },
}

add_binds("normal", {
    { "^f$", [[Start `follow` mode. Hint all clickable elements
        (as defined by the `follow.selectors.clickable`
        selector) and open links in the current tab.]],
        function (w)
            w:set_mode("follow", {
                selector = "clickable", evaluator = "click",
                func = function (s) w:emit_form_root_active_signal(s) end,
            })
        end },

    -- Open new tab
    { "^F$", [[Start follow mode. Hint all links (as defined by the
            `follow.selectors.uri` selector) and open links in a new tab.]],
        function (w)
            w:set_mode("follow", {
                prompt = "background tab", selector = "uri", evaluator = "uri",
                func = function (uri)
                    assert(type(uri) == "string")
                    w:new_tab(uri, { switch = false, private = w.view.private })
                end
            })
        end },

    -- Start extended follow mode
    { "^;$", [[Start `ex-follow` mode. See the [ex-follow](#mode-ex-follow)
        help section for the list of follow modes.]],
        function (w) w:set_mode("ex-follow") end },

    { "^g;$", [[Start `ex-follow` mode and stay there until `<Escape>` is pressed.]],
        function (w) w:set_mode("ex-follow", true) end },
})

-- Extended follow mode
new_mode("ex-follow", {
    enter = function (w, persist)
        w.follow_persist = persist
    end,
})

add_binds("ex-follow", {
    { ";", [[Hint all focusable elements (as defined by the
        `follow.selectors.focus` selector) and focus the matched element.]],
        function (w)
            w:set_mode("follow", {
                prompt = "focus", selector = "focus", evaluator = "focus",
                func = function (s) w:emit_form_root_active_signal(s) end,
            })
        end },

    -- Yank element uri or description into primary selection
    { "y", [[Hint all links (as defined by the `follow.selectors.uri`
        selector) and set the primary selection to the matched elements URI.]],
        function (w)
            w:set_mode("follow", {
                prompt = "yank", selector = "uri", evaluator = "uri",
                func = function (uri)
                    assert(type(uri) == "string")
                    uri = uri:gsub(" ", "%%20"):gsub("^mailto:", "")
                    luakit.selection.primary = uri
                    w:notify("Yanked uri: " .. uri, false)
                end
            })
        end },

    -- Yank element description
    { "Y", [[Hint all links (as defined by the `follow.selectors.uri`
        selector) and set the primary selection to the matched elements URI.]],
        function (w)
            w:set_mode("follow", {
                prompt = "yank desc", selector = "desc", evaluator = "desc",
                func = function (desc)
                    assert(type(desc) == "string")
                    luakit.selection.primary = desc
                    w:notify("Yanked desc: " .. desc)
                end
            })
        end },

    -- Open image src
    { "i", [[Hint all images (as defined by the `follow.selectors.image`
        selector) and open matching image location in the current tab.]],
        function (w)
            w:set_mode("follow", {
                prompt = "open image", selector = "image", evaluator = "src",
                func = function (src)
                    assert(type(src) == "string")
                    w:navigate(src)
                end
            })
        end },

    -- Open image src in new tab
    { "I", [[Hint all images (as defined by the
        `follow.selectors.image` selector) and open matching image location in
        a new tab.]],
        function (w)
            w:set_mode("follow", {
                prompt = "tab image", selector = "image", evaluator = "src",
                func = function (src)
                    assert(type(src) == "string")
                    w:new_tab(src, { private = w.view.private })
                end
            })
        end },

    -- Open thumbnail link
    { "x", [[Hint all thumbnails (as defined by the
        `follow.selectors.thumbnail` selector) and open link in current tab.]],
        function (w)
            w:set_mode("follow", {
                prompt = "open image link",
                selector = "thumbnail", evaluator = "parent_href",
                func = function (uri)
                    assert(type(uri) == "string")
                    w:navigate(uri)
                end
            })
        end },

    -- Open thumbnail link in new tab
    { "X", [[Hint all thumbnails (as defined by the
        `follow.selectors.thumbnail` selector) and open link in a new tab.]],
        function (w)
            w:set_mode("follow", {
                prompt = "tab image link", selector = "thumbnail",
                evaluator = "parent_href",
                func = function (uri)
                    assert(type(uri) == "string")
                    w:new_tab(uri, { switch = false, private = w.view.private })
                end
            })
        end },

    -- Open link
    { "o", [[Hint all links (as defined by the `follow.selectors.uri`
        selector) and open its location in the current tab.]],
        function (w)
            w:set_mode("follow", {
                prompt = "open", selector = "uri", evaluator = "uri",
                func = function (uri)
                    assert(type(uri) == "string")
                    w:navigate(uri)
                end
            })
        end },

    -- Open link in new tab
    { "t", [[Hint all links (as defined by the `follow.selectors.uri`
        selector) and open its location in a new tab.]],
        function (w)
            w:set_mode("follow", {
                prompt = "open tab", selector = "uri", evaluator = "uri",
                func = function (uri)
                    assert(type(uri) == "string")
                    w:new_tab(uri, { private = w.view.private })
                end
            })
        end },

    -- Open link in background tab
    { "b", [[Hint all links (as defined by the `follow.selectors.uri`
        selector) and open its location in a background tab.]],
        function (w)
            w:set_mode("follow", {
                prompt = "background tab", selector = "uri", evaluator = "uri",
                func = function (uri)
                    assert(type(uri) == "string")
                    w:new_tab(uri, { switch = false, private = w.view.private })
                end
            })
        end },

    -- Open link in new window
    { "w", [[Hint all links (as defined by the `follow.selectors.uri`
        selector) and open its location in a new window.]],
        function (w)
            w:set_mode("follow", {
                prompt = "open window", selector = "uri", evaluator = "uri",
                func = function (uri)
                    assert(type(uri) == "string")
                    window.new{uri}
                end
            })
        end },

    -- Set command `:open <uri>`
    { "O", [[Hint all links (as defined by the `follow.selectors.uri`
        selector) and generate a `:open` command with the elements URI.]],
        function (w)
            w:set_mode("follow", {
                prompt = ":open", selector = "uri", evaluator = "uri",
                func = function (uri)
                    assert(type(uri) == "string")
                    w:enter_cmd(":open " .. uri)
                end
            })
        end },

    -- Set command `:tabopen <uri>`
    { "T", [[Hint all links (as defined by the `follow.selectors.uri`
        selector) and generate a `:tabopen` command with the elements URI.]],
        function (w)
            w:set_mode("follow", {
                prompt = ":tabopen", selector = "uri", evaluator = "uri",
                func = function (uri)
                    assert(type(uri) == "string")
                    w:enter_cmd(":tabopen " .. uri)
                end
            })
        end },

    -- Set command `:winopen <uri>`
    { "W", [[Hint all links (as defined by the `follow.selectors.uri`
        selector) and generate a `:winopen` command with the elements URI.]],
        function (w)
            w:set_mode("follow", {
                prompt = ":winopen", selector = "uri", evaluator = "uri",
                func = function (uri)
                    assert(type(uri) == "string")
                    w:enter_cmd(":winopen " .. uri)
                end
            })
        end },
})

return _M

-- vim: et:sw=4:ts=8:sts=4:tw=80
