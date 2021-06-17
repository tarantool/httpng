local http = require 'httpng'
local fiber = require 'fiber'

local lua_sites = { {path = '/files'} }
local cfg = {
    threads = 4,
    sites = lua_sites,
}

::restart::
lua_sites[1].handler = 'examples/files1'
http.cfg(cfg)
fiber.sleep(1)

lua_sites[1].handler = 'examples/files2'
http.cfg(cfg)
fiber.sleep(1)

goto restart
