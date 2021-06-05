local export = {}

export['foo'] = {
    certificate_file = 'tests/foo.tarantool.io_cert.pem',
    certificate_key_file = 'tests/foo.tarantool.io_key.pem'
}
export['bar'] = {
    certificate_file = 'tests/bar.tarantool.io_cert.pem',
    certificate_key_file = 'tests/bar.tarantool.io_key.pem'
}
return export
