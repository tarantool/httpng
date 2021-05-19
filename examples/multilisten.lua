#!/usr/bin/env tarantool

local http = require 'httpng'
local fio = require 'fio'

local testdir = fio.dirname(fio.abspath(arg[0]))
local foo_cert_path = fio.pathjoin(testdir, '../tests/foo.tarantool.io_cert.pem')
local foo_key_path = fio.pathjoin(testdir, '../tests/foo.tarantool.io_key.pem')
local bar_cert_path = fio.pathjoin(testdir, '../tests/bar.tarantool.io_cert.pem')
local bar_key_path = fio.pathjoin(testdir, '../tests/bar.tarantool.io_key.pem')

-- box.cfg {log_level = 7}

http.cfg{
    listen = {
        -- http on IPv4 0.0.0.0:8080 and IPv6 [::]:8080
        8080,

        -- https://[::1]:8443
        {
            -- uses_sni = false (by default)
            addr = '::1', port = 8443, tls = {
                { certificate_file = foo_cert_path, certificate_key_file = foo_key_path },
            }
        },

        -- https://foo.tarantool.io:8443 or https://bar.tarantool.io:8443 (in /etc/hosts uri should ref to 127.0.0.1)
        -- https://127.0.0.1:8443 is blocked due to SNI.
        {
            addr = '127.0.0.1', port = 8443, tls = {
                { certificate_file = foo_cert_path, certificate_key_file = foo_key_path }, -- hostname1
                { certificate_file = bar_cert_path, certificate_key_file = bar_key_path }, -- hostname2
            },
            uses_sni = true
        }
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
