# HTTPNG module for Tarantool

https://tarantool.io/en/

Based on libh2o from H2O HTTP Server (https://h2o.examp1e.net/)

TLS/HTTP decryption/parsing/sends/encryption are performed in a separate
thread (you can configure httpng to use more than one such thread),
HTTP(S) request handlers are written in Lua and are launched in separate
fibers in the TX thread.

Implementation is not yet finished - there is no logging, WebSockets
support is limited and prone to DoS.

API is documented in doc/api.md. Please note that it is not yet finalized
(especially WebSockets).

Note that H2O implementation of large POST/PUT requests handling is
inefficient and prone to DoS, you can use `max_body_len` parameter
(defaults to 1 MiB) to limit the impact.

Thank you for your interest in Tarantool!
