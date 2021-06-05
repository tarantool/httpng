local box = require 'box'
local http = require 'httpng'
local fio = require 'fio'

local test_cases = {
    ['only_port_80'] = 80, -- listen on port 80 and ipv4/6 INADDRANYs
    ['only_http'] = 'http', -- listen on http port 80 and ipv4/6 INADDRANYs

    ['localhost'] = 'localhost',
    ['localhost:443'] = 'localhost:443',
    ['443'] = ':443',

    -- This shouldn't work.
    ['non_string_addr'] = {
        addr = 12, port = 80
    },

    ['http_uri'] = 'http://0.0.0.0:8080',

    -- listen on http port 80 and http port 443
    ['only_port_80_443'] = { 80, 443 },

    ['2_listeners_with_tls'] = {
        -- https://[::1]:8443
        {
            -- uses_sni = false (by default)
            addr = '::', port = 8443, tls = {
                require 'examples.ssl_pairs'.foo,
            }
        },

        -- https://foo.tarantool.io:8443 or https://bar.tarantool.io:8443
        -- (in /etc/hosts uri should ref to 127.0.0.1).
        -- https://127.0.0.1:8443 is blocked due to SNI.
        {
            addr = '127.0.0.1', port = 8443, tls = {
                require 'examples.ssl_pairs'.foo, -- hostname1
                require 'examples.ssl_pairs'.bar, -- hostname2
            },
            uses_sni = true
        }
    },

    ['80_and_2_tables'] = {
        80,
        { addr = '0.0.0.0', port = 8080},
        { addr = '0.0.0.0', port = 8443},
    },
}

http.cfg{
    listen = test_cases[arg[1]],
    threads = 4,
    handler = function(req, io)
        return {
            status = 200,
            headers = {
                ['x-req-id'] = 'qwe',
            },
            body = 'content'
        }

    end
}
