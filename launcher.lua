#!/usr/bin/env tarantool

local fio = require('fio')

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
	-- Array of tables because more than one header can have the same field name (key).
	local headers = {
		{['content-type'] = 'text/plain; charset=utf-8'},
		{['x-custom-header'] = 'foo'},
	}
	local payload = 'Small hello from lua'

	-- Returns nil for now.
	-- Last parameter MUST be true for now.
	local payload_writer = header_writer:write_header(200, headers, payload, true)
end

local function large_handler(req, header_writer)
	-- Array of tables because more than one header can have the same field name (key).
	local headers = {
		{['content-type'] = 'text/plain; charset=utf-8'},
		{['x-custom-header'] = 'foo'},
	}
	local payload = 'Large hello from lua'

	-- Returns nil for now.
	-- Last parameter MUST be true for now.
	local payload_writer = header_writer:write_header(200, headers, payload, true)
end

local httpng_lib = require "httpng"
local init_func = httpng_lib.init

local sample_site_lib = require "sample_site"

local lua_sites = {
	{['path'] = '/lua_large', ['handler'] = large_handler},
	{['path'] = '/lua_hello', ['handler'] = hello_handler},
}

init_func(lua_sites, sample_site_lib.get_site_desc, nil)
