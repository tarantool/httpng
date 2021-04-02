local t = require 'luatest'
local g = t.group 'bad_handlers'
--local http_client = require 'http.client'
local httpng = require 'httpng'
local fiber = require 'fiber'
local popen = require 'popen'

local write_handler_launched = false
local bad_write_ok
local bad_write_err
local write_bad_shuttle_ok
local write_bad_shuttle_err

local write_header_handler_launched = false
local bad_write_header_ok
local bad_write_header_err
local write_header_bad_shuttle_ok
local write_header_bad_shuttle_err
local write_header_invalid_ok
local write_header_invalid_err
local upgrade_to_websocket_bad_shuttle_ok
local upgrade_to_websocket_bad_shuttle_err
local write_first_header_ok
local write_second_header_ok
local write_second_header_err
local upgrade_to_websocket_ok
local upgrade_to_websocket_err
local close_ok
local close_err
local close_bad_shuttle_ok
local close_bad_shuttle_err
local query_ok
local query_err
local query_bad_ok
local query_bad_err
local query_large_query_at_ok
local query_large_query_at_err
local query_bad_path_ok
local query_bad_path_err
local _

local write_handler = function(req, io)
    write_handler_launched = true
    bad_write_ok, bad_write_err = pcall(io.write, io)

    local saved_shuttle = io.shuttle
    io.shuttle = 42
    write_bad_shuttle_ok, write_bad_shuttle_err = pcall(io.write, io, 'a')
    io.shuttle = saved_shuttle

    io:close()
end

local write_header_handler = function(req, io)
    write_header_handler_launched = true
    bad_write_header_ok, bad_write_header_err = pcall(io.write_header, io)

    local saved_shuttle = io.shuttle
    io.shuttle = 42
    write_header_bad_shuttle_ok, write_header_bad_shuttle_err =
        pcall(io.write_header, io, 200)
    io.shuttle = saved_shuttle

    write_header_invalid_ok, write_header_invalid_err =
        pcall(io.write_header, io, 'a')

    local saved_shuttle = io.shuttle
    io.shuttle = 42
    upgrade_to_websocket_bad_shuttle_ok, upgrade_to_websocket_bad_shuttle_err =
        pcall(io.upgrade_to_websocket, io)
    io.shuttle = saved_shuttle

    query_ok, query_err = pcall(req.query)

    local saved_query_at = req.query_at
    req.query_at = 'x'
    query_bad_ok, query_bad_err = pcall(req.query, req)
    req.query_at = 999999
    query_large_query_at_ok, query_large_query_at_err = pcall(req.query, req)
    req.query_at = saved_query_at

    req.path = 42
    query_bad_path_ok, query_bad_path_err = pcall(req.query, req)

    write_first_header_ok, _ = io:write_header(200, nil, 'a', true)
    write_second_header_ok, write_second_header_err =
        pcall(io.write_header, io, 200)

    upgrade_to_websocket_ok, upgrade_to_websocket_err =
        pcall(io.upgrade_to_websocket, io)

    close_ok, close_err = pcall(io.close)

    local saved_shuttle = io.shuttle
    io.shuttle = 42
    close_bad_shuttle_ok, close_bad_shuttle_err = pcall(io.close, io)
    io.shuttle = saved_shuttle

    io:close()
end

httpng.cfg({ sites = {
    { path = '/write', handler = write_handler },
    { path = '/write_header', handler = write_header_handler },
}})

g.test_write_params = function()
    t.assert(write_handler_launched == false)
    --[[
    local httpc = http_client.new()
    local result = httpc:request('GET', 'https://localhost:8080', nil,
        {verify_host = 0, verify_peer = 0})
    --]]--
    local ph = popen.shell('wget --no-check-certificate -O /dev/null '..
        'https://localhost:8080/write')
    local result = ph:wait().exit_code
    t.assert(result == 0, 'http request failed')
    t.assert(write_handler_launched == true, 'Handler was not launched')
    t.assert(bad_write_ok == false,
        'io:write() with invalid parameter set didn\'t fail')
    t.assert_str_matches(bad_write_err, 'Not enough parameters')

    t.assert(write_bad_shuttle_ok == false,
        'io:write() with corrupt io.shuttle didn\'t fail')
    t.assert_str_matches(write_bad_shuttle_err, 'shuttle is invalid')
end

g.test_write_header_params = function()
    t.assert(write_header_handler_launched == false)
    local ph = popen.shell('wget --no-check-certificate -O /dev/null '..
        'https://localhost:8080/write_header')
    local result = ph:wait().exit_code
    t.assert(result == 0, 'http request failed')
    t.assert(write_header_handler_launched == true, 'Handler was not launched')
    t.assert(bad_write_header_ok == false,
        'io:write_header() with invalid parameter set didn\'t fail')
    t.assert_str_matches(bad_write_header_err, 'Not enough parameters')

    t.assert(write_header_bad_shuttle_ok == false,
        'io:write_header() with corrupt io.shuttle didn\'t fail')
    t.assert_str_matches(write_header_bad_shuttle_err, 'shuttle is invalid')

    t.assert(write_header_invalid_ok == false,
        'io:write_header() with non-integer HTTP code didn\'t fail')
    t.assert_str_matches(write_header_invalid_err,
        'HTTP code is not an integer')

    t.assert(upgrade_to_websocket_bad_shuttle_ok == false,
        'io:upgrade_to_websocket() with corrupt io.shuttle didn\'t fail')
    t.assert_str_matches(upgrade_to_websocket_bad_shuttle_err,
        'shuttle is invalid')

    t.assert(write_first_header_ok == true, 'Valid io:write_header() fail')
    t.assert(write_second_header_ok == false,
        'Second io:write_header() didn\'t fail')
    t.assert_str_matches(write_second_header_err,
        'Handler has already written header')

    t.assert(query_ok == false,
        'req:query() with invalid parameter set didn\'t fail')
    t.assert_str_matches(query_err, 'Not enough parameters')

    t.assert(query_bad_ok == false,
        'req:query() with invalid query_at didn\'t fail')
    t.assert_str_matches(query_bad_err, 'query_at is not an integer')

    t.assert(query_large_query_at_ok == false,
        'req:query() with large query_at didn\'t fail')
    t.assert_str_matches(query_large_query_at_err,
        'query_at value is invalid')

    t.assert(query_bad_path_ok == false,
        'req:query() with invalid path didn\'t fail')
    t.assert_str_matches(query_bad_path_err, 'path is not a string')

    t.assert(upgrade_to_websocket_ok == false,
        'io:upgrade_to_websocket() after write_header() didn\'t fail')
    t.assert_str_matches(upgrade_to_websocket_err,
        'Unable to upgrade to WebSockets after sending HTTP headers')

    t.assert(close_ok == false,
        'io:close() with invalid parameter set didn\'t fail')
    t.assert_str_matches(close_err, 'Not enough parameters')

    t.assert(close_bad_shuttle_ok == false,
        'io:close() with corrupt io.shuttle didn\'t fail')
    t.assert_str_matches(close_bad_shuttle_err, 'shuttle is invalid')
end


--fiber.sleep(100) -- For 'external' wget etc

