local t = require('luatest')
local g = t.group('shutdown')
local http = require 'httpng'

g.test_simple_shutdown = function()
    http.cfg({ handler = function() end })
    http.shutdown()
end

g.test_shutdown_after_wrong_cfg = function()
    t.assert_error_msg_content_equals('No parameters specified',
        http.cfg)
    t.assert_error_msg_content_equals('Server is not launched',
        http.shutdown)
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

