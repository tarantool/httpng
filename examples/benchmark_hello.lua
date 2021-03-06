local http = require 'httpng'


http.cfg{
    listen = {
        {
            port = 8080,
            tls = { require 'examples.ssl_pairs'.foo },
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
