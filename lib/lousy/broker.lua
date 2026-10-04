--- Validation and document-scoped state for bundled WebProcess brokers.
--
-- Shared helpers validate hostile arguments and keep authorization records in
-- the trusted UI for the lifetime of one document.
--
-- @module lousy.broker
-- @copyright 2026 Luakit contributors

local _M = {}

local states = setmetatable({}, { __mode = "k" })

--- Get UI-owned state, invalidated on navigation, destruction or process loss.
-- @tparam widget view The originating webview.
-- @treturn table The current document's state.
function _M.state(view)
    if not states[view] then
        states[view] = {}
        local function clear() states[view] = {} end
        view:add_signal("load-status", function (_, status)
            if status == "provisional" then clear() end
        end)
        view:add_signal("crashed", clear)
        view:add_signal("destroy", function () states[view] = nil end)
    end
    return states[view]
end

--- Validate a positive integer identifier.
-- @param value A hostile value.
-- @treturn boolean Whether the value is an identifier.
function _M.id(value)
    return type(value) == "number" and value >= 1 and value <= 9007199254740991
        and value == math.floor(value)
end

--- Validate a network URI suitable for renderer-requested navigation.
-- @param uri A hostile URI.
-- @treturn boolean Whether the URI has an allowed scheme.
function _M.uri(uri)
    if type(uri) ~= "string" or #uri > 8192 or uri:find("[%z\1-\32\127]") then return false end
    local scheme = uri:match("^([%a][%w+.-]*):")
    scheme = scheme and scheme:lower()
    return scheme == "http" or scheme == "https" or scheme == "ftp" or scheme == "gopher"
end

local function value_matches(spec, value)
    if type(spec) == "function" then return spec(value) == true end
    if spec:sub(-1) == "?" then
        if value == nil then return true end
        spec = spec:sub(1, -2)
    end
    if spec == "id" then return _M.id(value) end
    if spec == "uri" then return _M.uri(value) end
    if spec == "string" then
        return type(value) == "string" and #value <= 8192 and not value:find("%z")
    end
    return type(value) == spec
end

--- Build an exact argument validator from a sequence of types or predicates.
-- @tparam table schema Argument types; a trailing '?' permits omission.
-- @treturn function Validator for a list of arguments.
function _M.args(schema)
    return function (...)
        local count = select("#", ...)
        if count > #schema then return false end
        for i, spec in ipairs(schema) do
            if not value_matches(spec, select(i, ...)) then return false end
        end
        return true
    end
end

--- Build a broker policy with exact argument validation and UI authorization.
-- @tparam table schema Argument types or predicates.
-- @tparam function authorize UI-owned authorization check.
-- @treturn table Broker registration policy.
function _M.policy(schema, authorize)
    local validate = _M.args(schema)
    return {
        validate = function (_, _, ...) return validate(...) end,
        authorize = authorize,
    }
end

--- Validate a bounded dense array of identifiers.
-- @param values A hostile array.
-- @treturn boolean Whether all entries are identifiers.
function _M.ids(values)
    if type(values) ~= "table" or #values > 1000 then return false end
    local count = 0
    for k, v in pairs(values) do
        if not _M.id(k) or k > #values or not _M.id(v) then return false end
        count = count + 1
    end
    return count == #values
end

--- Validate chrome search options.
-- @param opts A hostile options table.
-- @treturn boolean Whether search options are well formed.
function _M.search_options(opts)
    if type(opts) ~= "table" then return false end
    for k, v in pairs(opts) do
        if k == "query" then
            if not value_matches("string", v) then return false end
        elseif k == "limit" then
            if not _M.id(v) or v > 1000 then return false end
        elseif k == "page" then
            if not _M.id(v) or v > 1000000 then return false end
        else return false end
    end
    return true
end

return _M

-- vim: et:sw=4:ts=8:sts=4:tw=80
