local fio = require 'fio'

_G['testdir'] = fio.dirname(fio.abspath(arg[0]))
_G['foo_cert_path'] = fio.pathjoin(testdir, '../tests/foo.tarantool.io_cert.pem')
_G['foo_key_path'] = fio.pathjoin(testdir, '../tests/foo.tarantool.io_key.pem')
_G['bar_cert_path'] = fio.pathjoin(testdir, '../tests/bar.tarantool.io_cert.pem')
_G['bar_key_path'] = fio.pathjoin(testdir, '../tests/bar.tarantool.io_key.pem')
