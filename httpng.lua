local log = require("log")
local uri = require("uri")
local socket = require("socket")

package.cpath = package.cpath .. ';./?.dylib;../?.dylib'
local httpng_c = require("httpng_c")

-- ========================================================================= --
--                      local support functions.                             --
-- ========================================================================= --

--- Make contents of `tbl` in string representation, with indentation.
-- Returns string if table type passed, nil otherwise.
local function print_table_to_string(tbl)

    local function tprint(tbl, indent, str_res)
        for k, v in pairs(tbl) do
            local formatting = string.rep("  ", indent) .. k .. ": "
            if type(v) == "table" then
                str_res = str_res .. formatting .. "\n"
                str_res = tprint(v, indent + 1, str_res)
            else
                str_res = str_res .. formatting .. tostring(v) .. "\n"
            end
        end
        return str_res
    end

    return (type(tbl) == "table" and tprint(tbl, 0, "") or nil)
end

--- Find out if the table is an array.
-- Array is considered as a table with only [1...#table] indexes.
local function table_is_array(t)
    local i = 0
    for _ in pairs(t) do
        i = i + 1
        if t[i] == nil then return false end
    end
    return true
end

--- Concatenate t2 table with t1, if they are arrays.
local function array_concat(t1, t2)
    for i = 1, #t2 do
        t1[#t1 + 1] = t2[i]
    end
    return t1
end

--- Get list of IP addresses by hostname.
local function get_ip_addrs_by_hostname(hostname)
    local addrinfo = socket.getaddrinfo(hostname, nil,
        { protocol = 'tcp' })
    if addrinfo == nil then
        return nil
    end

    local res = {}
    for _, info in ipairs(addrinfo) do
        table.insert(res, info.host)
    end
    return res
end

-- Returns table where every element has a key "<field1>:<field2>..."
-- and a value is an array that contains the elements of @arr with the
-- same values at provided fields.
local function find_duplicates_by_fields(arr, ...)
    local arg = { ... }

    if #arg == 0 then
        error("No fields provided")
    end

    local res = {}
    for i, v in ipairs(arr) do
        local key = ""
        for j, field in ipairs(arg) do
            key = key .. tostring(v[field])
            if j ~= #arg then
                key = key .. ":"
            end
        end

        if res[key] == nil then
            res[key] = { v }
        else
            res[key][#res[key] + 1] = v
        end
    end
    return res
end

local function is_integer(obj)
    return type(obj) == "number" and math.floor(obj) == obj
end
-- ========================================================================= --
--                Listen defaults and functions for them.                    --
-- ========================================================================= --


local default_listen_port = 3300
local max_port_value = 65535

-- If listen is absent, use this one.
local default_listen_value = default_listen_port

-- Default parameters for protocols.
local protocol_defaults = {
    ["http"] = {
        port = 80,
    },
    ["https"] = {
        port =  443,
    },
}
-- If IP address isn't defined, create listeners with the same other parameters
-- on these IP addresses.
local default_ip_addresses = {
    "0.0.0.0",
    "::",
}

local default_uses_sni = false

--- Get element from PROTOCOL_DEFAULTS using field name and value at this field.
-- Returns element if found, nil otherwise.
local function get_protocol_defaults_by(field, value)
    local res
    for protocol, defaults in pairs(protocol_defaults) do
        if defaults[field] == value then
            res = defaults
            break
        end
    end
    return res
end

-- ========================================================================= --
-- Functions for parsing <listen-conf>.
--
-- listen consists of, as we call it, <listen-conf>s.
--
-- If listen is a table and array(contains only [1...#listen] indexes),
--   then each element is considered as a <listen-conf>.
-- Otherwise, it is a single <listen-conf>.
--
-- "Clean" <listen-conf> is a table which has fields:
--  * addr - string with IPv4/IPv6 address or hostname.
--  * port - number of port to listen on.
--  * tls - table of tables <tls_config> if listener using tls, nil otherwise.
--      * <tls_config>:
--          * certificate_file - path to ssl certificate.
--          * certificate_key_file - path to key.
--
-- After preprocessing listen should be an array of "clean" <listen-conf>s.
-- Every "clean" <listen-conf> describes one listener.
-- ========================================================================= --

--- Get "clean" <listen-conf> from parameters.
-- addr and port are required. If tls is absent it will be set by default rules.
local function get_clean_listen_conf(listen_conf)
    assert(type(listen_conf.addr) == "string")
    assert(type(listen_conf.port) == "number")
    if (type(listen_conf.tls) == "table") then
        assert(table_is_array(listen_conf.tls))
        assert(type(listen_conf.uses_sni) == "boolean")
        assert((#listen_conf.tls > 1 and listen_conf.uses_sni)
            or #listen_conf.tls == 1)
    else
        assert(not listen_conf.tls)
    end

    return {
        addr = listen_conf.addr,
        port = listen_conf.port,
        tls = not listen_conf.tls and nil or listen_conf.tls,
        uses_sni = listen_conf.uses_sni
    }
end

-------------------------------------------------------------------------------
--                    Validation of <listen-conf> functions.                 --
-------------------------------------------------------------------------------
local function validate_number_listen_conf(listen_conf)
    assert(type(listen_conf) == "number")
    if not is_integer(listen_conf) then
        error("port must be an integer, not float:\n" .. listen_conf, 2)
    end
    if listen_conf < 0 or listen_conf > max_port_value then
        error("invalid port specified", 2)
    end
end

local function validate_string_listen_conf(listen_conf)
    assert(type(listen_conf) == "string")
end

local function validate_table_listen_conf(listen_conf)
    assert(type(listen_conf) == "table")
    local addr = listen_conf.addr
    local port = listen_conf.port
    local tls = listen_conf.tls
    local uses_sni = listen_conf.uses_sni

    if addr and type(addr) ~= "string" then
        error("'addr' must be a string:\n" ..
            print_table_to_string(listen_conf))
    end
    if port and type(port) ~= "number" then
        error("'port' must be a number:\n" ..
            print_table_to_string(listen_conf))
    end
    if not tls then
        return
    end
    if type(tls) ~= "table" then
        error("'tls' must be a table:\n" ..
            print_table_to_string(listen_conf))
    end
    if table_is_array(tls) == false then
        error("'tls' should be an array:\n" ..
            print_table_to_string(listen_conf))
    end
    if #tls > 1 and not uses_sni then
        error("'uses_sni' can't be false or nil if there're certificates " ..
            "more than 1:\n" .. print_table_to_string(listen_conf))
    end

 -- TODO: throw warning if found unused fields

    for _, tls_config in ipairs(tls) do
        local certificate_file = tls_config.certificate_file
        local certificate_key_file = tls_config.certificate_key_file
        if certificate_file and type(certificate_file) ~= "string" then
            error("'certificate_file'' must be a string\n" ..
                print_table_to_string(tls_config))
        end
        if certificate_key_file and type(certificate_key_file) ~= "string" then
            error("'certificate_key_file' must be a string:\n" ..
                print_table_to_string(tls_config))
        end
        if certificate_file == nil or certificate_key_file == nil then
            error("missing 'certificate_file' or 'certificate_key_file':\n" ..
                print_table_to_string(tls_config))
        end
    end
end

-------------------------------------------------------------------------------
--           Getting "clean" <listen-conf>s from "dirty" one functions.      --
-------------------------------------------------------------------------------

local function get_clean_listen_confs_from_number(listen_conf)
    validate_number_listen_conf(listen_conf)

    local port = listen_conf
    local res = {}
    for _, addr in ipairs(default_ip_addresses) do
        local clean_conf = get_clean_listen_conf({addr = addr, port = port})
        table.insert(res, clean_conf)
    end
    return res
end

local function get_clean_listen_confs_from_string(listen_conf)
    validate_string_listen_conf(listen_conf)

    local res
    -- If listen_conf is protocol (http/https like).
    if listen_conf:match("^[a-z%d]+$") and
      not get_ip_addrs_by_hostname(listen_conf) then
        local protocol = listen_conf
        local defaults = protocol_defaults[protocol]
        if not defaults then
            error("unknown protocol specified: " .. protocol)
        end
        res = get_clean_listen_confs_from_number(defaults.port)

    -- listen_conf is considered as uri.
    else
        local uri_table = uri.parse(listen_conf)
        if uri_table == nil then
            -- uri can't parse :port
            if listen_conf:match("^:%d+$") then
                local port = tonumber(listen_conf:sub(2))
                res = get_clean_listen_confs_from_number(port)
                goto return_get_clean_lc_from_str
            end
            error("impossible to parse URI: " .. listen_conf)
        end

        local host = uri_table.host
        local port = tonumber(uri_table.service)
        local protocol = uri_table.scheme
        if protocol and protocol_defaults[protocol] and port == nil then
            port = protocol_defaults[protocol].port
        end
        if port == nil then
            port = default_listen_port
        end

        local ip_addrs = get_ip_addrs_by_hostname(host)
        if ip_addrs == nil then
            error("unknown hostame: " .. host)
        end

        res = {}
        for i = 1, #ip_addrs do
            table.insert(res, get_clean_listen_conf(
                { addr = ip_addrs[i], port = port }))
        end
    end

::return_get_clean_lc_from_str::
return res

end

local function get_clean_listen_confs_from_table(listen_conf)
    validate_table_listen_conf(listen_conf)

    local addr = listen_conf.addr
    local port = listen_conf.port
    local tls = listen_conf.tls
    local uses_sni = listen_conf.uses_sni
    local res = {}

    if uses_sni == nil then
        uses_sni = default_uses_sni
    end
    if port == nil then
        port = default_listen_port
    else
        validate_number_listen_conf(port)
    end
    if addr == nil then
        for _, addr in ipairs(default_ip_addresses) do
            local clean_conf = get_clean_listen_conf({addr = addr,
                port = port, tls = tls, uses_sni = uses_sni})
            table.insert(res, clean_conf)
        end
    else
        local clean_conf = get_clean_listen_conf({addr = addr,
            port = port, tls = tls, uses_sni = uses_sni})
        table.insert(res, clean_conf)
    end
    return res
end

--- Get an array of "clean" <listen-conf>s from given <listen-conf>.
-- Return value is an array because from one <listen-conf> can be created few
-- "clean" <listen-confs>. E.g.: <listen-conf> = 3300 create few listeners with
-- default ip addresses on port 3300.
local function get_clean_listen_confs(listen_conf)
    -- TODO: check unexpected input data
    local res
    if type(listen_conf) == "number" then
        res = get_clean_listen_confs_from_number(listen_conf)
    elseif type(listen_conf) == "string" then
        res = get_clean_listen_confs_from_string(listen_conf)
    elseif type(listen_conf) == "table" then
        res = get_clean_listen_confs_from_table(listen_conf)
    else
        error("unknown listen_conf type\n")
    end
    return res
end

-- ========================================================================= --
--             High-level local functions of preprocessing 'listen'.         --
-- ========================================================================= --

-- TODO: exclude duplicate listeners
--- Get listen as an array of "clean" <listen-conf>s.
local function prepare_listen_for_c(listen)
    if listen == nil then
        listen = default_listen_value
    end

    local result_listen
    if type(listen) == "table" and table_is_array(listen) then
        result_listen = {}
        for i = 1, #listen do
            local clean_listen_confs = get_clean_listen_confs(listen[i])
            array_concat(result_listen, clean_listen_confs)
        end
    else
        result_listen = get_clean_listen_confs(listen)
    end
    return result_listen
end


local function check_for_duplicate_listeners(listen)
    local duplicate_listen = find_duplicates_by_fields(listen, "addr", "port")

    for addr_port, duplicate_listeners in pairs(duplicate_listen) do
        if #duplicate_listeners > 1 then
            local err_msg = ("Duplicate listeners for %s found:\n%s"):format(
                    addr_port, print_table_to_string(duplicate_listeners))
            error(err_msg)
        end
    end
end


-- ========================================================================= --
--                      Methods of main `export` table.                      --
-- ========================================================================= --
local export = table.deepcopy(httpng_c)

-- Main cfg function.
export.cfg = function (config)
    if config == nil then
        error("No parameters specified", 2)
    end
    local prepared_listen = prepare_listen_for_c(config.listen)
    check_for_duplicate_listeners(prepared_listen)
    config.listen = prepared_listen

    log.debug("Listeners for C:\n" .. print_table_to_string(config.listen))
    local res, err = pcall(httpng_c.cfg, config)
    if err then
        error(err, 2)
    end
    return res
end

return export
