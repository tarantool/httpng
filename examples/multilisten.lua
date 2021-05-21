#!/usr/bin/env tarantool

local box = require 'box'
local http = require 'httpng'
box.cfg {log_level = 7}

http.cfg{
    listen = {
        -- http on IPv4 0.0.0.0:8080 and IPv6 [::]:8080
        8080,

        -- https://[::1]:8443
        {
            -- uses_sni = false (by default)
            addr = '::1',
            port = 8443,
            tls = dofile("examples/paste_foo_ssl_pair.lua"),
        },

        -- https://foo.tarantool.io:8443 or https://bar.tarantool.io:8443 (in /etc/hosts uri should ref to 127.0.0.1)
        -- https://127.0.0.1:8443 is blocked due to SNI.
        {
            addr = '127.0.0.1',
            port = 8443,
            tls = dofile("examples/paste_foo_bar_ssl_pair.lua"),
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
