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

httpng module exports the following:

- `cfg()`: configure and start HTTP(S) server; reconfigure HTTP(S) server launched earlier (hot reload). Accepts and requires a single parameter - table with server configuration, described below (with hot reload details).

- `delete()`: proxied to `handler.delete()`.

- `force_decrease_threads()`: reset TCP connections for terminating threads (those which were told to gracefully terminate by `cfg()` with a lower value of `threads` than configured earlier), then wait until those threads and corresponding fibers are finished. You can use that when you have used large `thread_termination_timeout` and do not want to wait.

- `handler`: handler function or router object currently used by HTTP(S) server.

- `route()`: proxied to `handler.route()`.

- `router.new()`: create new router object (Lua table technically)
which can be configured and used as a handler.

- `shutdown()`: forcefully shut down HTTP(S) server - reset TCP connections, then wait for threads and fibers to finish.

### cfg table

- `handler`: HTTP(S) request handler - a function (which handles all requests)
or a router object
(several user handlers for different paths,
(TODO) response templates,
(TODO) native file/folder hosting,
(TODO) authentification/authorization,
(TODO) custom pre/postprocessing,
etc).

- `max_body_len`: Integer, specifies the max size in bytes of HTTP(S) request (not response!) body. Defaults to 1 MiB. Please note that the current implementation of libh2o does not work well with a large request body (too easy to DoS).

- `max_conn_per_thread`: Integer, max number of HTTP(S) TCP connections per thread.

- `min_proto_version`: String, sets minimal accepted SSL/TLS protocol. Defaults to 'tls1.2'. Accepted values are 'ssl3', 'tls1', 'tls1.0', 'tls1.1', 'tls1.2', 'tls1.3'.

- `openssl_security_level`: Integer, defaults to 1. Please see OpenSSL 1.1* documentation for details.

- `shuttle_size`: Integer, specifies the max size in bytes of
the internal buffer used to pass data between HTTP(S) server threads
and TX thread. Defaults to 65536.
One "shuttle" is used for every HTTP(S) request.
Lowering its value helps decrease memory usage but limits maximum HTTP(S)
request (not HTTP(S) response!) body size (but also look on `use_body_split`)
as well as the maximal accepted size of request headers.

- `threads`: Integer, how many threads to use for HTTP(S) requests processing. It is unlikely that you would need more than 4 because performance is limited by Lua processing which is performed in the TX thread even if you do not access the database. Defaults to 1.

- `thread_termination_timeout`: Number, specifies the number of seconds until TCP connections for gracefully terminating HTTP(S) threads are forcefully reset. Defaults to 60 seconds.

- `use_body_split`: Boolean, whether to split HTTP(S) request body which does not fit into `shuttle_size` into chunks (which means several trips between HTTP(S) thread and TX thread) or fail request. Note that you are limited by `max_body_len` in any case. Defaults to `False`.

When you are calling `cfg()` after successful call to `cfg()`,
it performs reconfiguration (hot reload).
You can replace `handler`, increase or decrease `threads`,
change `thread_termination_timeout`.
You can also change the router object with corresponding functions
and these changes would affect the running server.
Attempts to change other parameters would throw an error.

### Route specifications

The concept is compatible with tarantool/http v1
(https://github.com/tarantool/http/tree/1.1.0) which is inspired by
[Mojolicious](http://mojolicio.us/perldoc/Mojolicious/Guides/Routing) API.

You can modify a router object by calling
`delete(o, entry_paths)`,
`route(o, entry, handler, before_which)`.
`o` is router object - you can use standard Lua syntax sugar `:` and omit `o`.
`entry` is a single route entry,
`handler` is a function to handle matching HTTP(S) requests,
 `before_witch` is a string specifying `path` of
an existing entry (optional, default is to add to the tail).
`entry_paths` is a string or an array of strings specifying `path`s
of existing entries to remove.
You can also use tables with `path`
instead of strings (all other fields are ignored).

Each route entry can contain the following fields:
- `host`: String, virtual host name like `foo.tarantool.io` (optional).
Exact match only. TODO: Placeholders support?
- `method`: String, HTTP(S) method (`POST`, `GET`, etc). Optional.
- `path`: String, HTTP(S) path expression, must start with `/` and can use
placeholders (see description below).

Entries are processed in order. It is recommended to place more frequently
accessed paths first to improve performance.

The following placeholders specifications are supported:
- Standard: colon prefix followed by a name, e. g. `:name`.
These placeholders would match all characters except "." and "/".
E. g. HTTP(S) request for "/sebastian/hello" with route `path` specification
"/:name/hello" would match and create stash entry "name" with value "sebastian".
You can optionally use angle brackets to separate placeholder name from the
surrounding characters, e. g. "/<:name>hello" would match "/sebastianhello".
Please see Mojolicious documentation for more examples.
Colon prefix is optional for standard placeholders when angle brackets
are used.
- Relaxed: hash prefix, match all characters except "/". E. g. "/#name/hello".
- Wildcard: asterisk prefix, match all characters. E. g. "/*name/hello".

You can use `req:stash('name')` to get matched string from the request handler.
There are also special stash names:
- `format` - the current output format (e.g. `html`, `txt`). It is
  detected automatically based on the request's `path` (for example, `/abc.js`
  sets `format` to `js`). When producing a response, `format` is used
  to serve the response's 'Content-type:'.

TODO: Define API for interaction between httpng and (user) router object.

### Handlers

`function handler(req, io)`

This is what HTTPNG is about - handling HTTP(S) requests. Handlers are Lua functions that run in separate fibers in the TX thread (and can access the Tarantool database if they want to).

- `req`: Table with the following entries:
  - `body`: String, HTTP(S) request body.
  - `headers`: Table containing HTTP(S) request headers with entries like `['user-agent'] = 'godzilla'`
  - `host`: String, virtual host name.
  - `https`: Boolean, is SSL/TLS used for underlying TCP connection
     (can be `nil` instead of `false` for performance reasons).
  - `is_websocket`: Boolean, is this WebSockets request.
  - `method`: String, 'GET', 'PUT' etc.
  - `ouraddr`: Table, contains an IP address and port connection has been received on in `socket.getaddrinfo()` format.
  - `path`: String, contains "path" of HTTP(S) request - that is, '/en/download?a=b' for 'https://www.tarantool.io/en/download?a=b'.
  - `peer`: Table, contains IP address and port of HTTP(S) client in `socket.getaddrinfo()` format.
  - `query`: String, everything after "?" in path or `nil`.
  - `stash`: Function `(req, name)` to get string matched by placeholder `name`
(see "Route specifications").
  - `version_major`: Number, contains "major" part of HTTP version ('2' for 'HTTP 2.0').
  - `version_minor`: Number, contains "minor" part of HTTP version ('0' for 'HTTP 2.0').
  - `_shuttle`: Userdata, please do not touch.

- `io`: Table with the following entries:
  - `close`: function(`io`), equivalent to `io:write(nil, True)` - finishes HTTP(S) request handling. You do not need that in most cases because return from handler always does that.
  - `headers`: Empty table where you can create entries containing HTTP(S) response headers like `['content-type'] = 'text/html'`. It is used if you do not specify `headers` when calling `write_header()` or `write()`.
  - `upgrade_to_websocket`: function, to be documented.
  - `write_header`: function(`io, code, headers, body, is_last`), sends HTTP(S) `code` (Integer), `headers` (optional Table with entries like `['content-type'] = 'text/plain; charset=utf-8'`; if it is not specified then `io.headers` is used) and `body` (optional String). `is_last` is optional Boolean, set to `True` if there would be no more sends for this request, defaults to `False`. Returns `True` if the connection has already been closed so there is no point in trying to send anything else. `io` is a reference to self - `io:write_header(code, headers, payload, is_last)`. `write_header()` can be called only once per HTTP(S) request. Note that libh2o may add some headers to handle chunked encoding etc.
  - `write`: function(`body, is_last`), sends `body` (String). `is_last` is Boolean, set to `True` if there would be no more sends for this request, defaults to `False`. Returns `True` if the connection has already been closed so there is no point in trying to send anything else. `io` is a reference to self - `io:write(payload, is_last)`. If there was no call to `write_header()` earlier, HTTP(S) code would be 200 and `io.headers` would be used. Note that libh2o may add some headers to handle chunked encoding etc.
  - `_shuttle`: Userdata, please do not touch.

`handler()` can optionally return a table with `status`, `headers` and `body`, the effect is the same as a call to `io:write_header(status, headers, body, True)` (if `io:write_header()` was not called; note that `io.headers` is *not* used) or `io:write(body, True)` (if `io:write_header()` was called earlier; `status` and `headers` are silently ignored).

### Listen

`httpng.cfg{ listen = <...> }`

The way to specify server listeners (IP:port pairs) in HTTPNG is to pass a value with a key called `listen` to the table which is the first argument to `httpng.cfg()` function. There are some rules on how to set `listen`.

`listen`: Table or integer or string, depending on a case. We have such a term as "listener-conf" which is an entity describing one listener (IP:port pair).
If `listen` is an array (consists only of elements with keys `[1, #listen]`), then
it is considered as a table of listener-confs, in other ways it is considered as one listener-conf (one listener). `listen = 3300`, by default. There're 3 types of listener-conf:
  + Table. It is the most descriptive way to define listener-conf. The table contains fields:
    - `addr`: String. IPv4/IPv6 address. If it is `nil` then 2 listeners with '0.0.0.0' and '::' addresses are created.
    - `port`: Integer. Must be in the interval [0, 65535]. If it is `nil`, then the default 3300 port is used.
    - `tls`: Array of tables. Each table contains fields `certificate_file` and `certificate_key_file` which are strings with paths of the certificate file and private key file for a certificate accordingly. If `tls` is `nil`, an insecure HTTP protocol is used.
    - `uses_sni`: Boolean. Specifies to enable TLS SNI or not. `false`, by default.

    Examples:
      - ```Lua
        -- https://foo.com:8080 and https://bar.com:8080
        -- https://0.0.0.0:8080 won't be accepted due to SNI.
        {
          addr = '0.0.0.0',
          port = 8080,
          tls = {
            { certificate_file = '/path/to/certificate/foo.com_cert.pem', certificate_key_file = '/path/to/key/foo.com_key.pem' },
            { certificate_file = '/path/to/certificate/bar.com_cert.pem', certificate_key_file = '/path/to/key/bar.com_key.pem' },
          },
          uses_sni = true
        }
        ```
      - ```Lua
        -- Any requests to [::] (https://[::]:3300).
        -- Note: a user's browser would refuse to accept such a page unless it attempts to access the website from this certificate (foo.com in this example)
        {
          addr = '::',
          tls = { { certificate_file = 'path/to/certificate/foo.com_cert.pem', certificate_key_file = '/path/to/key/foo.com_key.pem' } },
        }
    
  + Integer. Defines port. In this case, will be created listeners with mentioned port on default IP addresses to listen by an insecure HTTP protocol.
  + String. Describes URI. Note: HTTPS protocol can't be enabled in this way because `tls` can be defined only if listener-conf is a table.

    Examples:
      - ```Lua 
        listen = 'http://0.0.0.0:8080'
        ```
      - ```Lua
        -- listen on foo.com port 80
        listen = 'http://foo.com'
        ```
  Examples of `listen`:
  - ```Lua
    listen = { 8080, 9090 }
    ```
  - ```Lua
    listen = {
      {
        addr = '::',
        port = 8080,
      },
      9090,
      {
        addr = '0.0.0.0',
        port = 8080,
        tls = { 
          { certificate_file = '/path/to/certificate/foo.com_cert.pem', certificate_key_file = '/path/to/key/foo.com_key.pem' }
        },
      }
    }
    ```
