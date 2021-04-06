local t = require('luatest')
local g = t.group('wrong_config')
local http = require 'httpng'

g.test_empty_cfg = function()
    t.assert_error_msg_content_equals('No parameters specified', http.cfg)
end

g.test_no_handlers = function()
    t.assert_error_msg_content_equals('No handlers specified', http.cfg, {})
end

g.test_c_sites = function()
    t.assert_error_msg_content_equals('c_sites_func must be function',
        http.cfg, { c_sites_func = 42 })
end

g.test_c_sites_fail = function()
    t.assert_error_msg_content_equals('c_sites_func() failed',
        http.cfg, { c_sites_func = function() error('') end })
end

g.test_c_sites_wrong = function()
    t.assert_error_msg_content_equals('c_sites_func() returned wrong data type',
        http.cfg, { c_sites_func = function() return 42 end })
end

g.test_wrong_param_type = function()
    t.assert_error_msg_content_equals('parameter threads is not a number',
        http.cfg, { threads = 'test' })
end

g.test_invalid_sites = function()
    t.assert_error_msg_content_equals('sites is not a table',
        http.cfg, { sites = 'test' })
end

g.test_invalid_sites_table = function()
    t.assert_error_msg_content_equals('sites is not a table of tables',
        http.cfg, { sites = { path = 42 } })
end

g.test_invalid_sites_path = function()
    t.assert_error_msg_content_equals('sites[].path is not a string',
        http.cfg, { sites = { { path = 42 } } })
end

g.test_sites_path_is_nil = function()
    t.assert_error_msg_content_equals('sites[].path is nil',
        http.cfg, { sites = { { } } })
end

g.test_handler_is_not_a_function = function()
    t.assert_error_msg_content_equals('handler is not a function',
        http.cfg, { handler = 42 })
end

g.test_sites_handler_is_not_a_function = function()
    t.assert_error_msg_content_equals('sites[].handler is not a function',
        http.cfg, { sites = { { path = '/', handler = 42 }, } })
end

g.test_listen_not_a_table = function()
    t.assert_error_msg_content_equals('listen is not a table',
        http.cfg, { handler = function() end, listen = 42 })
end

g.test_listen_invalid = function()
    t.assert_error_msg_content_equals('listen is not a table of tables',
        http.cfg, { handler = function() end, listen = { port = 8080 } })
end

g.test_listen_port_invalid = function()
    t.assert_error_msg_content_equals('invalid port specified',
        http.cfg, { handler = function() end, listen = { { port = 77777 } } })
end

g.test_listen_port_root = function()
    t.assert_error_msg_content_equals('Failed to listen',
        http.cfg, { handler = function() end, listen = { { port = 80 } } })
end

g.test_min_proto_version_num = function()
    t.assert_error_msg_content_equals('min_proto_version is not a string',
        http.cfg, { handler = function() end, min_proto_version = 1 })
end

g.test_min_proto_version_invalid = function()
    t.assert_error_msg_content_equals('unknown min_proto_version specified',
        http.cfg, { handler = function() end, min_proto_version = 'ssl2' })
end

g.test_level_nan = function()
    t.assert_error_msg_content_equals('openssl_security_level is not a number',
        http.cfg, { handler = function() end, openssl_security_level = 'ssl' })
end

g.test_level_invalid = function()
    t.assert_error_msg_content_equals('openssl_security_level is invalid',
        http.cfg, { handler = function() end, openssl_security_level = 6 })
end

g.test_simple = function()
    http.cfg( { handler = function() end } )
end
