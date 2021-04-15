local http = require 'httpng'

box.cfg {log_level = 7}

http.cfg{
    -- listen = 80, -- listen on http port 80 [WORKS]
    -- listen = 443, -- listen on https(!) port 443 [WORKS]
    -- -- ports 80 and 443 are well-known. same for
    -- listen = 'http', -- listen on http port 80 [WORKS]
    -- listen = 'https', -- listen on https port 443 [WORKS]

    -- --[[ Despriptive way ]]
    -- listen = { addr = '0.0.0.0', port = 80, tls = false, backlog = 4096 },
    -- listen = { addr = '0.0.0.0', port = 443, tls = true, backlog = 4096 },

    -- --[[ Table shortened into uri ]]
    -- -- listen = 'localhost' -- wrong, considered as service
    -- listen = 'localhost:443', -- [WORKS]
    listen = ':443', -- [WORKS]
    -- listen = 'http://0.0.0.0:8080', -- [WORKS]
    -- listen = 'https://0.0.0.0:8000', -- [WORKS]

    -- --[[ Multiple listen ]]
    -- listen = { 80, 443 }, -- listen on http port 80 and https port 443 [WORKS]
    -- listen = { 'http', 'https' }, -- [WORKS]
    -- listen = {
    --     80,
    --     { addr = '0.0.0.0', port = 8080, tls = false, backlog = 4096 },
    --     { addr = '0.0.0.0', port = 8443, tls = true, backlog = 4096, cert = ... },
    -- }, -- listen on http 80, 8080 and https 8443 [WORKS]
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
