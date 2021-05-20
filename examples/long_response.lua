local http = require 'httpng'
local fiber = require 'fiber'
dofile('examples/load_ssl_source.lua')

http.cfg{
    --[[
    listen = {
        { addr = '0.0.0.0', port = 8080, tls = false, backlog = 4096 },
        { addr = '0.0.0.0', port = 8443, tls = true, backlog = 4096,
            cert = ... },
    },
    --]]--
    listen = {
        {
            port = 8080,
            tls = {
              { certificate_file = foo_cert_path, certificate_key_file = foo_key_path }
            },
        },
    },
    threads = 4,
    handler = function(req, io)

        io.headers['content-length'] = 20;
        io.headers['x-req-id'] = 'abc';
        io:write_header(200);

        io:write("some data+")
        fiber.sleep(1)
        io:write("some data!")
        io:close()

    end
}
