#!/usr/bin/env tarantool

local fio = require('fio')
local fiber = require('fiber')
require('strict')

box.cfg{
  listen = 3306,
}

local s = box.schema.space.create('tester')
s:format({
             {name = 'id', type = 'unsigned'},
             {name = 'desc', type = 'string'},
         })
local index = s:create_index('primary',
        {
            type = 'tree',
            parts = {'id'}
        })

local large = box.schema.space.create('large')
large:format({
             {name = 'id', type = 'unsigned'},
             {name = 'desc', type = 'string'},
         })
large:create_index('primary',
        {
            type = 'tree',
            parts = {'id'}
        })

s:insert{1, 'First'}
s:insert{2, 'Second'}

local long_string = ''
local large_entry_part_count = 30000
local counter
for counter = 1, large_entry_part_count
do
	long_string = long_string..'Large entry, part #'..counter..' of '..large_entry_part_count..'\n'
end

large:insert{2, long_string}

for counter = 3, 10000
do
	s:insert{counter, 'Entry #'..counter}
end

local function hello_handler(req, header_writer)
	if (req.method ~= 'GET') then
		header_writer:write_header(500, nil, 'Unsupported HTTP method', true)
		return
	end

	-- Array of tables because more than one header can have the same field name (key).
	local headers = {
		{['content-type'] = 'text/plain; charset=utf-8'},
		{['x-custom-header'] = 'foo'},
	}
	local payload = 'Small hello from lua'

	local payload_writer = header_writer:write_header(200, headers, payload, true)
end

local function large_handler(req, header_writer)
	if (req.method ~= 'GET') then
		header_writer:write_header(500, nil, 'Unsupported HTTP method', true)
		return
	end

	local payload = large:get(2).desc

	-- Array of tables because more than one header can have the same field name (key).
	local headers = {
		{['content-type'] = 'text/plain; charset=utf-8'},
		--{['content-length'] = #payload}, -- won't work, must be string
		{['content-length'] = string.format("%d", #payload)},
		{['x-custom-header'] = 'foo'},
	}

	local payload_writer = header_writer:write_header(200, headers, payload, true)
end

local function multi_handler(req, header_writer)
	if (req.method ~= 'GET') then
		header_writer:write_header(500, nil, 'Unsupported HTTP method', true)
		return
	end

	-- Array of tables because more than one header can have the same field name (key).
	local headers = {
		{['content-type'] = 'text/plain; charset=utf-8'},
		{['x-custom-header'] = 'foo'},
	}

	local payload_writer = header_writer:write_header(200, headers, nil, false)

	local k, v, counter
	counter = 0
	for k, v in s:pairs() do
		--payload_writer:write(v.desc, false) --works but w/o newlines
		local closed = payload_writer:write(string.format("%s\n", v.desc), false)
		if closed then
			-- Connection has already been closed
			return
		end
		counter = counter + 1
		if ((counter % 2000) == 0) then
			fiber.sleep(0.5)
		end
	end

	payload_writer:write('<End of list>', true)
	--payload_writer:write(nil, true) -- also works
end

-- For performance reasons only one string with path/query is passed from C.
local function get_query(req)
	if req.query_at == -1 then
		return nil
	end
	return string.sub(req.path, req.query_at, -1)
end

local function req_handler(req, header_writer)
	if (req.method ~= 'GET') then
		header_writer:write_header(500, nil, 'Unsupported HTTP method', true)
		return
	end

	-- Array of tables because more than one header can have the same field name (key).
	local headers = {
		{['content-type'] = 'text/plain; charset=utf-8'},
		{['x-custom-header'] = 'foo'},
	}

	local payload
	local req_query = get_query(req)
	if req_query then
		local query_str = string.match(req_query, "^?id=%d+")
		if query_str then
			local id_str = string.sub(query_str, 5, -1)
			local id = tonumber(id_str)
			if id then
				local tuple = s:get(id)
				if tuple then
					payload = tuple.desc
				else
					payload = 'Entry was not found'
				end
			else
				payload = 'Invalid id was specified (not a number)'
			end
		else
			payload = 'Unable to parse query (format: "?id=3")'
		end
	else
		payload = 'No query specified'
	end

	local payload_writer = header_writer:write_header(200, headers, payload, true)
end

local function ws_server_handler(req, header_writer)
	if (req.is_websocket == false) then
		header_writer:write_header(500, {}, 'Only WebSocket requests are supported', true)
		return
	end

	-- Array of tables because more than one header can have the same field name (key).
	local headers = {
		{['content-type'] = 'text/plain; charset=utf-8'},
		{['x-custom-header'] = 'foo'},
	}
	if (req.version_major >= 2) then
		-- Currently H2O does not support WebSockets with HTTP/2 (and there is an assert).
		header_writer:write_header(500, {}, 'Can\'t use WebSockets with HTTP/2 or later', true)
		return
	end

	local ws = header_writer:upgrade_to_websocket(headers)
	if (ws == nil) then
		return
	end

	fiber.sleep(1)
	local counter = 1
	while (true) do
		if (ws:send_text(string.format("%d\n", counter))) then
			return
		end
		counter = counter + 1
		fiber.sleep(1)
	end
end

local function ws_app_handler(req, header_writer)
	if (req.method ~= 'GET') then
		header_writer:write_header(500, nil, 'Unsupported HTTP method', true)
		return
	end

	-- Array of tables because more than one header can have the same field name (key).
	local headers = {
		{['content-type'] = 'text/html; charset=utf-8'},
		{['x-custom-header'] = 'foo'},
	}
	local payload = [[
<html><head><title>Example WebSockets Application</title></head><body>
<script>
	webSocket = new WebSocket('wss://localhost:7890/lua_ws_server');
	webSocket.onopen = function (event) {
		document.write('Connection to WebSocket server established successfully<br>\n');
	}
	webSocket.onmessage = function (event) {
		document.write('Received data: ', event.data, '<br>\n');
	}
	document.write('Trying to connect to WebSocket server...<br>\n');
	while (true) {
		sleep(1);
	}
</script>
</body></html>
]]

	local payload_writer = header_writer:write_header(200, headers, payload, true)
end

local httpng_lib = require "httpng"
local init_func = httpng_lib.init

local sample_site_lib = require "sample_site"

local lua_sites = {
	{['path'] = '/lua_large', ['handler'] = large_handler},
	{['path'] = '/lua_hello', ['handler'] = hello_handler},
	{['path'] = '/lua_multi', ['handler'] = multi_handler},
	{['path'] = '/lua_req',   ['handler'] = req_handler},
	{['path'] = '/lua_ws_server', ['handler'] = ws_server_handler},
	{['path'] = '/lua_ws_app', ['handler'] = ws_app_handler},
}

init_func(lua_sites, sample_site_lib.get_site_desc, nil)
