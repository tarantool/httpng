#!/usr/bin/env tarantool

local fio = require('fio')
local fiber = require('fiber')
require('strict')

print '\n\n\n\n\nFilling in test spaces, please wait...\n\n\n'
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
	long_string = long_string..'Large entry, part #'..counter..
		' of '..large_entry_part_count..'\n'
end

large:insert{2, long_string}

for counter = 3, 10000
do
	s:insert{counter, 'Entry #'..counter}
end

local function hello_handler(req, io)
	if (req.method ~= 'GET') then
		io:write_header(500, nil,
			'Unsupported HTTP method', true)
		return
	end

	local headers = {
		['content-type'] = 'text/plain; charset=utf-8',
		['x-custom-header'] = 'foo',
	}
	local payload = 'Small hello from lua'

	io:write_header(200, headers, payload, true)
end

local function large_handler(req, io)
	if (req.method ~= 'GET') then
		io:write_header(500, nil, 'Unsupported HTTP method',
			true)
		return
	end

	local payload = large:get(2).desc

	local headers = {
		['content-type'] = 'text/plain; charset=utf-8',
		--['content-length'] = #payload, -- won't work, must be string
		['content-length'] = string.format("%d", #payload),
		['x-custom-header'] = 'foo',
	}

	io:write_header(200, headers, payload, true)
end

local function multi_handler(req, io)
	if (req.method ~= 'GET') then
		io:write_header(500, nil,
			'Unsupported HTTP method', true)
		return
	end

	local headers = {
		['content-type'] = 'text/plain; charset=utf-8',
		['x-custom-header'] = 'foo',
	}

	io:write_header(200, headers)

	local k, v, counter
	counter = 0
	for k, v in s:pairs() do
		--io:write(v.desc, false) --works but w/o newlines
		local closed = io:write(string.format("%s\n",
			v.desc), false)
		if closed then
			-- Connection has already been closed
			return
		end
		counter = counter + 1
		if ((counter % 2000) == 0) then
			fiber.sleep(0.5)
		end
	end

	io:write('<End of list>', true)
	--io:write(nil, true) -- also works
end

-- For performance reasons only one string with path/query is passed from C.
local function get_query(req)
	if req.query_at == -1 then
		return nil
	end
	return string.sub(req.path, req.query_at, -1)
end

local function req_handler(req, io)
	if (req.method ~= 'GET') then
		io:write_header(500, nil,
			'Unsupported HTTP method', true)
		return
	end

	local headers = {
		['content-type'] = 'text/plain; charset=utf-8',
		['x-custom-header'] = 'foo',
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
				payload = 'Invalid id was specified '..
					'(not a number)'
			end
		else
			payload = 'Unable to parse query (format: "?id=3")'
		end
	else
		payload = 'No query specified'
	end

	io:write_header(200, headers, payload, true)
end

local function ws_server_handler(req, io)
	if (req.is_websocket == false) then
		io:write_header(500, nil,
			'Only WebSocket requests are supported', true)
		return
	end

	local headers = {
		['content-type'] = 'text/plain; charset=utf-8',
		['x-custom-header'] = 'foo',
	}
	if (req.version_major >= 2) then
		-- Currently H2O does not support WebSockets with HTTP/2
		-- (and there is an assert).
		io:write_header(500, nil,
			'Can\'t use WebSockets with HTTP/2 or later', true)
		return
	end

	local ws

	local function initiate_connection_termination()
		-- Creating new fiber every time is a bad idea,
		-- this is just a stopgap.
		fiber.new(function()
			ws:send_text('Server is terminating connection')
			ws:close()
		end)
	end

	local function send_from_recv_handler(data)
		-- Creating new fiber every time is a bad idea,
		-- this is just a stopgap.
		fiber.new(function()
			ws:send_text(data)
		end)
	end

	local recv_limit = 5
	local recv_count = 0
	ws = io:upgrade_to_websocket(headers, function(data)
		-- Warning: This function is NOT allowed to yield
		--[[
		-- This call would fail because we are inside
		-- WebSocket recv handler.
		if (ws:send_text('(direct) Server received "'..data..'"')) then
			--return
		end
		--]]--
		send_from_recv_handler('Server received "'..data..'"')
		recv_count = recv_count + 1
		if (recv_count >= recv_limit) then
			initiate_connection_termination()
		end
		--fiber.sleep(3) -- This works at the moment but is a bad idea
		--because it stalls TX HTTP processing.
	end)
	if (ws == nil) then
		return
	end

	fiber.sleep(1)
	local num_iterations = 3
	local counter
	for counter = 1, num_iterations do
		if (ws:send_text(string.format('%d of %d', counter,
		    num_iterations))) then
			return
		end
		fiber.sleep(1)
	end

	if (ws:send_text('Server is now waiting for data from app')) then
		return
	end
	while true do
		if (ws:send_text(string.format('Server receives count: %d',
		    recv_count))) then
			return
		end
		fiber.sleep(1)
	end
	ws:close()
end

local function ws_app_handler(req, io)
	if (req.method ~= 'GET') then
		io:write_header(500, nil, 'Unsupported HTTP method',
			true)
		return
	end

	local headers = {
		['content-type'] = 'text/html; charset=utf-8',
		['x-custom-header'] = 'foo',
	}
	local payload = [[
<html><head><title>Example WebSockets Application</title></head><body>
<script>
	webSocket = new WebSocket('wss://localhost:7890/ws_server');
	sendCounter = 0;
	sendLimit = 10;
	function sendOne() {
		++sendCounter;
		document.write('App: sending "', sendCounter,
			'" to server<br>\n');
		webSocket.send(sendCounter);
		if (sendCounter >= sendLimit) {
			clearInterval(periodicSendsTimer);
			document.write('App: stopped sends<br>\n');
		}
	}
	function startSending() {
		document.write('App: starting sends, will perform ',
			'no more than ', sendLimit, ' of them<br>\n');
		periodicSendsTimer = setInterval(sendOne, 1000);
	}
	webSocket.onopen = function (event) {
		document.write('Connection to WebSocket server established ',
			'successfully<br>\n');
		delay = 5000;
		document.write('App will start sending data to server in ',
			delay, ' milliseconds<br>\n');
		window.setTimeout(startSending, delay);
	}
	webSocket.onmessage = function (event) {
		document.write('App has received data: ', event.data,
			'<br>\n');
	}
	webSocket.onclose = function (event) {
		clearInterval(periodicSendsTimer);
		document.write(
			'Connection has been closed by the server<br>\n');
	}
	document.write('Trying to connect to WebSocket server...<br>\n');
</script>
</body></html>
]]

	io:write_header(200, headers, payload, true)
end

local function post_helper_handler(req, io)
	if (req.method ~= 'GET') then
		io:write_header(500, nil, 'Unsupported HTTP method',
			true)
		return
	end

	local headers = {
		['content-type'] = 'text/html; charset=utf-8',
	}
	local payload = [[
<html><head><title>POST helper</title></head><body>
<form action="/post_test" method="post">
  <label for="fname">First name:</label>
  <input type="text" id="fname" name="fname"><br><br>
  <label for="lname">Last name:</label>
  <input type="text" id="lname" name="lname"><br><br>
  <input type="submit" value="Submit">
</form>
</body></html>
]]
	io:write_header(200, headers, payload, true)
end

local function post_test_handler(req, io)
	if (req.method ~= 'POST') then
		io:write_header(500, nil, 'Unsupported HTTP method',
			true)
		return
	end

	local headers = {
		['content-type'] = 'text/html; charset=utf-8',
	}
	local payload
	if req.body == nil then
		payload = 'Empty body'
	else
		payload = 'body="'..req.body..'"'
	end
	io:write_header(200, headers, payload, true)
end

local function put_handler(req, io)
	if (req.method ~= 'PUT') then
		io:write_header(500, nil, 'Unsupported HTTP method',
			true)
		return
	end

	local headers
	local payload
	if req.body == nil then
		headers = {
			['content-type'] = 'text/html; charset=utf-8',
		}
		payload = 'Error: empty body'
	else
		headers = {
			['content-type'] = 'application/octet-stream',
		}
		payload = req.body
	end
	io:write_header(200, headers, payload, true)
end

local httpng_lib = require "httpng"
local init_func = httpng_lib.cfg

local lua_sites = {
	{['path'] = '/hello',     ['handler'] = hello_handler},
	{['path'] = '/large',     ['handler'] = large_handler},
	{['path'] = '/multi',     ['handler'] = multi_handler},
	{['path'] = '/req',       ['handler'] = req_handler},
	{['path'] = '/ws_server', ['handler'] = ws_server_handler},
	{['path'] = '/ws_app',    ['handler'] = ws_app_handler},
	{['path'] = '/post_helper', ['handler'] = post_helper_handler},
	{['path'] = '/post_test', ['handler'] = post_test_handler},
	{['path'] = '/put',       ['handler'] = put_handler},
}

print '\n\n\nFilling in test spaces completed, launching HTTP server...\n\n'
init_func({
	['threads'] = 4,
	['max_conn_per_thread'] = 64,
	['shuttle_size'] = 1024,
	['max_body_len'] = 16 * 1024 * 1024,
	['use_body_split'] = true,
	['sites'] = lua_sites,
})
