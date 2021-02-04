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

local httpng_lib = require "httpng"
local init_func = httpng_lib.init

local sample_site_lib = require "sample_site"
init_func(sample_site_lib.get_site_desc, nil)
