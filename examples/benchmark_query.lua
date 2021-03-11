box.cfg{
    listen = 3306,
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

-- For performance reasons only one string with path/query is passed from C.
local function get_query(req)
    if req.query_at == -1 then
        return nil
    end
    return string.sub(req.path, req.query_at, -1)
end

http.cfg{
    listen = {
        { port = 8080 },
    },
    threads = 8,
    handler = function(req, io)
        local payload
        local req_query = get_query(req)
        if req_query then
            local query_str = string.match(req_query, '^?id=%d+')
            if query_str then
                local id_str = string.sub(query_str, 5, -1)
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
