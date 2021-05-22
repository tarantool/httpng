local t = require 'luatest'
local http = require 'httpng'
local fiber = require 'fiber'

g_repeated_shutdown = t.group('repeated_shutdown')

local shutdown_test_running = false
local function shutdown_fiber_func()
    local cfg = {
        handler = function() end,
        threads = 4,
        thread_termination_timeout = 1,
    }
    http.cfg(cfg)
    fiber.sleep(0.1)
    http.shutdown()
    shutdown_test_running = false
end

local function test_shutdown_once(iteration)
    local timeout = 2
    shutdown_test_running = true
    local f = fiber.new(shutdown_fiber_func)
    f:set_joinable(true)
    local start = fiber.clock()

::again::
    if (fiber.clock() - start > timeout) then
        t.fail('timed out on iteration #'..iteration)
    end
    fiber.sleep(0.1)
    if shutdown_test_running then
        goto again
    end

    f:join()
end

g_repeated_shutdown.test_shutdown = function()
    for i = 1, 100 do
        test_shutdown_once(i)
    end
end
