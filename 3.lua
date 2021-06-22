dofile 'tests/all.lua'
local fiber = require 'fiber'

local start_heartbeat = function()
    fiber.new(function()
        ::retry::
        io.write('Heartbeat\n')
	io.flush()
        fiber.sleep(1)
        goto retry
    end)
end

start_heartbeat()
g_good_handlers.test_cancellation_http1_tls()
os.exit()
