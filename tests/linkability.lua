local http = require 'httpng'

local ok, err = pcall(http.cfg, {handler = 'invalid'})
