local http = require 'httpng'
dofile('examples/load_ssl_source.lua')


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
    listen = {
        port = 8080,
        tls = {
          { certificate_file = foo_cert_path, certificate_key_file = foo_key_path }
        },
    },
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
