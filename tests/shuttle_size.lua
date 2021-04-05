local t = require('luatest')
local g = t.group('shuttle_size')
local http = require 'httpng'

--[[ There is no point testing other values - parameters are automatically
saturated for MIN and MAX, only shuttle_size with Lua handlers is "special"
(MIN_shuttle_size is enough for C handlers)
--]]--
g.test_small_for_lua = function()
    t.assert_error_msg_content_equals('shuttle_size is too small for Lua handlers',
        http.cfg, { shuttle_size = 64, handler = function() end })
end

