package = 'httpng'
version = 'scm-1'
source  = {
    url    = 'git://github.com/tarantool/httpng.git',
    branch = 'master',
}
description = {
    summary  = "HTTPNG server for Tarantool",
    homepage = 'https://github.com/tarantool/httpng/',
    license  = 'BSD',
}
dependencies = {
    'lua >= 5.1',
}
external_dependencies = {
    TARANTOOL = {
        header = "tarantool/module.h"
    }
}
build = {
    type = 'cmake',

    variables = {
        version = 'scm-1',
        CMAKE_BUILD_TYPE='RelWithDebInfo',
        TARANTOOL_DIR='$(TARANTOOL_DIR)',
        TARANTOOL_INSTALL_LIBDIR='$(LIBDIR)',
        TARANTOOL_INSTALL_LUADIR='$(LUADIR)',
    }
}

-- vim: syntax=lua
