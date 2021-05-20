local http = require 'httpng'
dofile('examples/load_ssl_source.lua')


http.cfg{
    listen = {
        {
            port = 8080,
            tls = {
              { certificate_file = foo_cert_path, certificate_key_file = foo_key_path }
            },
        },
    },
    threads = 7,
    openssl_security_level = 0,
    handler = function(req, io)
        return {
            status = 200,
            body = 'Hello, World!'
        }

    end
}
