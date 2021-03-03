# HTTPNG module for Tarantool

https://tarantool.io/en/

Based on libh2o from H2O HTTP Server (https://h2o.examp1e.net/)

Implementation is not yet finished - there is no graceful shutdown
or reload, error checking is minimal, there is no logging.

At the moment it listens on 127.0.0.1 at configured port
(default is 8080). TLS only, key/certificate filenames are hardcoded.

API is not yet finalized (especially WebSockets), you can use examples
as a documentation for now.

Note that H2O implementation of large POST/PUT requests handling is
inefficient and prone to DoS, you can use max_body_len parameter
(defaults to 1 MiB) to limit the impact.

Thank you for your interest in Tarantool!
