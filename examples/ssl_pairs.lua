local fio = require 'fio'
local testdir = fio.dirname(fio.abspath(arg[0]))

local export = {}
export['foo'] = {
    certificate_file = fio.pathjoin(testdir, '../tests/foo.tarantool.io_cert.pem'),
    certificate_key_file =  fio.pathjoin(testdir, '../tests/foo.tarantool.io_key.pem')
}
export['bar'] = {
    certificate_file = fio.pathjoin(testdir, '../tests/bar.tarantool.io_cert.pem'),
    certificate_key_file = fio.pathjoin(testdir, '../tests/bar.tarantool.io_key.pem')
}
return export
