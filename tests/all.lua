local t = require('luatest')
local http = require 'httpng'
local fiber = require 'fiber'
local popen
pcall(function() popen = require 'popen' end)
local curl_bin = 'curl'

local stubborn_handler = function(req, io)
::again::
    local closed = io:write('foobar')
    if closed then
        -- Connection has already been closed
        return
    end
    goto again
end

local stubborn2_handler = function(req, io)
    local start = fiber.clock()
::again::
    local closed = io:write('foobar')
    if closed then
        -- Connection has already been closed
        return
    end
    if (fiber.clock() - start >= 0.5) then
        return
    end
    goto again
end

local shutdown_support_checked = false
local shutdown_works = false

local test_shutdown = function()
    http.cfg{handler = function() end}
    local err
    shutdown_works, err = pcall(http.shutdown)
    shutdown_support_checked = true
    return err
end

local ensure_shutdown_works = function()
    if not shutdown_support_checked then
        test_shutdown()
        assert(shutdown_support_checked)
    end
    t.skip_if(not shutdown_works,
        'This test requires httpng.shutdown() to work')
end

local using_popen = function()
    return popen ~= nil
end

local ensure_can_start_and_kill_processes = function()
    --t.skip('simulating broken process launch or kill')
end

--[[
    Actually it now checks that we can launch processes and kill them,
    not necessarily using popen module (which is more efficient).
    Please use ensure_can_start_and_kill_processes() in new code.
--]]
local ensure_popen = function()
    --t.skip_if(popen == nil, 'This test requires popen')
    ensure_can_start_and_kill_processes()
end

local my_http_cfg = function(cfg)
    http.cfg(cfg)
    if not using_popen() then
        -- FIXME: Wait until initialized? To investigate.
        fiber.sleep(0.1)
    end
end

local get_client_result = function(ph)
    if ph == nil then
        return nil
    end

    local result
    if using_popen() then
        return ph:wait().exit_code
    end

    local ok, status = pcall(http._debug_wait_process, ph)
    if not ok then
        error('Unable to determine process exit code, reason: ' .. status)
    end
    return status
end

local my_shell_internal = function(cmd, stdout)
    if not using_popen() then
        os.remove 'tmp_pid.txt'
        if (os.execute('tests/process_helper ' .. cmd) ~= 0) then
            return nil
        end
    ::retry_pid::
        local file = assert(io.open 'tmp_pid.txt')
        if (file == nil) then
            fiber.sleep(0.001)
            goto retry_pid
        end
        local pid = file:read('*a')
        file:close()
        return pid
    end

    local opts = {}
    opts.shell = true
    opts.setsid = true
    opts.groupsignal = true
    opts.stdout = stdout
    opts.close_fds = false -- That's the point
    return popen.new({cmd}, opts)
end

local my_shell = function(cmd)
    return my_shell_internal(cmd)
end

local my_shell_r = function(cmd)
    return my_shell_internal(cmd, popen.opts.PIPE)
end

g_shuttle_size = t.group('shuttle_size')

--[[ There is no point testing other values - parameters are automatically
saturated for MIN and MAX, only shuttle_size with Lua handlers is "special"
(MIN_shuttle_size is enough for C handlers)
--]]--
g_shuttle_size.test_small_for_lua = function()
    t.assert_error_msg_content_equals(
        'shuttle_size is too small for Lua handlers',
        http.cfg, { shuttle_size = 64, handler = function() end })
end

g_wrong_config = t.group('wrong_config')

g_wrong_config.test_empty_cfg = function()
    t.assert_error_msg_content_equals('No parameters specified', http.cfg)
end

g_wrong_config_requires_shutdown = t.group('wrong_config_requires_shutdown')
g_wrong_config_requires_shutdown.before_each(ensure_shutdown_works)
g_wrong_config_requires_shutdown.test_no_handlers = function()
    t.assert_error_msg_content_equals('No handlers specified', http.cfg, {})
end

g_wrong_config.test_c_sites = function()
    t.assert_error_msg_content_equals('c_sites_func must be a function',
        http.cfg, { c_sites_func = 42 })
end

g_wrong_config.test_c_sites_fail = function()
    t.assert_error_msg_content_equals('c_sites_func() failed',
        http.cfg, { c_sites_func = function() error('') end })
end

g_wrong_config.test_c_sites_wrong = function()
    t.assert_error_msg_content_equals('c_sites_func() returned wrong data type',
        http.cfg, { c_sites_func = function() return 42 end })
end

g_wrong_config.test_wrong_param_type = function()
    t.assert_error_msg_content_equals('parameter threads is not a number',
        http.cfg, { threads = 'test' })
end

g_wrong_config.test_invalid_sites = function()
    t.assert_error_msg_content_equals('sites is not a table',
        http.cfg, { sites = 'test' })
end

g_wrong_config.test_invalid_sites_table = function()
    t.assert_error_msg_content_equals('sites is not a table of tables',
        http.cfg, { sites = { path = 42 } })
end

g_wrong_config.test_invalid_sites_path = function()
    t.assert_error_msg_content_equals('sites[].path is not a string',
        http.cfg, { sites = { { path = 42 } } })
end

g_wrong_config.test_sites_path_is_nil = function()
    t.assert_error_msg_content_equals('sites[].path is nil',
        http.cfg, { sites = { { } } })
end

g_wrong_config.test_handler_is_not_a_function = function()
    t.assert_error_msg_content_equals('handler is not a function',
        http.cfg, { handler = 42 })
end

g_wrong_config.test_sites_handler_is_not_a_function = function()
    t.assert_error_msg_content_equals(
        'sites[].handler is not a function or string',
        http.cfg, { sites = { { path = '/', handler = 42 }, } })
end

g_wrong_config.test_listen_port_invalid = function()
    t.assert_error_msg_content_equals('invalid port specified',
        http.cfg, { handler = function() end, listen = { { port = 77777 } } })
end

g_wrong_config.test_min_proto_version_num = function()
    t.assert_error_msg_content_equals('min_proto_version is not a string',
        http.cfg, { handler = function() end, min_proto_version = 1 })
end

g_wrong_config.test_min_proto_version_invalid = function()
    t.assert_error_msg_content_equals('unknown min_proto_version specified',
        http.cfg, { handler = function() end, min_proto_version = 'ssl2' })
end

g_wrong_config.test_level_nan = function()
    t.assert_error_msg_content_equals('openssl_security_level is not a number',
        http.cfg, { handler = function() end, openssl_security_level = 'ssl' })
end

g_wrong_config.test_level_invalid = function()
    t.assert_error_msg_content_equals('openssl_security_level is invalid',
        http.cfg, { handler = function() end, openssl_security_level = 6 })
end

g_wrong_config.test_many_roots = function()
    t.assert_error_msg_content_equals('There can be only one "/"',
        http.cfg, {
            sites = {
                { path = '/', handler = function() end },
                { path = '/', handler = function() end },
            }
	})
end

g_wrong_config.test_many_roots_alt = function()
    t.assert_error_msg_content_equals('There can be only one "/"',
        http.cfg, {
            sites = { { path = '/', handler = function() end } },
            handler = function() end
        }
    )
end

g_wrong_config.test_paths_after_root = function()
    t.assert_error_msg_content_equals('Can\'t add other paths after adding "/"',
        http.cfg, {
            sites = {
                { path = '/', handler = function() end },
                { path = '/alt', handler = function() end },
            }
	})
end

g_wrong_config.test_dup_paths = function()
    t.assert_error_msg_content_equals("Can't add duplicate paths",
        http.cfg, {
            sites = {
                { path = '/alt', handler = function() end },
                { path = '/alt', handler = function() end },
            }
	})
end

g_shutdown = t.group('shutdown')

g_shutdown.test_simple_shutdown = function()
    local err = test_shutdown()
    assert(shutdown_support_checked)
    t.fail_if(err ~= nil, err)
end

g_shutdown.test_shutdown_after_wrong_cfg = function()
    t.assert_error_msg_content_equals('No parameters specified',
        http.cfg)
    t.assert_error_msg_content_equals('Server is not launched',
        http.shutdown)
end

g_shutdown.test_unexpected_shutdown = function()
    t.assert_error_msg_content_equals('Server is not launched',
        http.shutdown)
end

g_shutdown.test_double_shutdown = function()
    http.cfg({ handler = function() end })
    http.shutdown()
    t.assert_error_msg_content_equals('Server is not launched',
        http.shutdown)
end

g_bad_handlers = t.group 'bad_handlers'
g_bad_handlers.before_each(ensure_shutdown_works)
g_bad_handlers.after_each(function() pcall(http.shutdown) end)

local write_handler_launched = false
local bad_write_ok
local bad_write_err
local write_bad_shuttle_ok
local write_bad_shuttle_err
local write_after_write_with_is_last_ok
local write_after_write_with_is_last_err
local write_after_write_with_is_last_result

local write_header_handler_launched = false
local bad_write_header_ok
local bad_write_header_err
local write_header_bad_shuttle_ok
local write_header_bad_shuttle_err
local write_header_invalid_ok
local write_header_invalid_err
local upgrade_to_websocket_bad_shuttle_ok
local upgrade_to_websocket_bad_shuttle_err
local write_first_header_ok
local write_second_header_ok
local write_second_header_err
local upgrade_to_websocket_ok
local upgrade_to_websocket_err
local close_ok
local close_err
local close_bad_shuttle_ok
local close_bad_shuttle_err
local _

local write_handler = function(req, io)
    write_handler_launched = true
    bad_write_ok, bad_write_err = pcall(io.write, io)

    local saved_shuttle = io._shuttle
    io._shuttle = 42
    write_bad_shuttle_ok, write_bad_shuttle_err = pcall(io.write, io, 'a')
    io._shuttle = saved_shuttle

    io:write('foo', true)
    write_after_write_with_is_last_ok, write_after_write_with_is_last_err,
        write_after_write_with_is_last_result = pcall(io.write, io, 'foo')

    io:close()
end

local write_header_handler = function(req, io)
    write_header_handler_launched = true
    bad_write_header_ok, bad_write_header_err = pcall(io.write_header, io)

    local saved_shuttle = io._shuttle
    io._shuttle = 42
    write_header_bad_shuttle_ok, write_header_bad_shuttle_err =
        pcall(io.write_header, io, 200)
    io._shuttle = saved_shuttle

    write_header_invalid_ok, write_header_invalid_err =
        pcall(io.write_header, io, 'a')

    local saved_shuttle = io._shuttle
    io._shuttle = 42
    upgrade_to_websocket_bad_shuttle_ok, upgrade_to_websocket_bad_shuttle_err =
        pcall(io.upgrade_to_websocket, io)
    io._shuttle = saved_shuttle

    write_first_header_ok, _ = io:write_header(200, nil, 'a', true)
    write_second_header_ok, write_second_header_err =
        pcall(io.write_header, io, 200)

    upgrade_to_websocket_ok, upgrade_to_websocket_err =
        pcall(io.upgrade_to_websocket, io)

    close_ok, close_err = pcall(io.close)

    local saved_shuttle = io._shuttle
    io._shuttle = 42
    close_bad_shuttle_ok, close_bad_shuttle_err = pcall(io.close, io)
    io._shuttle = saved_shuttle

    io:close()
end


local ssl_pairs = require 'tests.ssl_pairs'
local listen_with_single_ssl_pair = {
    tls = {
        ssl_pairs['foo'],
    }
}

local function cfg_bad_handlers(use_tls)
    write_handler_launched = false
    write_header_handler_launched = false
    local cfg = {
        sites = {
            { path = '/write', handler = write_handler },
            { path = '/write_header', handler = write_header_handler },
        },
    }
    if (use_tls) then
        cfg.listen = listen_with_single_ssl_pair
    end
    my_http_cfg(cfg)
end

local test_write_params = function(ver, use_tls)
    ensure_popen()
    cfg_bad_handlers(use_tls)
    t.assert(write_handler_launched == false)
    local protocol
    if use_tls then
        protocol = 'https'
    else
        protocol = 'http'
    end
    local ph = my_shell(curl_bin .. ' -k -s ' .. ver ..
        ' ' .. protocol .. '://localhost:3300/write')
    local result = get_client_result(ph)

    t.assert(result == 0, 'http request failed')
    t.assert(write_handler_launched == true, 'Handler was not launched')
    t.assert(bad_write_ok == false,
        'io:write() with invalid parameter set didn\'t fail')
    t.assert_str_matches(bad_write_err, 'Not enough parameters')

    t.assert(write_bad_shuttle_ok == false,
        'io:write() with corrupt io._shuttle didn\'t fail')
    t.assert_str_matches(write_bad_shuttle_err, 'shuttle is invalid')

    t.assert(write_after_write_with_is_last_ok == false or
        write_after_write_with_is_last_result ~= false,
        'io:write() after io:write(is_last == true) should either error or return true')
    --t.assert_str_matches(write_after_write_with_is_last_err, 'TODO')
end

g_bad_handlers.test_write_params_http1_insecure = function()
    test_write_params('--http1.1')
end

g_bad_handlers.test_write_params_http1_tls = function()
    test_write_params('--http1.1', true)
end

g_bad_handlers.test_write_params_http2_insecure = function()
    test_write_params('--http2')
end

g_bad_handlers.test_write_params_http2_tls = function()
    test_write_params('--http2', true)
end

local test_write_header_params = function(ver, use_tls)
    ensure_popen()
    cfg_bad_handlers(use_tls)
    t.assert(write_header_handler_launched == false)
    local protocol
    if use_tls then
        protocol = 'https'
    else
        protocol = 'http'
    end
    local ph = my_shell(curl_bin .. ' -k -s ' .. ver ..
        ' -o /dev/null ' ..
        ' ' .. protocol .. '://localhost:3300/write_header')
    local result = get_client_result(ph)
    t.assert(result == 0, 'http request failed')
    t.assert(write_header_handler_launched == true, 'Handler was not launched')
    t.assert(bad_write_header_ok == false,
        'io:write_header() with invalid parameter set didn\'t fail')
    t.assert_str_matches(bad_write_header_err, 'Not enough parameters')

    t.assert(write_header_bad_shuttle_ok == false,
        'io:write_header() with corrupt io._shuttle didn\'t fail')
    t.assert_str_matches(write_header_bad_shuttle_err, 'shuttle is invalid')

    t.assert(write_header_invalid_ok == false,
        'io:write_header() with non-integer HTTP code didn\'t fail')
    t.assert_str_matches(write_header_invalid_err,
        'HTTP code is not an integer')

    t.assert(upgrade_to_websocket_bad_shuttle_ok == false,
        'io:upgrade_to_websocket() with corrupt io._shuttle didn\'t fail')
    t.assert_str_matches(upgrade_to_websocket_bad_shuttle_err,
        'shuttle is invalid')

    t.assert(write_first_header_ok == true, 'Valid io:write_header() fail')
    t.assert(write_second_header_ok == false,
        'Second io:write_header() didn\'t fail')
    t.assert_str_matches(write_second_header_err,
        'Handler has already written header')

    t.assert(upgrade_to_websocket_ok == false,
        'io:upgrade_to_websocket() after write_header() didn\'t fail')
    t.assert_str_matches(upgrade_to_websocket_err,
        'Unable to upgrade to WebSockets after sending HTTP headers')

    t.assert(close_ok == false,
        'io:close() with invalid parameter set didn\'t fail')
    t.assert_str_matches(close_err, 'Not enough parameters')

    t.assert(close_bad_shuttle_ok == false,
        'io:close() with corrupt io._shuttle didn\'t fail')
    t.assert_str_matches(close_bad_shuttle_err, 'shuttle is invalid')
end

g_bad_handlers.test_write_header_params_http1_insecure = function()
    test_write_header_params '--http1.1'
end

g_bad_handlers.test_write_header_params_http1_tls = function()
    test_write_header_params('--http1.1', true)
end

g_bad_handlers.test_write_header_params_http2_insecure = function()
    test_write_header_params '--http2'
end

g_bad_handlers.test_write_header_params_http2_tls = function()
    test_write_header_params('--http2', true)
end

g_hot_reload = t.group 'hot_reload'
g_hot_reload.before_each(ensure_shutdown_works)
g_hot_reload.after_each(function() pcall(http.shutdown) end)

local foo_handler = function(req, io)
    return { body = 'foo' }
end

local bar_handler = function(req, io)
    return { body = 'bar' }
end

local version_handler_launched = false
local received_http1_req = false
local received_http2_req = false
local check_http_version_handler = function(req, io)
    version_handler_launched = true
    if (req.version_major == 2) then
        received_http2_req = true
    else
        if (req.version_major == 1) then
            received_http1_req = true
        end
    end
    return {body = 'foo'}
end

local check_site_content = function(ver, proto, location, str)
    ensure_popen()
    local target
    if using_popen() then
        target = ' '
    else
        target = ' -o tmp_curl.txt '
    end
    local cmd = curl_bin .. ' -k -s ' .. ver .. target ..
        proto .. '://' .. location
    local output
    if using_popen() then
        local ph = my_shell_r(cmd)
        local result = ph:wait().exit_code
        output = ph:read()
        assert(result == 0, 'curl failed')
    else
        os.remove('tmp_curl.txt')
        local result = get_client_result(my_shell(cmd))
        assert(result == 0, 'curl failed')
        local file = io.open('tmp_curl.txt')
        if (file == nil) then
            error('nothing read')
        end
        output = file:read('*a')
        file:close()
    end

    if (output ~= str) then
        print('Expected: "'..str..'", actual: "'..output..'"')
        assert(output == str, 'Got unexpected response from HTTP(S) server')
    end
end

local http2_support_checked = false
local http2_supported

local test_curl_supports_v2 = function()
    version_handler_launched = false
    received_http1_req = false
    received_http2_req = false
    my_http_cfg{handler = check_http_version_handler}
    check_site_content('--http2', 'http', 'localhost:3300', 'foo')
    assert(version_handler_launched == true)
    if (not received_http1_req and received_http2_req) then
        http2_supported = true
    end
    http2_support_checked = true
    http.shutdown()
end

local ensure_http2 = function()
    if (not http2_support_checked) then
        test_curl_supports_v2()
        assert(http2_support_checked)
    end
    t.skip_if(not http2_supported, 'This test requires HTTP/2 support in curl')
end

local cfg_for_two_sites = function(cfg, first, second, ver, use_tls)
    local proto
    if (use_tls) then
        cfg.listen = listen_with_single_ssl_pair
        proto = 'https'
    else
        proto = 'http'
    end
    my_http_cfg(cfg)
    local location_main = 'localhost:3300/' .. first
    local location_alt = 'localhost:3300/' .. second

    return proto, location_main, location_alt
end

local test_extra_sites = function(ver, use_tls)
    local cfg = {
        sites = { { path = '/alt', handler = foo_handler } },
        threads = 4,
    }
    local proto, location_main, location_alt =
        cfg_for_two_sites(cfg, '', 'alt', ver, use_tls)
    check_site_content(ver, proto, location_main, 'not found')
    check_site_content(ver, proto, location_alt, 'foo')

    cfg.sites[#cfg.sites + 1] = { path = '/', handler = bar_handler }

    cfg.listen = nil
    my_http_cfg(cfg)
    check_site_content(ver, proto, location_main, 'bar')
    check_site_content(ver, proto, location_alt, 'foo')
end

g_hot_reload.test_extra_sites_http1_insecure = function()
    test_extra_sites '--http1.1'
end

g_hot_reload.test_extra_sites_http1_tls = function()
    test_extra_sites('--http1.1', true)
end

g_hot_reload.test_extra_sites_http2_insecure = function()
    ensure_http2()
    test_extra_sites '--http2'
end

g_hot_reload.test_extra_sites_http2_tls = function()
    ensure_http2()
    test_extra_sites('--http2', true)
end

local test_add_primary_handler = function(ver, use_tls)
    local cfg = {
        sites = { { path = '/alt', handler = foo_handler } },
        threads = 4,
    }
    local proto, location_main, location_alt =
        cfg_for_two_sites(cfg, '', 'alt', ver, use_tls)
    check_site_content(ver, proto, location_main, 'not found')
    check_site_content(ver, proto, location_alt, 'foo')

    cfg.handler = bar_handler
    cfg.listen = nil

    my_http_cfg(cfg)
    check_site_content(ver, proto, location_main, 'bar')
    check_site_content(ver, proto, location_alt, 'foo')
end

g_hot_reload.test_add_primary_handler_http1_insecure = function()
    test_add_primary_handler '--http1.1'
end

g_hot_reload.test_add_primary_handler_http1_tls = function()
    test_add_primary_handler('--http1.1', true)
end

g_hot_reload.test_add_primary_handler_http2 = function()
    ensure_http2()
    test_add_primary_handler '--http2'
end

local test_add_intermediate_site = function(ver, use_tls)
    local cfg = {
        handler = foo_handler,
        threads = 4,
    }
    local proto, location_main, location_alt =
        cfg_for_two_sites(cfg, '', 'alt', ver, use_tls)
    check_site_content(ver, proto, location_main, 'foo')
    check_site_content(ver, proto, location_alt, 'foo')

    cfg.sites = {}
    cfg.sites[#cfg.sites + 1] = { path = '/alt', handler = bar_handler }
    cfg.listen = nil

    my_http_cfg(cfg)
    check_site_content(ver, proto, location_main, 'foo')
    check_site_content(ver, proto, location_alt, 'bar')
end

g_hot_reload.test_add_intermediate_site_http1_insecure = function()
    test_add_intermediate_site '--http1.1'
end

g_hot_reload.test_add_intermediate_site_http1_tls = function()
    test_add_intermediate_site('--http1.1', true)
end

g_hot_reload.test_add_intermediate_site_http2 = function()
    ensure_http2()
    test_add_intermediate_site '--http2'
end

local test_add_intermediate_site_alt = function(ver, use_tls)
    local cfg = {
        sites = { { path = '/', handler = foo_handler } },
        threads = 4,
    }
    local proto, location_main, location_alt =
        cfg_for_two_sites(cfg, '', 'alt', ver, use_tls)
    check_site_content(ver, proto, location_main, 'foo')
    check_site_content(ver, proto, location_alt, 'foo')

    cfg.sites[#cfg.sites + 1] = { path = '/alt', handler = bar_handler }
    cfg.listen = nil

    my_http_cfg(cfg)
    check_site_content(ver, proto, location_main, 'foo')
    check_site_content(ver, proto, location_alt, 'bar')
end

g_hot_reload.test_add_intermediate_site_alt_http1_insecure = function()
    test_add_intermediate_site_alt '--http1.1'
end

g_hot_reload.test_add_intermediate_site_alt_http1_tls = function()
    test_add_intermediate_site_alt('--http1.1', true)
end

g_hot_reload.test_add_intermediate_site_alt_http2 = function()
    ensure_http2()
    test_add_intermediate_site_alt '--http2'
end

local test_add_duplicate_paths = function(ver, use_tls)
    local cfg = {
        sites = { { path = '/foo', handler = foo_handler } },
        threads = 4,
    }
    local proto, location_main, location_alt =
        cfg_for_two_sites(cfg, 'foo', 'bar', ver, use_tls)
    check_site_content(ver, proto, location_main, 'foo')
    check_site_content(ver, proto, location_alt, 'not found')

    cfg.sites[#cfg.sites + 1] = { path = '/bar', handler = bar_handler }
    cfg.sites[#cfg.sites + 1] = { path = '/bar', handler = bar_handler }

    t.assert_error_msg_content_equals("Can't add duplicate paths",
        http.cfg, cfg)
    if not using_popen() then fiber.sleep(0.1) end
    check_site_content(ver, proto, location_main, 'foo')
    check_site_content(ver, proto, location_alt, 'not found')
end

g_hot_reload.test_add_duplicate_paths_http1_insecure = function()
    test_add_duplicate_paths '--http1.1'
end

g_hot_reload.test_add_duplicate_paths_http1_tls = function()
    test_add_duplicate_paths('--http1.1', true)
end

g_hot_reload.test_add_duplicate_paths_http2_insecure = function()
    ensure_http2()
    test_add_duplicate_paths '--http2'
end

g_hot_reload.test_add_duplicate_paths_http2_tls = function()
    ensure_http2()
    test_add_duplicate_paths('--http2', true)
end

local test_add_duplicate_paths_alt = function(ver, use_tls)
    local cfg = {
        sites = { { path = '/', handler = foo_handler } },
        threads = 4,
    }
    local proto, location_main, location_alt =
        cfg_for_two_sites(cfg, '', 'alt', ver, use_tls)
    check_site_content(ver, proto, location_main, 'foo')
    check_site_content(ver, proto, location_alt, 'foo')

    cfg.sites[#cfg.sites + 1] = { path = '/alt', handler = bar_handler }
    cfg.sites[#cfg.sites + 1] = { path = '/alt', handler = bar_handler }

    t.assert_error_msg_content_equals("Can't add duplicate paths",
        http.cfg, cfg)
    if not using_popen() then fiber.sleep(0.1) end

    check_site_content(ver, proto, location_main, 'foo')
    check_site_content(ver, proto, location_alt, 'foo')
end

g_hot_reload.test_add_duplicate_paths_alt_http1_insecure = function()
    test_add_duplicate_paths_alt '--http1.1'
end

g_hot_reload.test_add_duplicate_paths_alt_http1_tls = function()
    test_add_duplicate_paths_alt('--http1.1', true)
end

g_hot_reload.test_add_duplicate_paths_alt_http2 = function()
    ensure_http2()
    test_add_duplicate_paths_alt '--http2'
end

local test_remove_path = function(ver, use_tls)
    local cfg = {
        sites = {
            { path = '/foo', handler = foo_handler },
            { path = '/bar', handler = bar_handler },
        },
        threads = 4,
    }
    local proto, location_main, location_alt =
        cfg_for_two_sites(cfg, 'foo', 'bar', ver, use_tls)
    check_site_content(ver, proto, location_main, 'foo')
    check_site_content(ver, proto, location_alt, 'bar')

    cfg.sites[#cfg.sites] = nil
    cfg.listen = nil

    my_http_cfg(cfg)
    check_site_content(ver, proto, location_main, 'foo')
    check_site_content(ver, proto, location_alt, 'not found')
end

g_hot_reload.test_remove_path_http1_insecure = function()
    test_remove_path '--http1.1'
end

g_hot_reload.test_remove_path_http1_tls = function()
    test_remove_path('--http1.1', true)
end

g_hot_reload.test_remove_path_http2 = function()
    ensure_http2()
    test_remove_path '--http2'
end

local test_remove_all_paths = function(ver, use_tls)
    local cfg = {
        sites = {
            { path = '/', handler = foo_handler },
        },
        threads = 4,
    }
    local proto, location_main, location_alt =
        cfg_for_two_sites(cfg, '', 'bar', ver, use_tls)
    check_site_content(ver, proto, location_main, 'foo')

    cfg.sites = nil
    cfg.listen = nil

    my_http_cfg(cfg)
    check_site_content(ver, proto, location_main, 'not found')
end

g_hot_reload.test_remove_all_paths_http1_insecure = function()
    test_remove_all_paths '--http1.1'
end

g_hot_reload.test_remove_all_paths_http1_tls = function()
    test_remove_all_paths('--http1.1', true)
end

g_hot_reload.test_remove_all_paths_http2 = function()
    ensure_http2()
    test_remove_all_paths '--http2'
end

local test_remove_all_paths_alt = function(ver, use_tls)
    local cfg = {
        handler = foo_handler,
        threads = 4,
    }
    local proto, location_main, location_alt =
        cfg_for_two_sites(cfg, '', 'bar', ver, use_tls)
    check_site_content(ver, proto, location_main, 'foo')

    cfg.handler = nil
    cfg.listen = nil

    my_http_cfg(cfg)
    check_site_content(ver, proto, location_main, 'not found')
end

g_hot_reload.test_remove_all_paths_alt_http1_insecure = function()
    test_remove_all_paths_alt '--http1.1'
end

g_hot_reload.test_remove_all_paths_alt_http1_tls = function()
    test_remove_all_paths_alt('--http1.1', true)
end

g_hot_reload.test_remove_all_paths_alt_http2 = function()
    ensure_http2()
    test_remove_all_paths_alt '--http2'
end

g_hot_reload.test_change_params = function()
    local cfg = {
        sites = { { path = '/write', handler = write_handler } },
        threads = 4,
        max_conn_per_thread = 64,
        shuttle_size = 1024,
        max_body_len = 16 * 1024 * 1024,
        use_body_split = true,
    }

    http.cfg(cfg)

    cfg.max_conn_per_thread = 128
    http.cfg(cfg)

    cfg.max_body_len = 32 * 1024 * 1024
    http.cfg(cfg)

    cfg.use_body_split = false
    http.cfg(cfg)

    cfg.shuttle_size = 2048
    t.assert_error_msg_content_equals(
        "Reconfiguration can't change shuttle_size", http.cfg, cfg)
    cfg.shuttle_size = 1024

    cfg.threads = 5
    http.cfg(cfg)
end

g_hot_reload.test_decrease_threads = function()
    local cfg = {
        handler = foo_handler,
        threads = 4,
    }

    http.cfg(cfg)

    cfg.threads = 3
    http.cfg(cfg)
end

g_hot_reload.test_FLAKY_after_decrease_threads = function()
    local cfg = {
        handler = foo_handler,
        threads = 4,
    }

    http.cfg(cfg)

    cfg.threads = 3
    http.cfg(cfg)

    cfg.threads = 4
    local start = fiber.clock()
::retry::
    local ok, err = pcall(http.cfg, cfg)
    if (not ok) then
        assert(err == 'Unable to reconfigure until threads will shut down')
        assert(fiber.clock() - start < 0.5)
        fiber.sleep(0.01)
        goto retry
    end
end

local curls
g_hot_reload_with_curls = t.group 'hot_reload_with_curls'

local function kill_curls()
    if (curls == nil) then
        return
    end
    if using_popen() then
        for _, curl in pairs(curls) do
            curl:close()
        end
    else
        for _, curl in pairs(curls) do
            os.execute('sh -c "kill ' .. curl .. '" 2>/dev/null')
            pcall(http._debug_wait_process, curl) -- Avoid zombies.
        end
    end
    curls = nil
end

local function shutdown_and_kill_curls()
    pcall(http.shutdown)
    kill_curls()
end

g_hot_reload_with_curls.before_each(ensure_shutdown_works)
g_hot_reload_with_curls.after_each(shutdown_and_kill_curls)

local launch_hungry_curls = function(path, ver, use_tls)
    ensure_popen()
    assert(curls == nil)
    curls = {}
    local curl_count = 48
    local i
    local proto
    if use_tls then
        proto = 'https'
    else
        proto = 'http'
    end
    for i = 1, curl_count do
        curls[#curls + 1] =
            my_shell(curl_bin .. ' ' .. ver ..
                ' -k -s -o /dev/null ' .. proto .. '://' .. path)
    end
end

local test_FLAKY_decrease_stubborn_threads = function(ver, use_tls)
    ensure_popen()
    local cfg = {
        handler = stubborn_handler,
        threads = 2,
    }
    if use_tls then
        cfg.listen = listen_with_single_ssl_pair
    end

    http.cfg(cfg)

    -- We do not (yet?) have API to check that earlier test from combo is done.
    fiber.sleep(0.1)

    launch_hungry_curls('localhost:3300', ver, use_tls)
    fiber.sleep(1)

    cfg.threads = 1
    cfg.listen = nil
    http.cfg(cfg)

    cfg.threads = 2
    local start = fiber.clock()
::retry::
    local ok, err = pcall(http.cfg, cfg)
    assert(not ok, 'httpng.cfg() should fail')
    assert(err == 'Unable to reconfigure until threads will shut down')
    if (fiber.clock() - start >= 1) then
        -- We have waited long enough, this is as it should be.
        http.force_decrease_threads()
        http.cfg(cfg)
        return
    end
    fiber.sleep(0.1)
    goto retry
end

g_hot_reload_with_curls.test_FLAKY_decrease_stubborn_threads_http1_insecure =
        function()
    test_FLAKY_decrease_stubborn_threads '--http1.1'
end

g_hot_reload_with_curls.test_FLAKY_decrease_stubborn_threads_http1_tls =
        function()
    test_FLAKY_decrease_stubborn_threads('--http1.1', true)
end

g_hot_reload_with_curls.test_FLAKY_decrease_stubborn_threads_http2_insecure =
        function()
    ensure_http2()
    test_FLAKY_decrease_stubborn_threads '--http2'
end

g_hot_reload_with_curls.test_FLAKY_decrease_stubborn_threads_http2_tls =
        function()
    ensure_http2()
    test_FLAKY_decrease_stubborn_threads('--http2', true)
end

local test_FLAKY_decrease_stubborn_threads_with_timeout =
        function(ver, use_tls)
    ensure_popen()
    local cfg = {
        handler = stubborn_handler,
        threads = 2,
        thread_termination_timeout = 2,
    }
    if use_tls then
        cfg.listen = listen_with_single_ssl_pair
    end

    my_http_cfg(cfg)

    local curls_start = fiber.clock()
    launch_hungry_curls('localhost:3300', ver, use_tls)
    fiber.sleep(0.1)

    cfg.threads = 1
    cfg.listen = nil
    local start = fiber.clock()
    http.cfg(cfg)

    cfg.threads = 2
::retry::
    local ok, err = pcall(http.cfg, cfg)
    if (ok) then
        local now = fiber.clock()
        if (not(now - start >= cfg.thread_termination_timeout - 0.5)) then
            print('now - start = ', now - start)
            print('now - curls_start = ', now - curls_start)
            print('cfg.thread_termination_timeout = ',
                cfg.thread_termination_timeout)
            error('threads have terminated too early');
        end
        return
    end
    assert(err == 'Unable to reconfigure until threads will shut down')
    assert(fiber.clock() - start < cfg.thread_termination_timeout + 1,
        'threads have terminated too late')
    fiber.sleep(0.05)
    goto retry
end

g_hot_reload_with_curls.test_FLAKY_decrease_stubborn_threads_with_timeout_h1i =
        function()
    test_FLAKY_decrease_stubborn_threads_with_timeout '--http1.1'
end

g_hot_reload_with_curls.test_FLAKY_decrease_stubborn_threads_with_timeout_h1s =
        function()
    test_FLAKY_decrease_stubborn_threads_with_timeout('--http1.1', true)
end

g_hot_reload_with_curls.test_FLAKY_decrease_stubborn_threads_with_timeout_h2i =
        function()
    ensure_http2()
    test_FLAKY_decrease_stubborn_threads_with_timeout '--http2'
end

g_hot_reload_with_curls.test_FLAKY_decrease_stubborn_threads_with_timeout_h2s =
        function()
    ensure_http2()
    test_FLAKY_decrease_stubborn_threads_with_timeout('--http2', true)
end

local test_FLAKY_decrease_not_so_stubborn_thr_with_timeout =
        function(ver, use_tls)
    ensure_popen()
    local cfg = {
        handler = stubborn2_handler,
        threads = 2,
        thread_termination_timeout = 2,
    }
    if use_tls then
        cfg.listen = listen_with_single_ssl_pair
    end

    my_http_cfg(cfg)

    assert(cfg.thread_termination_timeout > 0.1)
    local curls_start = fiber.clock()
    launch_hungry_curls('localhost:3300', ver, use_tls)
    fiber.sleep(0.1)

    cfg.threads = 1
    cfg.listen = nil
    local start = fiber.clock()
    http.cfg(cfg)

    cfg.threads = 2
::retry::
    local ok, err = pcall(http.cfg, cfg)
    if (ok) then
        local now = fiber.clock()
        if (now - start < 0.4) then
            print('now - start = ', now - start)
            print('now - curls_start = ', now - curls_start)
            error('threads have terminated too early');
        end
        assert(fiber.clock() - start < cfg.thread_termination_timeout + 0.5,
            'threads have terminated too late');
        return
    end
    assert(err == 'Unable to reconfigure until threads will shut down')
    assert(fiber.clock() - start < cfg.thread_termination_timeout + 0.5)
    fiber.sleep(0.05)
    goto retry
end

g_hot_reload_with_curls.test_FLAKY_decr_not_so_stubborn_thr_with_timeout_h1i =
        function()
    test_FLAKY_decrease_not_so_stubborn_thr_with_timeout '--http1.1'
end

g_hot_reload_with_curls.test_FLAKY_decr_not_so_stubborn_thr_with_timeout_h1s =
        function()
    test_FLAKY_decrease_not_so_stubborn_thr_with_timeout('--http1.1', true)
end

g_hot_reload_with_curls.test_FLAKY_decr_not_so_stubborn_thr_with_timeout_h2i =
        function()
    ensure_http2()
    test_FLAKY_decrease_not_so_stubborn_thr_with_timeout '--http2'
end

g_hot_reload_with_curls.test_FLAKY_decr_not_so_stubborn_thr_with_timeout_h2s =
        function()
    ensure_http2()
    test_FLAKY_decrease_not_so_stubborn_thr_with_timeout('--http2', true)
end

local alt_foo_handler = function(req, io)
    return { body = 'FOO' }
end

local alt_bar_handler = function(req, io)
    return { body = 'BAR' }
end

local test_replace_handlers = function(ver, use_tls)
    local cfg = {
        handler = foo_handler,
        sites = { { path = '/alt', handler = alt_foo_handler } },
        threads = 4,
    }
    local proto, location_main, location_alt =
        cfg_for_two_sites(cfg, '', 'alt', ver, use_tls)

    check_site_content(ver, proto, location_main, 'foo')
    check_site_content(ver, proto, location_alt, 'FOO')

    cfg.handler = bar_handler
    cfg.sites[1].handler = alt_bar_handler
    cfg.listen = nil
    my_http_cfg(cfg)

    check_site_content(ver, proto, location_main, 'bar')
    check_site_content(ver, proto, location_alt, 'BAR')
end

g_hot_reload.test_replace_handlers_http1_insecure = function()
    test_replace_handlers '--http1.1'
end

g_hot_reload.test_replace_handlers_http1_tls = function()
    test_replace_handlers('--http1.1', true)
end

g_hot_reload.test_replace_handlers_http2_insecure = function()
    ensure_http2()
    test_replace_handlers '--http2'
end

g_hot_reload.test_replace_handlers_http2_tls = function()
    ensure_http2()
    test_replace_handlers('--http2', true)
end

g_hot_reload.test_force_decrease_threads = function()
    t.assert_error_msg_content_equals(
        'Not configured, nothing to terminate', http.force_decrease_threads)
end

g_good_handlers = t.group 'good_handlers'
g_good_handlers.before_each(ensure_shutdown_works)
g_good_handlers.after_each(shutdown_and_kill_curls)

local query_handler = function(req, io)
    if (req.method ~= 'GET') then
        return {status = 500, body = 'Unsupported HTTP method'}
    end
    local payload
    if req.query == 'id=2' then
        payload = 'good'
    else
        payload = 'bad'
    end
    return {body = payload}
end

local test_something = function(ver, use_tls, query, handler, expected)
    local cfg = {handler = handler}
    local proto
    if use_tls then
        cfg.listen = listen_with_single_ssl_pair
        proto = 'https'
    else
        proto = 'http'
    end
    my_http_cfg(cfg)
    check_site_content(ver, proto, 'localhost:3300' .. query, expected)
end

local test_some_query = function(ver, use_tls, query, expected)
    test_something(ver, use_tls, query, query_handler, expected)
end

local test_expected_query = function(ver, use_tls)
    test_some_query(ver, use_tls, '?id=2', 'good')
end

g_good_handlers.test_expected_query_http1_insecure = function()
    test_expected_query '--http1.1'
end

g_good_handlers.test_expected_query_http1_tls = function()
    test_expected_query('--http1.1', true)
end

g_good_handlers.test_expected_query_http2_insecure = function()
    ensure_http2()
    test_expected_query '--http2'
end

g_good_handlers.test_expected_query_http2_tls = function()
    ensure_http2()
    test_expected_query('--http2', true)
end

local test_unexpected_query = function(ver, use_tls)
    test_some_query(ver, use_tls, '?id=3', 'bad')
end

g_good_handlers.test_unexpected_query_http1_insecure = function()
    test_unexpected_query '--http1.1'
end

g_good_handlers.test_unexpected_query_http1_tls = function()
    test_unexpected_query('--http1.1', true)
end

g_good_handlers.test_unexpected_query_http2_insecure = function()
    ensure_http2()
    test_unexpected_query '--http2'
end

g_good_handlers.test_unexpected_query_http2_tls = function()
    ensure_http2()
    test_unexpected_query('--http2', true)
end

local test_no_query = function(ver, use_tls)
    test_some_query(ver, use_tls, '', 'bad')
end

g_good_handlers.test_no_query_http1_insecure = function()
    test_no_query '--http1.1'
end

g_good_handlers.test_no_query_http1_tls = function()
    test_no_query('--http1.1', true)
end

g_good_handlers.test_no_query_http2 = function()
    ensure_http2()
    test_no_query '--http2'
end

g_good_handlers.test_curl_supports_v1 = function()
    version_handler_launched = false
    received_http1_req = false
    received_http2_req = false
    my_http_cfg{handler = check_http_version_handler}
    check_site_content('--http1.1', 'http', 'localhost:3300', 'foo')

    assert(version_handler_launched == true)
    assert(received_http1_req == true)
    assert(received_http2_req == false)
end

g_good_handlers.test_curl_supports_v2 = function()
    if (not http2_support_checked) then
        test_curl_supports_v2()
        assert(http2_support_checked)
    end
    assert(http2_supported,
        'http/2 support in curl is required to test everything fully')
end

g_wrong_config.test_combo3 = function()
    -- Crash or ASAN failure on broken versions.
    http._cfg_debug{inject_shutdown_error = true}
    pcall(g_shutdown.test_simple_shutdown)
    pcall(g_wrong_config.test_no_handlers)
    pcall(g_wrong_config.test_sites_handler_is_not_a_function)
    http._cfg_debug{inject_shutdown_error = false}
    http.shutdown()
    shutdown_works = true
end

g_wrong_config.test_combo4 = function()
    -- ASAN failure on broken versions.
    http._cfg_debug{inject_shutdown_error = true}
    pcall(g_shutdown.test_simple_shutdown)
    pcall(g_wrong_config.test_no_handlers)
    pcall(g_wrong_config.test_listen_port_root)
    pcall(g_wrong_config.test_many_roots_alt)
    pcall(g_wrong_config.test_paths_after_root)
    http._cfg_debug{inject_shutdown_error = false}
    http.shutdown()
    shutdown_works = true
end

g_wrong_config.test_combo5 = function()
    -- ASAN failure on broken versions.
    http._cfg_debug{inject_shutdown_error = true}
    pcall(g_shutdown.test_simple_shutdown)
    pcall(g_wrong_config.test_invalid_sites)
    pcall(g_wrong_config.test_handler_is_not_a_function)
    pcall(g_wrong_config.test_sites_handler_is_not_a_function)
    http._cfg_debug{inject_shutdown_error = false}
    http.shutdown()
    shutdown_works = true
end

local test_cancellation = function(ver, use_tls)
    ensure_can_start_and_kill_processes()
    local cfg = {
        handler = query_handler, -- Almost any (not stubborn!)
        threads = 4,
    }
    if use_tls then
        cfg.listen = listen_with_single_ssl_pair
    end

    http.cfg(cfg)
    for _ = 1, 100 do
        launch_hungry_curls('localhost:3300', ver, use_tls)
        fiber.sleep(0.01)
        kill_curls()
    end
end

g_good_handlers.test_cancellation_http1_tls = function()
    test_cancellation('--http1.1', true)
end

g_good_handlers.test_cancellation_http2_tls = function()
    ensure_http2()
    test_cancellation('--http2', true)
end

g_good_handlers.test_cancellation_http1_insecure = function()
    test_cancellation('--http1.1')
end

g_good_handlers.test_cancellation_http2_insecure = function()
    ensure_http2()
    test_cancellation('--http2')
end

local test_host = function(ver, use_tls)
    local cfg = { handler = function(req) return {body = req.host} end }
    local proto
    if use_tls then
        proto = 'https'
        cfg.listen = {
            tls = { ssl_pairs['foo'], ssl_pairs['bar'] },
            uses_sni = true
        }
    else
        proto = 'http'
    end
    my_http_cfg(cfg)

    check_site_content(ver, proto, 'foo.tarantool.io:3300', 'foo.tarantool.io')
    check_site_content(ver, proto, 'bar.tarantool.io:3300', 'bar.tarantool.io')

    if use_tls then
        local ok, err = pcall(check_site_content, ver, proto,
            'localhost:3300', 'localhost')
        assert(ok == false)
    else
        check_site_content(ver, proto, 'localhost:3300', 'localhost')
    end
end

g_good_handlers.test_host_http1_tls = function()
    test_host('--http1.1', true)
end

g_good_handlers.test_host_http1_insecure = function()
    test_host('--http1.1')
end

g_good_handlers.test_host_http2_tls = function()
    ensure_http2()
    test_host('--http2', true)
end

g_good_handlers.test_host_http2_insecure = function()
    ensure_http2()
    test_host('--http2')
end

local encryption_handler = function(req, io)
    if req.https then
        return {body = 'encrypted'}
    end
    return {body = 'insecure'}
end

local test_req_encryption_info = function(ver)
    my_http_cfg{
        listen = { 8080, { port = 3300, tls = { ssl_pairs['foo'] } } },
        handler = encryption_handler,
    }

    check_site_content(ver, 'http', 'foo.tarantool.io:8080', 'insecure')
    check_site_content(ver, 'https', 'foo.tarantool.io:3300', 'encrypted')
end

g_good_handlers.test_req_encryption_info_http1 = function()
    ensure_http2()
    test_req_encryption_info('--http1.1')
end

g_good_handlers.test_req_encryption_info_http2 = function()
    ensure_http2()
    test_req_encryption_info('--http2')
end

local write_header_handler2 = function(req, io)
    io:write_header(200, nil, 'foo')
end

local write_handler2 = function(req, io)
    io:write('foo')
end

local faulty_handler = function()
    error 'foo'
end

local test_some_handler = function(ver, use_tls, handler)
    test_something(ver, use_tls, '', handler, 'foo')
end

local test_write_header_handler = function(ver, use_tls)
    test_some_handler(ver, use_tls, write_header_handler2)
end

local test_write_handler = function(ver, use_tls)
    test_some_handler(ver, use_tls, write_handler2)
end

local test_faulty_handler = function(ver, use_tls)
    test_something(ver, use_tls, '', faulty_handler,
        'Path handler execution error')
end

g_good_handlers.test_write_header_handler_http2_tls = function()
    ensure_http2()
    test_write_header_handler('--http2', true)
end

g_good_handlers.test_write_header_handler_http2_insecure = function()
    ensure_http2()
    test_req_encryption_info('--http2')
end

g_good_handlers.test_write_header_handler_http1_tls = function()
    test_write_header_handler('--http1.1', true)
end

g_good_handlers.test_write_header_handler_http1_insecure = function()
    test_req_encryption_info('--http1.1')
end

g_good_handlers.test_write_handler_http2_tls = function()
    ensure_http2()
    test_write_handler('--http2', true)
end

g_good_handlers.test_write_handler_http2_insecure = function()
    ensure_http2()
    test_write_handler('--http2')
end

g_good_handlers.test_write_handler_http1_tls = function()
    test_write_handler('--http1.1', true)
end

g_good_handlers.test_write_handler_http1_insecure = function()
    test_write_handler('--http1.1')
end

g_bad_handlers.test_faulty_handler_http2_tls = function()
    ensure_http2()
    test_faulty_handler('--http2', true)
end

g_bad_handlers.test_faulty_handler_http2_insecure = function()
    ensure_http2()
    test_faulty_handler('--http2')
end

g_bad_handlers.test_faulty_handler_http1_tls = function()
    test_faulty_handler('--http1.1', true)
end

g_bad_handlers.test_faulty_handler_http1_insecure = function()
    test_faulty_handler('--http1.1')
end

g_hot_reload.test_min_proto = function()
    local cfg = { handler = function() end, min_proto_version = 'tls1.2'}
    http.cfg(cfg)
    cfg.min_proto_version = 'tls1.3'
    t.assert_error_msg_content_equals(
        "min_proto_version can't be changed on reconfiguration", http.cfg, cfg)
end

g_hot_reload.test_security_level = function()
    local cfg = { handler = function() end, openssl_security_level = 1}
    http.cfg(cfg)
    cfg.openssl_security_level = 2
    t.assert_error_msg_content_equals(
        "openssl_security_level can't be changed on reconfiguration",
        http.cfg, cfg)
end

g_hot_reload.test_listen_change = function()
    local cfg = { handler = function() end, listen = 3300}
    http.cfg(cfg)
    cfg.listen = 8080
    t.assert_error_msg_content_equals(
        "listen can't be changed on reconfiguration",
        http.cfg, cfg)
end

g_hot_reload.test_null_threads = function()
    http.cfg{threads = 4, handler = function() end}
    http.cfg{threads = box.NULL}
end

g_hot_reload.test_null_thread_termination_timeout = function()
    http.cfg{thread_termination_timeout = 5, handler = function() end}
    http.cfg{thread_termination_timeout = box.NULL}
end

g_hot_reload.test_null_shuttle_size = function()
    http.cfg{shuttle_size = 1024, handler = function() end}
    t.assert_error_msg_content_equals(
        "Reconfiguration can't change shuttle_size",
        http.cfg, {shuttle_size = box.NULL})
end

g_hot_reload.test_change_min_proto_version = function()
    http.cfg{min_proto_version = 'tls1.3', handler = function() end}
    t.assert_error_msg_content_equals(
        "min_proto_version can't be changed on reconfiguration",
        http.cfg, {min_proto_version = 'tls1.2'})
end

g_hot_reload.test_null_min_proto_version = function()
    http.cfg{min_proto_version = 'tls1.3', handler = function() end}
    t.assert_error_msg_content_equals(
        "min_proto_version can't be changed on reconfiguration",
        http.cfg, {min_proto_version = box.NULL})
end

g_hot_reload.test_null_security_level = function()
    http.cfg{openssl_security_level = 5, handler = function() end}
    t.assert_error_msg_content_equals(
        "openssl_security_level can't be changed on reconfiguration",
        http.cfg, {openssl_security_level = box.NULL})
end

g_hot_reload.test_null_listen = function()
    http.cfg{listen = 8080, handler = function() end}
    t.assert_error_msg_content_equals(
        "listen can't be changed on reconfiguration",
        http.cfg, {listen = box.NULL})
end

g_hot_reload.test_reload_file = function()
    local filename = 'tmp_reload_test'
    local filename_ext = filename .. '.lua'
    local file = io.open(filename_ext, 'w')
    file:write(
    [[
    local handler = function(req, io)
        return { body = 'foo' }
    end

    return {handler = handler}
    ]]
    )
    file:close()

    http.cfg{handler = require(filename).handler}
    check_site_content('', 'http', 'localhost:3300', 'foo')
    local file = io.open(filename_ext, 'w')
    file:write(
    [[
    local handler = function(req, io)
        return { body = 'bar' }
    end

    return {handler = handler}
    ]]
    )
    file:close()
    package.loaded[filename] = nil
    http.cfg{handler = require(filename).handler}
    check_site_content('', 'http', 'localhost:3300', 'bar')
end
