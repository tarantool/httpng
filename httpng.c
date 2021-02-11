#include <fcntl.h>
#include <float.h>

#include <lauxlib.h>
#include <module.h>

#include "../xtm/xtm_api.h"

#include <tarantool-httpng/httpng.h>

#ifdef USE_LIBUV
#include <uv.h>
#include <h2o/socket/uv-binding.h>
#endif /* USE_LIBUV */

#ifndef lengthof
#define lengthof(array) (sizeof(array) / sizeof((array)[0]))
#endif

#ifdef TCP_FASTOPEN
#define H2O_DEFAULT_LENGTH_TCP_FASTOPEN_QUEUE 4096
#else
#define H2O_DEFAULT_LENGTH_TCP_FASTOPEN_QUEUE 0
#endif /* TCP_FASTOPEN */

/* Failing HTTP requests is fine, but failing to respond from TX thread
 * is not so queue size must be larger */
#define QUEUE_TO_TX_ITEMS (1 << 12) /* Must be power of 2 */
#define QUEUE_FROM_TX_ITEMS (QUEUE_TO_TX_ITEMS << 1) /* Must be power of 2 */

#define USE_HTTPS 1
//#define USE_HTTPS 0

#define H2O_DEFAULT_PORT_FOR_PROTOCOL_USED 65535

#define LUA_QUERY_NONE UINT_MAX

/* FIXME: Make it dynamic to use whole shuttle_size. */
#define LUA_MAX_PATH_LEN 256

struct thread_ctx;
typedef struct listener_ctx {
	h2o_accept_ctx_t accept_ctx;
#ifdef USE_LIBUV
	uv_tcp_t uv_tcp_listener;
	uv_poll_t uv_poll_from_tx;
#else /* USE_LIBUV */
	h2o_socket_t *sock;
#endif /* USE_LIBUV */
	struct thread_ctx *thread_ctx;
	int fd;
} listener_ctx_t;

typedef struct {
	int fd;
} listener_cfg_t;

typedef struct {
	char *path;
	char *global_var_name;
} lua_site_t;

typedef struct {
	h2o_handler_t super;
	char *global_var_name;
	const char *path;
	size_t path_len;
} lua_h2o_handler_t;

typedef struct {
	const char *name;
	const char *value;
	unsigned name_len;
	unsigned value_len;
} http_header_entry_t;

typedef struct {
	h2o_generator_t generator;
	http_header_entry_t headers[16]; /* FIXME: Dynamic? */
	char *global_var_name; /* global var with Lua handler */
	const char *site_path;
	struct fiber *fiber;
	int lua_state_ref;
	unsigned num_headers;
	unsigned http_code;
	unsigned payload_len;
	unsigned path_len;
	unsigned query_at;
	const char *payload;
	bool fiber_done;
	bool is_last_send;
	bool sent_something;
	bool cancelled; /* Changed by TX thread. */
	bool is_waiting; /* Changed by TX thread. */
	char path[LUA_MAX_PATH_LEN]; /* From h2o_req_t. */
	char embedded_payload[];
} lua_response_t;

static struct {
	h2o_globalconf_t globalconf;
	listener_cfg_t *listener_cfgs;
	thread_ctx_t *thread_ctxs;
	struct fiber **tx_fiber_ptrs;
	SSL_CTX *ssl_ctx;
	unsigned shuttle_size;
	unsigned num_listeners;
	unsigned num_accepts;
        unsigned max_conn_per_thread;
	unsigned num_threads;
	int tfo_queues;
	volatile bool tx_fiber_should_work;
} conf = {
	.tfo_queues = H2O_DEFAULT_LENGTH_TCP_FASTOPEN_QUEUE,
};

__thread thread_ctx_t *curr_thread_ctx;

static inline shuttle_t *get_shuttle_from_generator_lua(h2o_generator_t *generator)
{
	lua_response_t *const response = container_of(generator, lua_response_t, generator);
	return (shuttle_t *)((char *)response - offsetof(shuttle_t, payload));
}

/* Called when dispatch must not fail */
void stubborn_dispatch_uni(struct xtm_queue *queue, void *func, void *param)
{
	while (xtm_fun_dispatch(queue, func, param, 0)) {
		/* Error; we must not fail so retry a little later */
		fiber_sleep(0);
	}
}

/* Launched in HTTP server thread */
static void cancel_processing_lua_req_in_http_thread(shuttle_t *shuttle)
{
	assert(shuttle->disposed);
	free_shuttle(shuttle);
}

/* Launched in TX thread.
 * It can queue request to HTTP thread or free everything itself. */
static void free_lua_shuttle_from_tx(shuttle_t *shuttle)
{
	/* Can't call free_shuttle() from TX thread because it [potentially] uses per-thread pools. */
	stubborn_dispatch(shuttle->thread_ctx->queue_from_tx, &cancel_processing_lua_req_in_http_thread, shuttle);
}

/* Launched in TX thread */
static void cancel_processing_lua_req_in_tx(shuttle_t *shuttle)
{
	lua_response_t *const response = (lua_response_t *)&shuttle->payload;

	/* We do not use fiber_cancel() because it causes exception
	 * in Lua code so Lua handler have to use pcall() and even
	 * that is not 100% guarantee because such exception
	 * can theoretically happen before pcall().
	 * Also we have unref Lua state. */
	if (response->fiber == NULL)
		free_lua_shuttle_from_tx(shuttle);
	else if (response->is_waiting) {
		assert(!response->fiber_done);
		response->cancelled = true;
		fiber_wakeup(response->fiber);
	} else if (response->fiber_done)
		free_lua_shuttle_from_tx(shuttle);
	else
		response->cancelled = true;
		; /* Fiber would clean up because we have set cancelled=true */
}

/* Launched in HTTP server thread */
static void free_shuttle_lua(shuttle_t *shuttle)
{
	shuttle->disposed = true;
	stubborn_dispatch(get_curr_thread_ctx()->queue_to_tx, &cancel_processing_lua_req_in_tx, shuttle);
}

/* Launched in TX thread */
static void continue_processing_lua_req_in_tx(shuttle_t *shuttle)
{
	lua_response_t *const response = (lua_response_t *)&shuttle->payload;

	assert(response->fiber != NULL);
	assert(!response->fiber_done);
	assert(response->is_waiting);
	fiber_wakeup(response->fiber);
}

/* Launched in HTTP server thread when H2O has sent everything and asks for more */
static void proceed_sending_lua(h2o_generator_t *self, h2o_req_t *req)
{
	shuttle_t *const shuttle = get_shuttle_from_generator_lua(self);
	thread_ctx_t *const thread_ctx = get_curr_thread_ctx();
	stubborn_dispatch(thread_ctx->queue_to_tx, &continue_processing_lua_req_in_tx, shuttle);
}

/* Launched in HTTP server thread to postprocess first response (with HTTP headers) */
static void postprocess_lua_req_first(shuttle_t *shuttle)
{
	lua_response_t *const response = (lua_response_t *)(&shuttle->payload);
	if (shuttle->disposed)
		return;
	h2o_req_t *const req = shuttle->never_access_this_req_from_tx_thread;
	req->res.status = response->http_code;
	req->res.reason = "OK"; /* FIXME? */
	const unsigned num_headers = response->num_headers;
	unsigned header_idx;
	for (header_idx = 0; header_idx < num_headers; ++header_idx) {
		const http_header_entry_t *const header = &response->headers[header_idx];
		h2o_add_header_by_str(&req->pool, &req->res.headers, header->name, header->name_len,
				1, /* FIXME: Benchmark whether this faster than 0 */
				NULL, /* FIXME: Do we need orig_name? */
				header->value, header->value_len);

	}

	response->generator = (h2o_generator_t){ proceed_sending_lua, NULL }; /* Do not use stop_sending, we handle everything in free_shuttle_lua(). */
	response->sent_something = true;
	h2o_start_response(req, &response->generator);

	h2o_iovec_t buf;
	buf.base = (void *)response->payload;
	buf.len = response->payload_len;
	h2o_send(req, &buf, 1, response->is_last_send ? H2O_SEND_STATE_FINAL : H2O_SEND_STATE_IN_PROGRESS);
}

/* Launched in HTTP server thread to postprocess response (w/o HTTP headers) */
static void postprocess_lua_req_others(shuttle_t *shuttle)
{
	lua_response_t *const response = (lua_response_t *)(&shuttle->payload);
	if (shuttle->disposed)
		return;
	h2o_req_t *const req = shuttle->never_access_this_req_from_tx_thread;

	h2o_iovec_t buf;
	buf.base = (void *)response->payload;
	buf.len = response->payload_len;
	h2o_send(req, &buf, 1, response->is_last_send ? H2O_SEND_STATE_FINAL : H2O_SEND_STATE_IN_PROGRESS);
}

static void add_http_header_to_shuttle(shuttle_t *shuttle, const char *key, size_t key_len, const char *value, size_t value_len)
{
	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	if (response->num_headers >= lengthof(response->headers))
		return;

	response->headers[response->num_headers++] = (http_header_entry_t){key, value, key_len, value_len};
}

/* Launched in TX thread. */
static inline void wait_for_lua_shuttle_return(lua_response_t *response)
{
	response->is_waiting = true;
	fiber_yield();
	response->is_waiting = false;
}

/* Launched in TX thread */
static int payload_writer_write(lua_State *L)
{
	/* Lua parameters: self, payload, is_last. */
	const unsigned num_params = lua_gettop(L);
	if (num_params < 2)
		goto Error;

	lua_getfield(L, 1, "shuttle");
	int is_integer;
	shuttle_t *const shuttle = (shuttle_t *)lua_tointegerx(L, -1, &is_integer);
	if (!is_integer)
		goto Error;

	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	if (response->cancelled) {
		/* Returning Lua true because connection has already been closed. */
		lua_pushboolean(L, true);
		return 1;
	}

	size_t payload_len;
	response->payload = lua_tolstring(L, 2, &payload_len);
	response->payload_len = payload_len;

	bool is_last;
	if (num_params >= 3)
		is_last	= lua_toboolean(L, 3);
	else
		is_last = false;

	response->is_last_send = is_last;
	stubborn_dispatch(shuttle->thread_ctx->queue_from_tx, &postprocess_lua_req_others, shuttle);
	wait_for_lua_shuttle_return(response);

	/* Returning Lua true if connection has already been closed. */
	lua_pushboolean(L, response->cancelled);
	return 1;

Error: /* FIXME: Error message */
	lua_pushboolean(L, true);
	return 1;
}

/* Launched in TX thread. */
static int payload_writer_write_nop(lua_State *L)
{
	/* Lua parameters: self, payload, is_last. */
	/* Returning Lua true because connection has already been closed. */
	lua_pushboolean(L, true);
	return 1;
}

/* Launched in TX thread */
static int header_writer_write_header(lua_State *L)
{
	/* Lua parameters: self, code, headers, payload, is_last. */
	const unsigned num_params = lua_gettop(L);
	if (num_params < 3)
		goto Error;

	lua_getfield(L, 1, "shuttle");

	int is_integer;
	shuttle_t *const shuttle = (shuttle_t *)lua_tointegerx(L, -1, &is_integer);
	if (!is_integer)
		goto Error;

	bool is_last;
	if (num_params >= 5)
		is_last	= lua_toboolean(L, 5);
	else
		is_last = false;

	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	if (response->cancelled) {
		/* Can't send anything, connection has been closed. */
		if (is_last)
			lua_pushnil(L);
		else {
			lua_createtable(L, 0, 2);
			lua_pushcfunction(L, payload_writer_write_nop);
			lua_setfield(L, -2, "write");
		}
		return 1;
	}

	response->http_code = lua_tointegerx(L, 2, &is_integer);
	if (!is_integer)
		goto Error;

	lua_pushnil(L); /* Start of table. */
	while (lua_next(L, 3)) {
		lua_pushnil(L); /* Start of table. */
		while (lua_next(L, -2)) {
			size_t key_len;
			size_t value_len;
			const char *const key = lua_tolstring(L, -2, &key_len);
			const char *const value = lua_tolstring(L, -1, &value_len);

			add_http_header_to_shuttle(shuttle, key, key_len, value, value_len);

			/* Remove value, keep key for next iteration. */
			lua_pop(L, 1);
		}
		/* Remove value, keep key for next iteration. */
		lua_pop(L, 1);
	}

	if (num_params >= 4) {
		size_t payload_len;
		response->payload = lua_tolstring(L, 4, &payload_len);
		response->payload_len = payload_len;
	}

	response->is_last_send = is_last;
	stubborn_dispatch(shuttle->thread_ctx->queue_from_tx, &postprocess_lua_req_first, shuttle);
	wait_for_lua_shuttle_return(response);

	if (is_last)
		lua_pushnil(L);
	else if (response->cancelled) {
		lua_createtable(L, 0, 1);
		lua_pushcfunction(L, payload_writer_write_nop);
		lua_setfield(L, -2, "write");
	} else {
		lua_createtable(L, 0, 2);
		lua_pushcfunction(L, payload_writer_write);
		lua_setfield(L, -2, "write");
		lua_pushinteger(L, (uintptr_t)shuttle);
		lua_setfield(L, -2, "shuttle");
	}
	return 1;

Error: /* FIXME: Error message? */
	lua_pushnil(L);
	return 1;
}

/* Launched in TX thread */
static int
lua_fiber_func(va_list ap)
{
	shuttle_t *const shuttle = va_arg(ap, shuttle_t *);
	lua_State *const L = va_arg(ap, lua_State *);

	lua_response_t *const response = (lua_response_t *)&shuttle->payload;

	lua_getglobal(L, response->global_var_name); /* User handler function, written in Lua */

	/* First param for Lua handler - query */
	lua_createtable(L, 0, 1);
	if (response->query_at != LUA_QUERY_NONE) {
		const char *const query = &response->path[response->query_at];
		const unsigned query_len = response->path_len - response->query_at;
		lua_pushlstring (L, query, query_len);
		lua_setfield(L, -2, "query");
	}

	/* Second param for Lua handler - header_writer */
	lua_createtable(L, 0, 2);
	lua_pushcfunction(L, header_writer_write_header);
	lua_setfield(L, -2, "write_header");

	lua_pushinteger(L, (uintptr_t)shuttle);
	lua_setfield(L, -2, "shuttle");

	const int lua_state_ref = response->lua_state_ref;
	if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
		/* FIXME: Should probably log this instead(?) */
		fprintf(stderr, "User handler for \"\%s\" failed with error \"%s\"\n", response->site_path, lua_tostring(L, -1));

		if (response->cancelled) {
			/* No point trying to send something, connection has already been closed. */
			/* There would be no more calls from HTTP thread, must clean up. */
			free_lua_shuttle_from_tx(shuttle);
		} else {
			response->is_last_send = true;
			if (response->sent_something) {
				/* Do not add anything to user output to prevent corrupt HTML etc. */
				response->payload_len = 0;
				stubborn_dispatch(shuttle->thread_ctx->queue_from_tx, &postprocess_lua_req_others, shuttle);
			} else {
				static const char key[] = "content-type";
				static const char value[] = "text/plain; charset=utf-8";
				add_http_header_to_shuttle(shuttle, key, sizeof(key) - 1, value, sizeof(value) - 1);
				static const char error_str[] = "Path handler execution error";
				response->http_code = 500;
				response->payload = error_str;
				response->payload_len = sizeof(error_str) - 1;
				stubborn_dispatch(shuttle->thread_ctx->queue_from_tx, &postprocess_lua_req_first, shuttle);
			}
			wait_for_lua_shuttle_return(response);
			if (response->cancelled)
				/* There would be no more calls from HTTP thread, must clean up. */
				free_lua_shuttle_from_tx(shuttle);
			else
				response->fiber_done = true;
				/* cancel_processing_lua_req_in_tx() is not yet called, it would clean up because we have set fiber_done=true. */
		}
	} else if (response->cancelled)
		/* There would be no more calls from HTTP thread, must clean up */
		free_lua_shuttle_from_tx(shuttle);
	else
		response->fiber_done = true;
		/* cancel_processing_lua_req_in_tx() is not yet called, it would clean up because we have set fiber_done=true */

	luaL_unref(luaT_state(), LUA_REGISTRYINDEX, lua_state_ref);
	return 0;
}

/* Launched in TX thread */
static void process_lua_req_in_tx(shuttle_t *shuttle)
{
	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	response->num_headers = 0;

#define RETURN_WITH_ERROR(err) \
	do { \
		luaL_unref(L, LUA_REGISTRYINDEX, response->lua_state_ref); \
		static const char key[] = "content-type"; \
		static const char value[] = "text/plain; charset=utf-8"; \
		add_http_header_to_shuttle(shuttle, key, sizeof(key) - 1, value, sizeof(value) - 1); \
		static const char error_str[] = err; \
		response->is_last_send = true; \
		response->http_code = 500; \
		response->payload = error_str; \
		response->payload_len = sizeof(error_str) - 1; \
		stubborn_dispatch(shuttle->thread_ctx->queue_from_tx, &postprocess_lua_req_first, shuttle); \
		return; \
	} while (0)

	struct lua_State *const L = luaT_state();
	struct lua_State *const new_L = lua_newthread(L);
	response->lua_state_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	if ((response->fiber = fiber_new("HTTP Lua fiber", &lua_fiber_func)) == NULL)
		RETURN_WITH_ERROR("Failed to create fiber");
	response->payload_len = 0;
	response->fiber_done = false;
	fiber_start(response->fiber, shuttle, new_L);
}
#undef RETURN_WITH_ERROR

/* Launched in HTTP server thread */
static int lua_req_handler(lua_h2o_handler_t *self, h2o_req_t *req)
{
	if (!(h2o_memis(req->method.base, req->method.len, H2O_STRLIT("GET")) &&
	    h2o_memis(req->path_normalized.base, req->path_normalized.len, self->path, self->path_len))) {
		/* FIXME: Should handle method/path in Lua handler */
		return -1;
	}

	shuttle_t *const shuttle = prepare_shuttle(req);
	/* Can fill in shuttle->payload here */

	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	if ((response->path_len = req->path.len) > sizeof(response->path)) {
		/* Error */
		free_shuttle_with_anchor(shuttle);
		req->res.status = 500;
		req->res.reason = "Request is too long";
		h2o_send_inline(req, H2O_STRLIT("Request is too long\n"));
		return 0;
	}
	memcpy(response->path, req->path.base, response->path_len);

	static_assert(LUA_QUERY_NONE < (1ULL << (8 * sizeof(response->query_at))));
	response->query_at = (req->query_at == SIZE_MAX) ? LUA_QUERY_NONE : req->query_at;

	response->is_last_send = false;
	response->sent_something = false;
	response->cancelled = false;
	response->is_waiting = false;
	response->global_var_name = self->global_var_name;
	response->site_path = self->path;

	thread_ctx_t *const thread_ctx = get_curr_thread_ctx();
	if (xtm_fun_dispatch(thread_ctx->queue_to_tx, (void(*)(void *))&process_lua_req_in_tx, shuttle, 0)) {
		/* Error */
		free_shuttle_with_anchor(shuttle);
		req->res.status = 500;
		req->res.reason = "Queue overflow";
		h2o_send_inline(req, H2O_STRLIT("Queue overflow\n"));
		return 0;
	}
	shuttle->anchor->user_free_shuttle = &free_shuttle_lua;

	return 0;
}

static h2o_pathconf_t *register_handler(h2o_hostconf_t *hostconf, const char *path, int (*on_req)(h2o_handler_t *, h2o_req_t *))
{
	/* These functions never return NULL, dying instead */
	h2o_pathconf_t *pathconf = h2o_config_register_path(hostconf, path, 0);
	h2o_handler_t *handler = h2o_create_handler(pathconf, sizeof(*handler));
	handler->on_req = on_req;
	return pathconf;
}

/* N. b.: *path and *global_var_name must live until server is shutdown */
static h2o_pathconf_t *register_lua_handler(h2o_hostconf_t *hostconf, const char *path, size_t path_len, char *global_var_name)
{
	/* These functions never return NULL, dying instead */
	h2o_pathconf_t *pathconf = h2o_config_register_path(hostconf, path, 0);
	lua_h2o_handler_t *handler = (lua_h2o_handler_t *)h2o_create_handler(pathconf, sizeof(*handler));
	handler->super.on_req = (int (*)(h2o_handler_t *, h2o_req_t *))lua_req_handler;
	handler->global_var_name = global_var_name;
	handler->path = path;
	handler->path_len = path_len;
	return pathconf;
}

static inline shuttle_t *alloc_shuttle(thread_ctx_t *thread_ctx)
{
	/* FIXME: Use per-thread pools */
	(void)thread_ctx;
	shuttle_t *const shuttle = (shuttle_t *)malloc(conf.shuttle_size);
	if (shuttle == NULL)
		h2o_fatal("no memory");
	return shuttle;
}

void free_shuttle(shuttle_t *shuttle)
{
	free(shuttle);
}

void free_shuttle_with_anchor(shuttle_t *shuttle)
{
	assert(!shuttle->disposed);
	shuttle->anchor->shuttle = NULL;
	free_shuttle(shuttle);
}

static void anchor_dispose(void *param)
{
	anchor_t *const anchor = param;
	shuttle_t *const shuttle = anchor->shuttle;
	if (shuttle != NULL) {
		if (anchor->user_free_shuttle != NULL)
			anchor->user_free_shuttle(shuttle);
		else
			shuttle->disposed = true;
	}

	/* probably should implemented support for "stubborn" anchors - 
	optionally wait for tx processing to finish so TX thread can access h2o_req_t directly
	thus avoiding copying LARGE buffers, it only makes sense
	in very specific cases because it stalls the whole thread if such
	request is gone */
}

shuttle_t *prepare_shuttle(h2o_req_t *req)
{
	anchor_t *const anchor = h2o_mem_alloc_shared(&req->pool, sizeof(anchor_t), &anchor_dispose);
	anchor->user_free_shuttle = NULL;
	thread_ctx_t *const thread_ctx = get_curr_thread_ctx();
	shuttle_t *const shuttle = alloc_shuttle(thread_ctx);
	shuttle->anchor = anchor;
	anchor->shuttle = shuttle;
	shuttle->never_access_this_req_from_tx_thread = req;
	shuttle->thread_ctx = thread_ctx;
	shuttle->disposed = false;
	shuttle->stopped = false;
	return shuttle;
}

#ifdef USE_LIBUV

static void on_uv_socket_free(void *data)
{
	thread_ctx_t *const thread_ctx = get_curr_thread_ctx();
	--thread_ctx->num_connections;
	free(data);
}

#else /* USE_LIBUV */

static void on_socketclose(void *data)
{
	(void)data;
	thread_ctx_t *const thread_ctx = get_curr_thread_ctx();
	--thread_ctx->num_connections;
}

#endif /* USE_LIBUV */

#ifdef USE_LIBUV

static void on_call_from_tx(uv_poll_t *handle, int status, int events)
{
	(void)handle;
	(void)events;
	if (status != 0)
		return;
	xtm_fun_invoke_all(get_curr_thread_ctx()->queue_from_tx);
}

#else /* USE_LIBUV */

static void on_call_from_tx(h2o_socket_t *listener, const char *err)
{
	if (err != NULL)
		return;

	xtm_fun_invoke_all(get_curr_thread_ctx()->queue_from_tx);
}

#endif /* USE_LIBUV */

#ifdef USE_LIBUV

static void on_accept(uv_stream_t *uv_listener, int status)
{
	if (status != 0)
		return;

	/* FIXME: Pools instead of malloc? */
	uv_tcp_t *const conn = h2o_mem_alloc(sizeof(*conn));
	if (uv_tcp_init(uv_listener->loop, conn)) {
		free(conn);
		return;
	}

	if (uv_accept(uv_listener, (uv_stream_t *)conn)) {
		uv_close((uv_handle_t *)conn, (uv_close_cb)free);
		return;
	}

	listener_ctx_t *const listener_ctx = (listener_ctx_t *)uv_listener->data;
	h2o_accept(&listener_ctx->accept_ctx, h2o_uv_socket_create((uv_stream_t *)conn, (uv_close_cb)on_uv_socket_free));
}

#else /* USE_LIBUV */

static void on_accept(h2o_socket_t *listener, const char *err)
{
	if (err != NULL)
		return;

	listener_ctx_t *const listener_ctx = (listener_ctx_t *)listener->data;
	thread_ctx_t *const thread_ctx = listener_ctx->thread_ctx;
	unsigned remain = conf.num_accepts;

	do {
		if (thread_ctx->num_connections >= conf.max_conn_per_thread)
			break;
		h2o_socket_t *sock = h2o_evloop_socket_accept(listener);
		if (sock == NULL)
			return;

		++thread_ctx->num_connections;

		sock->on_close.cb = on_socketclose;

		h2o_accept(&listener_ctx->accept_ctx, sock);
	} while (--remain);
}

#endif /* USE_LIBUV */

static inline void set_cloexec(int fd)
{
	/* For performance reasons do not check result in production builds
	 * (should not fail anyway).
	 * TODO: Remove this call completely? Do we plan to create
	 * child processes ? */
	int result = fcntl(fd, F_SETFD, FD_CLOEXEC);
	assert(result != -1);
}

/** Returns file descriptor or -1 on error */
static int open_listener_ipv4(const char *addr_str, uint16_t port)
{
	struct sockaddr_in addr;
	int fd;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	if (!inet_aton(addr_str, &addr.sin_addr)) {
		return -1;
	}
	addr.sin_port = htons(port);

	/* FIXME: Do all OSes we care about support SOCK_CLOEXEC? */
	if ((fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) == -1) {
		return -1;
	}

	int reuseaddr_flag = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr_flag, sizeof(reuseaddr_flag)) != 0 ||
            bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    listen(fd, SOMAXCONN) != 0) {
		close(fd);
		/* TODO: Log error */
		return -1;
	}

#ifdef TCP_DEFER_ACCEPT
	{
		/* We are only interested in connections when actual data is received */
		int flag = 1;
		if (setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &flag, sizeof(flag)) != 0) {
			close(fd);
			/* TODO: Log error */
			return -1;
		}
	}
#endif /* TCP_DEFER_ACCEPT */

	if (conf.tfo_queues > 0) {
		/* TCP extension to do not wait for SYN/ACK for "known" clients */
#ifdef TCP_FASTOPEN
		int tfo_queues;
#ifdef __APPLE__
		/* In OS X, the option value for TCP_FASTOPEN must be 1 if is's enabled */
		tfo_queues = 1;
#else
		tfo_queues = conf.tfo_queues;
#endif /* __APPLE__ */
		if (setsockopt(fd, IPPROTO_TCP, TCP_FASTOPEN, (const void *)&tfo_queues, sizeof(tfo_queues)) != 0) {
			/* TODO: Log warning */
		}
#else
		assert(!"conf.tfo_queues not zero on platform without TCP_FASTOPEN");
#endif /* TCP_FASTOPEN */
	}

	return fd;
}

static SSL_CTX *setup_ssl(const char *cert_file, const char *key_file)
{
	if (!SSL_load_error_strings())
		return NULL;
	SSL_library_init(); /* Always succeeds */
	OpenSSL_add_all_algorithms();

	SSL_CTX *ssl_ctx = SSL_CTX_new(SSLv23_server_method());
	if (ssl_ctx == NULL)
		return NULL;
	SSL_CTX_set_options(ssl_ctx, SSL_OP_NO_SSLv2);//x x x: //make configurable;

	if (SSL_CTX_use_certificate_file(ssl_ctx, cert_file, SSL_FILETYPE_PEM) != 1)
		return NULL;
	if (SSL_CTX_use_PrivateKey_file(ssl_ctx, key_file, SSL_FILETYPE_PEM) != 1)
		return NULL;

/* setup protocol negotiation methods */
#if H2O_USE_NPN
	h2o_ssl_register_npn_protocols(ssl_ctx, h2o_http2_npn_protocols);
#endif
#if H2O_USE_ALPN
	h2o_ssl_register_alpn_protocols(ssl_ctx, h2o_http2_alpn_protocols);
#endif

	return ssl_ctx;
}

static void *worker_func(void *param)
{
	const unsigned thread_idx = (unsigned)(uintptr_t)param;
	thread_ctx_t *const thread_ctx = &conf.thread_ctxs[thread_idx];
	curr_thread_ctx = thread_ctx;
	thread_ctx->idx = thread_idx;
	thread_ctx->tid = pthread_self();
	if ((thread_ctx->queue_from_tx = xtm_create(QUEUE_FROM_TX_ITEMS)) == NULL) {
		//TODO: Report
		return NULL;
	}

	if ((thread_ctx->listener_ctxs = (listener_ctx_t *)malloc(conf.num_listeners * sizeof(listener_ctx_t))) == NULL) {
		//TODO: Report
		xtm_delete(thread_ctx->queue_from_tx);
		return NULL;
	}

	memset(&thread_ctx->ctx, 0, sizeof(thread_ctx->ctx));
#ifdef USE_LIBUV
	uv_loop_init(&thread_ctx->loop);
	h2o_context_init(&thread_ctx->ctx, &thread_ctx->loop, &conf.globalconf);
#else /* USE_LIBUV */
	h2o_context_init(&thread_ctx->ctx, h2o_evloop_create(), &conf.globalconf);
#endif /* USE_LIBUV */

	listener_ctx_t *listener_ctx = &thread_ctx->listener_ctxs[0];//x x x: More than one
	listener_cfg_t *listener_cfg = &conf.listener_cfgs[0];//x x x: More than one
	memset(listener_ctx, 0, sizeof(*listener_ctx));
	listener_ctx->thread_ctx = thread_ctx;
	listener_ctx->accept_ctx.ssl_ctx = conf.ssl_ctx;
	listener_ctx->accept_ctx.ctx = &thread_ctx->ctx;
	listener_ctx->accept_ctx.hosts = conf.globalconf.hosts;
	if (thread_idx) {
		if ((listener_ctx->fd = dup(listener_cfg->fd)) == -1) {
			//TODO: Report
			free(thread_ctx->listener_ctxs);
			xtm_delete(thread_ctx->queue_from_tx);
			return NULL;
		}
		set_cloexec(listener_ctx->fd);
	} else {
		listener_ctx->fd = listener_cfg->fd;
	}

#ifdef USE_LIBUV
	if (uv_tcp_init(thread_ctx->ctx.loop, &listener_ctx->uv_tcp_listener))
		goto Error;
	if (uv_tcp_open(&listener_ctx->uv_tcp_listener, listener_ctx->fd))
		goto Error;
	listener_ctx->uv_tcp_listener.data = listener_ctx;
	if (uv_listen((uv_stream_t *)&listener_ctx->uv_tcp_listener, SOMAXCONN, on_accept))
		goto Error;

	if (uv_poll_init(thread_ctx->ctx.loop, &listener_ctx->uv_poll_from_tx, xtm_fd(thread_ctx->queue_from_tx)))
		goto Error;
	if (uv_poll_start(&listener_ctx->uv_poll_from_tx, UV_READABLE, on_call_from_tx))
		goto Error;
#else /* USE_LIBUV */
	listener_ctx->sock = h2o_evloop_socket_create(thread_ctx->ctx.loop, listener_ctx->fd, H2O_SOCKET_FLAG_DONT_READ);
	listener_ctx->sock->data = listener_ctx;

	thread_ctx->sock_from_tx = h2o_evloop_socket_create(thread_ctx->ctx.loop, xtm_fd(thread_ctx->queue_from_tx), H2O_SOCKET_FLAG_DONT_READ);

	h2o_socket_read_start(thread_ctx->sock_from_tx, on_call_from_tx);
	h2o_socket_read_start(listener_ctx->sock, on_accept);
#endif /* USE_LIBUV */

	__sync_synchronize(); /* For the fiber in TX thread to see everything we have initialized */

	//x x x;//SIGTERM should terminate loop
#ifdef USE_LIBUV
	uv_run(&thread_ctx->loop, UV_RUN_DEFAULT);
Error:
	; /* FIXME: Free resources etc */
#else /* USE_LIBUV */
	h2o_evloop_t *loop = thread_ctx->ctx.loop;
	while (h2o_evloop_run(loop, INT32_MAX) == 0)
		;
#endif /* USE_LIBUV */

	//void h2o_socket_read_stop(h2o_socket_t *sock);//x x x;
	//x x x;//should flush these queues first
	xtm_delete(thread_ctx->queue_from_tx);
	free(thread_ctx->listener_ctxs);
	return NULL;
}

static int
tx_fiber_func(va_list ap)
{
	const unsigned fiber_idx = va_arg(ap, unsigned);
	/* This fiber processes requests from particular thread */
	thread_ctx_t *const thread_ctx = &conf.thread_ctxs[fiber_idx];
	struct xtm_queue *const queue_to_tx = thread_ctx->queue_to_tx;
	const int pipe_fd = xtm_fd(queue_to_tx);
	/* conf.tx_fiber_should_work is read non-atomically for performance
	 * reasons so it should be changed in this thread by queueing
	 * corresponding function call */
	while (conf.tx_fiber_should_work) {
		if (coio_wait(pipe_fd, COIO_READ, DBL_MAX) & COIO_READ) {
			xtm_fun_invoke_all(queue_to_tx);
		}
	}
	return 0;
}

/* Lua parameters: lua_sites, function_to_call, function_param */
int init(lua_State *L)
{
	enum {
		LUA_STACK_IDX_LUA_SITES = 1,
		LUA_STACK_IDX_FUNCTION_TO_CALL = 2,
		LUA_STACK_IDX_FUNCTION_PARAM = 3,
	};
	memset(&conf.globalconf, 0, sizeof(conf.globalconf));

	if (lua_gettop(L) < 3)
		goto Error;

	if (lua_type(L, LUA_STACK_IDX_FUNCTION_TO_CALL) != LUA_TFUNCTION)
		goto Error;

	if (lua_pcall(L, 1, 1, 0) != LUA_OK)
		goto Error;

	int is_integer;
	const site_desc_t *const site_desc = (site_desc_t *)lua_tointegerx(L, -1, &is_integer);
	if (!is_integer)
		goto Error;
	const path_desc_t *const path_descs = site_desc->path_descs;
	conf.tx_fiber_should_work = 1;
	/* FIXME: Add sanity checks, especially shuttle_size - it must >sizeof(shuttle_t) (accounting for Lua payload) and aligned */
	conf.num_threads = site_desc->num_threads;
	conf.shuttle_size = site_desc->shuttle_size;
	conf.max_conn_per_thread = site_desc->max_conn_per_thread;
	conf.num_accepts = conf.max_conn_per_thread / 16;
	if (conf.num_accepts < 8)
		conf.num_accepts = 8;

	if ((conf.thread_ctxs = (thread_ctx_t *)malloc(conf.num_threads * sizeof(thread_ctx_t))) == NULL)
		goto Error;

	h2o_config_init(&conf.globalconf);
	h2o_hostconf_t *hostconf = h2o_config_register_host(&conf.globalconf, h2o_iovec_init(H2O_STRLIT("default")), H2O_DEFAULT_PORT_FOR_PROTOCOL_USED); //x x x: customizable
	if (hostconf == NULL)
		goto Error;

	{
		const path_desc_t *path_desc = path_descs;
		if (path_desc->path == NULL)
			goto Error; /* Need at least one */

		do {
			register_handler(hostconf, path_desc->path, path_desc->handler);
		} while ((++path_desc)->path != NULL);
	}
	lua_site_t *lua_sites = NULL; /* FIXME: Free allocated memory - including malloc'ed path and global_var_name - when shutting down */
	unsigned lua_site_idx = 0;
	lua_pushnil(L); /* Start of table. */
	while (lua_next(L, LUA_STACK_IDX_LUA_SITES)) {
		lua_getfield(L, -1, "path");
		size_t path_len;
		const char *const path = lua_tolstring(L, -1, &path_len);
		if (path == NULL)
			goto Error;

		lua_site_t *const new_lua_sites = realloc(lua_sites, sizeof(lua_site_t) * (lua_site_idx + 1));
		if (new_lua_sites == NULL)
			goto Error;
		lua_sites = new_lua_sites;
		lua_site_t *const lua_site = &lua_sites[lua_site_idx];
		if ((lua_site->path = malloc(path_len + 1)) == NULL)
			goto Error;
		memcpy(lua_site->path, path, path_len);
		lua_site->path[path_len] = 0;

		lua_site->global_var_name = malloc(32); /* FIXME */
		snprintf(lua_site->global_var_name, 32, "httpng_handler_%u", lua_site_idx);

		lua_getfield(L, -2, "handler");
		if (lua_type(L, -1) != LUA_TFUNCTION)
			goto Error;
		lua_setglobal(L, lua_site->global_var_name); /* To access by C fiber */

		register_lua_handler(hostconf, lua_site->path, path_len, lua_site->global_var_name);

		++lua_site_idx;

		/* Remove path string and site array value, keep key for next iteration. */
		lua_pop(L, 2);
	}

	SSL_CTX *ssl_ctx;
	if (USE_HTTPS && (ssl_ctx = setup_ssl("cert.pem", "key.pem")) == NULL) {//x x x: customizable file names
		fprintf(stderr, "setup_ssl() failed (cert/key files not found?)\n");
		goto Error;
	}

#if 0
	/* Never returns NULL */
	h2o_logger_t *logger = h2o_access_log_register(&config.default_host, "/dev/stdout", NULL); //x x x: customizable
#endif

	conf.ssl_ctx = ssl_ctx;

	/* TODO: Implement more than one listener (HTTP/HTTPS, IPv4/IPv6, several IPs etc) */
	conf.num_listeners = 1; //x x x: customizable
	if ((conf.listener_cfgs = (listener_cfg_t *)malloc(conf.num_listeners * sizeof(listener_cfg_t))) == NULL)
		goto Error;

	{
		unsigned listener_idx;

		for (listener_idx = 0; listener_idx < conf.num_listeners; ++listener_idx)
			if ((conf.listener_cfgs[listener_idx].fd = open_listener_ipv4("127.0.0.1", 7890)) == -1) //x x x //customizable
				goto Error;
	}

	if ((conf.tx_fiber_ptrs = (struct fiber **)malloc(sizeof(struct fiber *) * conf.num_threads)) == NULL)
		goto Error;

	{
		unsigned i;
		for (i = 0; i < conf.num_threads; ++i) {
			if ((conf.thread_ctxs[i].queue_to_tx = xtm_create(QUEUE_TO_TX_ITEMS)) == NULL)
				goto Error;
			conf.thread_ctxs[i].num_connections = 0;

			char name[32];
			sprintf(name, "tx_h2o_fiber_%u", i);
			if ((conf.tx_fiber_ptrs[i] = fiber_new(name, tx_fiber_func)) == NULL)
				goto Error;
			fiber_set_joinable(conf.tx_fiber_ptrs[i], true);
			fiber_start(conf.tx_fiber_ptrs[i], i);
		}
	}

	{
		const path_desc_t *path_desc = path_descs;
		do {
			if (path_desc->init_userdata_in_tx != NULL && path_desc->init_userdata_in_tx(path_desc->init_userdata_in_tx_param))
				goto Error;
		} while ((++path_desc)->path != NULL);
	}

	/* Start processing HTTP requests and requests from TX thread */
	{
		unsigned i;
		for (i = 0; i < conf.num_threads; ++i) {
			pthread_t tid;
			if (pthread_create(&tid, NULL, worker_func, (void *)(uintptr_t)i))
				goto Error;
		}
	}

	return 0;

Error:
	//FIXME: Release resources, report errors details
	return 0;//x x x;
}

int deinit(lua_State *L)
{
	(void)L;
	//x x x; //terminate workers
	free(conf.listener_cfgs);
	free(conf.thread_ctxs);
	return 0;
}

static const struct luaL_Reg mylib[] = {
	{"init", init},
	{NULL, NULL}
};

int luaopen_httpng(lua_State *L)
{
	luaL_newlib(L, mylib);
	return 1;
}
