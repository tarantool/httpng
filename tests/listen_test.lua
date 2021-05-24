#!/usr/bin/env tarantool

local http = require 'httpng'
local fio = require 'fio'

local TESTDIR = fio.dirname(fio.abspath(arg[0]))
local foo_cert_path = fio.pathjoin(TESTDIR, 'foo.tarantool.io_cert.pem')
local foo_key_path = fio.pathjoin(TESTDIR, 'foo.tarantool.io_key.pem')
local bar_cert_path = fio.pathjoin(TESTDIR, 'bar.tarantool.io_cert.pem')
local bar_key_path = fio.pathjoin(TESTDIR, 'bar.tarantool.io_key.pem')


box.cfg {log_level = 7}

local test_cases = {
	['only_port_80'] = 80, -- listen on port 80 and ipv4/6 INADDRANYs
	['only_http'] = 'http', -- listen on http port 80 and ipv4/6 INADDRANYs

	--[[ Table shortened into uri ]]
	['localhost'] = 'localhost',
	['localhost:443'] = 'localhost:443',
	['443'] = ':443',

	-- This shouldn't work.
	['non_string_addr'] = {
		addr = 12, port = 80
	},

	['http_uri'] = 'http://0.0.0.0:8080', -- [WORKS]

	--[[ Multiple listen ]]
	['only_port_80_443'] = { 80, 443 }, -- listen on http port 80 and http port 443

	-- Despriptive way
	['2_listeners_with_tls'] = {
		-- https://[::1]:8443
		{
			-- uses_sni = false (by default)
			addr = '::', port = 8443, tls = {
				{ certificate_file = foo_cert_path, certificate_key_file = foo_key_path },
			}
		},

		-- https://foo.tarantool.io:8443 or https://bar.tarantool.io:8443 (in /etc/hosts uri should ref to 127.0.0.1)
		-- https://127.0.0.1:8443 is blocked due to SNI.
		{
			addr = '127.0.0.1', port = 8443, tls = {
				{ certificate_file = foo_cert_path, certificate_key_file = foo_key_path }, -- hostname1
				{ certificate_file = bar_cert_path, certificate_key_file = bar_key_path }, -- hostname2
			},
			uses_sni = true
		}
	},

	['80_and_2_tables'] = {
		80,
		{ addr = '0.0.0.0', port = 8080},
		{ addr = '0.0.0.0', port = 8443},
	}, -- listen on
}

http.cfg{
	listen = test_cases[arg[1]],
	threads = 4,
	handler = function(req, io)
		print('host: ', req.headers['host'])
		print('user-agent: ', req.headers['user-agent'])


		return {
			status = 200,
			headers = {
				['x-req-id'] = 'qwe',
			},
			body = 'content'
		}

	end
}
