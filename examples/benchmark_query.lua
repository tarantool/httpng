box.cfg{
    listen = 3306,
    wal_mode = 'none',
    checkpoint_interval = 0,
}

local s = box.schema.space.create('tester')
s:format({
    {name = 'id', type = 'unsigned'},
    {name = 'desc', type = 'string'},
})
local index = s:create_index('primary', {
    type = 'tree',
    parts = {'id'}
})

s:insert{1, 'First'}
s:insert{2, 'Second'}

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
    threads = 8,
    openssl_security_level = 0,
    handler = function(req, io)
        local payload
        local req_query = req:query()
        if req_query then
            local query_str = string.match(req_query, '^id=%d+')
            if query_str then
                local id_str = string.sub(query_str, 4, -1)
                local id = tonumber(id_str)
                if id then
                    local tuple = s:get(id)
                    if tuple then
                        payload = tuple.desc
                    else
                        payload = 'Entry was not found'
                    end
                else
                    payload = 'Invalid id was specified (not a number)'
                end
            else
                payload = 'Unable to parse query (format: "?id=3")'
            end
        else
            payload = 'No query specified'
        end

        return {
            status = 200,
            body = payload,
        }

    end
}
