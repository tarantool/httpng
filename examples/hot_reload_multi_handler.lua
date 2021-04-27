box.cfg{
    listen = 3306,
    wal_mode = 'none',
    checkpoint_interval = 0,
}

local s = box.schema.space.create('tester')
s:format({
    {name = 'id', type = 'unsigned'},
    {name = 'desc', type = 'string'},
})
local index = s:create_index('primary', {
    type = 'tree',
    parts = {'id'}
})

s:insert{1, 'First'}
s:insert{2, 'Second'}

local http = require 'httpng'
local fiber = require 'fiber'

local touch_db = function()
    local tuple = s:get(2)
end

local foo_handler = function(req, io)
    touch_db()
    return {
        status = 200,
        body = 'foo',
    }
end

local alt_foo_handler = function(req, io)
    touch_db()
    return {
        status = 200,
        body = 'FOO',
    }
end

local bar_handler = function(req, io)
    touch_db()
    return {
        status = 200,
        body = 'bar',
    }
end

local alt_bar_handler = function(req, io)
    touch_db()
    return {
        status = 200,
        body = 'BAR',
    }
end

local config = {
    threads = 4,
    listen =  { { port = 8080 } },
}

::again::

print 'Using foo..'
config.handler = foo_handler;
config.sites = {
    {path = '/subdir',       handler = alt_foo_handler},
}
http.cfg(config)
fiber.sleep(0.1)

print 'Using bar..'
config.sites = {
    {path = '/subdir',       handler = alt_bar_handler},
}
config.handler = bar_handler;
http.cfg(config)

fiber.sleep(0.1)
goto again
