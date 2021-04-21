local t = require('luatest')
local g = t.group('shutdown')
local http = require 'httpng'

g.test_simple_shutdown = function()
    http.cfg({ handler = function() end })
    http.shutdown()
end

g.test_double_cfg = function()
    http.cfg({ handler = function() end })
    t.assert_error_msg_content_equals('Server is already launched',
        http.cfg, { handler = function() end })
    http.shutdown()
end

g.test_unexpected_shutdown = function()
    t.assert_error_msg_content_equals('Server is not launched',
        http.shutdown)
end

g.test_double_shutdown = function()
    http.cfg({ handler = function() end })
    http.shutdown()
    t.assert_error_msg_content_equals('Server is not launched',
        http.shutdown)
end

