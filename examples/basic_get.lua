local http = require 'httpng'
local fio = require 'fio'

local testdir = fio.dirname(fio.abspath(arg[0]))
local foo_cert_path = fio.pathjoin(testdir, '../tests/foo.tarantool.io_cert.pem')
local foo_key_path = fio.pathjoin(testdir, '../tests/foo.tarantool.io_key.pem')

http.cfg{
    --[[ -- Not yet implemented
    listen = {
        { addr = '0.0.0.0', port = 8080, tls = false, backlog = 4096 },
        { addr = '0.0.0.0', port = 8443, tls = true, backlog = 4096,
            cert = ... },
    },
    ]]--
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
        print(req.headers['content-type'])
        print(req.headers['content-length'])
        print('user-agent: ', req.headers['user-agent'])

        return {
            status = 200,
            headers = {
                ['x-req-id'] = 'qwe',
            },
            body = 'content'
        }

    end
}
