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

s:insert{1, 'First'}
s:insert{2, 'Second'}

local counter
for counter = 3, 10000
do
	s:insert{counter, 'Entry #'..counter}
end

local httpng_lib = require "httpng"
local init_func = httpng_lib.init

local sample_site_lib = require "sample_site"
init_func(sample_site_lib.get_site_desc, nil)
