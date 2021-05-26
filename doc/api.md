# HTTPNG Lua API
(we would probably also implement some kind of C API in the future)

## Quick start

``` lua
local httpng = require 'httpng'
local fiber = require 'fiber' -- Only to use fiber.sleep() in this example

local hello = function(req, io)
    return { body = 'Hello, World!' }
end

local cfg = { handler = hello }

-- Configure and start HTTP(S) server.
httpng.cfg(cfg)

-- Do whatever you please, HTTP(S) server is serving requests now.
fiber.sleep(60)

-- Shut HTTP(S) server down.
httpng.shutdown()
```

## Gory details

httpng module exports the following functions:

- `cfg()`: configure and start HTTP(S) server; reconfigure HTTP(S) server launched earlier (hot reload). Accepts and requires a single parameter - table with server configuration, described below.

- `force_decrease_threads()`: reset TCP connections for terminating threads (those which were told to gracefully terminate by `cfg()` with a lower number of threads than configured earlier), then wait until those threads and corresponding fibers are finished. You can use that when you have used large `thread_termination_timeout` and do not want to wait.

- `shutdown()`: forcefully shut down HTTP(S) server - reset TCP connections, then wait for threads and fibers to finish.

### cfg table

- `handler`: Function, HTTP(S) request handler, this is the "shortcut" which is equivalent to sites entry with `path == '/'`. You can use `sites` or `handler` or both (if sites do not have '/' entry).

- `max_body_len`: Integer, specifies the max size in bytes of HTTP(S) request (not response!) body. Defaults to 1 MiB. Please note that the current implementation of libh2o does not work well with a large request body (too easy to DoS).

- `max_conn_per_thread`: Integer, max number of HTTP(S) TCP connections per thread.

- `min_proto_version`: String, sets minimal accepted SSL/TLS protocol. Defaults to 'tls1.2'. Accepted values are 'ssl3', 'tls1', 'tls1.0', 'tls1.1', 'tls1.2', 'tls1.3'.

- `openssl_security_level`: Integer, defaults to 1. Please see OpenSSL 1.1* documentation for details.

- `shuttle_size`: Integer, specifies the max size in bytes of the internal buffer used to pass data between HTTP(S) server threads and TX thread. Defaults to 65536. One "shuttle" is used for every HTTP(s) request. Lowering its value helps decrease memory usage but limits maximum HTTP(S) request (not response!) body size (but also look on `use_body_split`) as well as the maximal accepted size of request headers.

- `sites`: Array describing HTTP(S) sites, each entry should contain a table with the following fields:
  - `path`: String describing HTTP(S) path like '/foo' or '/'. Please note that order is important - "the first match" rule, in particular, '/' must be the last one. Duplicates are not allowed.
  - `handler`: Function to handle HTTP(S) requests (details are below).

- `threads`: Integer, how many threads to use for HTTP(S) requests processing. It is unlikely that you would need more than 4 because performance is limited by Lua processing which is performed in the TX thread even if you do not access the database. Defaults to 1.

- `thread_termination_timeout`: Number, specifies the number of seconds until TCP connections for gracefully terminating HTTP(S) threads are forcefully reset. Defaults to 60 seconds.

- `use_body_split`: Boolean, whether to split HTTP(S) request body which does not fit into `shuttle_size` into chunks (which means several trips between HTTP(S) thread and TX thread) or fail request. Note that you are limited by `max_body_len` in any case. Defaults to `False`.

### Handlers

`function handler(req, io)`

This is what HTTPNG is about - handling HTTP(S) requests. Handlers are Lua functions that run in separate fibers in the TX thread (and can access the Tarantool database if they want to).

- `req`: Table with the following entries:
  - `is_websocket`: Boolean, is this WebSockets request.
  - `method`: String, 'GET', 'PUT' etc.
  - `path`: String, contains "path" of HTTP(S) request - that is, '/bar?a=b' for "https://foo.com/bar?a=b".
  - `query`: Function, returns "query" (everything after "?" in path) or `nil`.
  - `query_at`: Integer, specifies a 1-based index of "?" in the `path` (-1 if there is none). You are more likely to use `req:query()`.
  - `version_major`: Number, contains "major" part of HTTP version ('2' for 'HTTP 2.0').
  - `version_minor`: Number, contains "minor" part of HTTP version ('0' for 'HTTP 2.0').

- `io`: Table with the following entries:
  - `close()`: function(`io`), equivalent to `io:write(nil, True)` - finishes HTTP(S) request handling. You do not need that in most cases because return from handler always does that.
  - `headers`: Table containing HTTP(S) request headers with entries like `['user-agent'] = 'godzilla'`
  - `shuttle`: Userdata, please do not touch.
  - `upgrade_to_websocket()`: function, to be documented.
  - `write_header`: function(`io, code, headers, body, is_last`), sends HTTP(S) `code` (Integer), `headers` (optional Table with entries like `['content-type'] = 'text/plain; charset=utf-8'`) and `body` (optional String). `is_last` is optional Boolean, set to `True` if there would be no more sends for this request, defaults to `False`. Returns `True` if the connection has already been closed so there is no point in trying to send anything else. `io` is a reference to self - `io:write_header(code, headers, payload, is_last)`. `write_header()` can be called only once per HTTP(S) request.
  - `write`: function(`body, is_last`), sends `body` (String). `is_last` is Boolean, set to `True` if there would be no more sends for this request, defaults to `False`. Returns `True` if the connection has already been closed so there is no point in trying to send anything else. `io` is a reference to self - `io:write(payload, is_last)`. If there was no call to `write_header()` earlier, HTTP(S) code would be 200 and there would be no headers (note that libh2o may add some headers to handle chunked encoding etc).

`handler()` can optionally return a table with `code`, `headers` and `body`, the effect is the same as a call to `io:write_header(code, headers, body, True)`.
