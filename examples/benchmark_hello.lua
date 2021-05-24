local http = require 'httpng'
local fio = require 'fio'

local TESTDIR = fio.dirname(fio.abspath(arg[0]))
local foo_cert_path = fio.pathjoin(TESTDIR, '../tests/foo.tarantool.io_cert.pem')
local foo_key_path = fio.pathjoin(TESTDIR, '../tests/foo.tarantool.io_key.pem')

http.cfg{
    listen = {
        { port = 8080, tls = {
            { certificate_file = foo_cert_path, certificate_key_file = foo_key_path }
          }
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
