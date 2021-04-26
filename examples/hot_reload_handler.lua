local http = require 'httpng'
local fiber = require 'fiber'

local foo_handler = function(req, io)
    return {
        status = 200,
        body = 'foo',
    }
end

local bar_handler = function(req, io)
    return {
        status = 200,
        body = 'bar',
    }
end

local config = {
    threads = 4,
    listen =  { { port = 8080 } },
}

::again::

print 'Using foo..'
config.handler = foo_handler;
http.cfg(config)
fiber.sleep(0.1)

print 'Using bar..'
config.handler = bar_handler;
http.cfg(config)

fiber.sleep(0.1)
goto again
