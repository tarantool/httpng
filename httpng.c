#include <fcntl.h>
#include <float.h>

#include <lauxlib.h>
#include <module.h>

#include "../xtm/xtm_api.h"

#include <tarantool-httpng/httpng.h>
#include <h2o/websocket.h>

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

/* We would need this when (if) alloc would be performed from thread pools w/o mutexes. */
#define SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD
#undef SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD

/* We would need this when (if) alloc would be performed from thread pools w/o mutexes. */
#define SHOULD_FREE_RECV_DATA_IN_HTTP_SERVER_THREAD
#undef SHOULD_FREE_RECV_DATA_IN_HTTP_SERVER_THREAD

#define USE_HTTPS 1
//#define USE_HTTPS 0

/* For debugging. FIXME: Make runtime configurable from Lua. */
#define DISABLE_HTTP2
#undef DISABLE_HTTP2

#define H2O_DEFAULT_PORT_FOR_PROTOCOL_USED 65535

#define LUA_QUERY_NONE UINT_MAX

#define WS_CLIENT_KEY_LEN 24 /* Hardcoded in H2O. */

#define DEFAULT_threads 1
#define DEFAULT_max_conn_per_thread 65536
#define DEFAULT_shuttle_size 65536

/* Limits are quite relaxed for now. */
#define MIN_threads 1
#define MIN_max_conn_per_thread 1
#define MIN_shuttle_size (sizeof(shuttle_t) + sizeof(uintptr_t))

/* Limits are quite relaxed for now. */
#define MAX_threads 1024
#define MAX_max_conn_per_thread (1024 * 1024)
#define MAX_shuttle_size (16 * 1024 * 1024)

#define DEFAULT_LISTEN_PORT 7890

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

typedef struct waiter {
	struct waiter *next;
	struct fiber *fiber;
} waiter_t;

typedef struct {
	char *path;
} lua_site_t;

typedef struct {
	h2o_handler_t super;
	int lua_handler_ref;
	const char *path;
	size_t path_len;
} lua_h2o_handler_t;

typedef struct {
	shuttle_t *parent_shuttle;
	unsigned payload_bytes;
	char payload[];
} recv_data_t;

typedef struct {
	const char *name;
	const char *value;
	unsigned name_len;
	unsigned value_len;
} http_header_entry_t;

typedef struct {
	unsigned num_headers;
	unsigned http_code;
	http_header_entry_t headers[];
} lua_first_response_only_t;

typedef struct {
	h2o_generator_t generator;
	unsigned payload_len;
	const char *payload;
	bool is_last_send;
} lua_any_response_t;

typedef struct {
	lua_any_response_t any;
	lua_first_response_only_t first; /* Must be last member of struct. */
} lua_response_struct_t;

typedef unsigned header_offset_t; /* FIXME: Make it ushort and add sanity checks. */
typedef struct {
	header_offset_t name_size;
	header_offset_t value_size;
} received_http_header_handle_t;

typedef struct {
	int lua_handler_ref; /* Reference to user Lua handler. */
	unsigned path_len;
	unsigned query_at;
	unsigned num_headers;
	unsigned char method_len;
	unsigned char ws_client_key_len;
	unsigned char version_major;
	unsigned char version_minor;
	char method[7];
	char buffer[]; /* "path" from h2o_req_t goes first. */
} lua_first_request_only_t;

typedef struct {
	const char *site_path;
	struct fiber *fiber;
	struct fiber *recv_fiber; /* Fiber for WebSocket recv handler. */
	h2o_websocket_conn_t *ws_conn;
	recv_data_t *recv_data; /* For WebSocket recv. */
	struct fiber *tx_fiber; /* The one which services requests from "our" HTTP thread. */
	waiter_t *waiter; /* Changed by TX thread. */
	int lua_state_ref;
	int lua_recv_handler_ref;
	int lua_recv_state_ref;
	bool fiber_done;
	bool sent_something;
	bool cancelled; /* Changed by TX thread. */
	bool ws_send_failed;
	bool upgraded_to_websocket;
	bool is_recv_fiber_waiting;
	bool is_recv_fiber_cancelled;
	bool in_recv_handler;
	char ws_client_key[WS_CLIENT_KEY_LEN];
	union { /* Can use struct instead when debugging. */
		lua_first_request_only_t req;
		lua_response_struct_t resp;
	} un; /* Must be last member of struct. */
} lua_response_t;

static struct {
	h2o_globalconf_t globalconf;
	listener_cfg_t *listener_cfgs;
	thread_ctx_t *thread_ctxs;
	struct fiber **tx_fiber_ptrs;
	SSL_CTX *ssl_ctx;
	unsigned shuttle_size;
	unsigned recv_data_size;
	unsigned num_listeners;
	unsigned num_accepts;
	unsigned max_conn_per_thread;
	unsigned num_threads;
	unsigned max_headers_lua;
	unsigned max_path_len_lua;
	unsigned max_recv_bytes_lua_websocket;
	int tfo_queues;
	volatile bool tx_fiber_should_work;
} conf = {
	.tfo_queues = H2O_DEFAULT_LENGTH_TCP_FASTOPEN_QUEUE,
};

__thread thread_ctx_t *curr_thread_ctx;

static void fill_http_headers(lua_State *L, lua_response_t *response, int param_lua_idx);
static int payload_writer_write_nop(lua_State *L);

static inline shuttle_t *get_shuttle_from_generator_lua(h2o_generator_t *generator)
{
	lua_response_t *const response = container_of(generator, lua_response_t, un.resp.any.generator);
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

/* Called when dispatch must not fail. */
static inline void stubborn_dispatch_lua(struct xtm_queue *queue, void (*func)(lua_response_t *), lua_response_t *param)
{
	stubborn_dispatch_uni(queue, func, param);
}

/* Called when dispatch must not fail. */
static inline void stubborn_dispatch_recv(struct xtm_queue *queue, void (*func)(recv_data_t *), recv_data_t *param)
{
	stubborn_dispatch_uni(queue, func, param);
}

/* Launched in HTTP server thread. */
static inline recv_data_t *alloc_recv_data(void)
{
	/* FIXME: Use per-thread pools? */
	recv_data_t *const recv_data = (recv_data_t *)malloc(conf.recv_data_size);
	if (recv_data == NULL)
		h2o_fatal("no memory");
	return recv_data;
}

/* Launched in HTTP server thread. */
static inline recv_data_t *prepare_websocket_recv_data(shuttle_t *parent, unsigned payload_bytes)
{
	recv_data_t *const recv_data = alloc_recv_data();
	recv_data->parent_shuttle = parent;
	recv_data->payload_bytes = payload_bytes;
	return recv_data;
}

/* Launched in HTTP server thread or in TX thread when !SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD. */
static void free_shuttle_internal(shuttle_t *shuttle)
{
	assert(shuttle->disposed);
	free_shuttle(shuttle);
}

/* Launched in HTTP server thread or in TX thread when !SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD.
 * FIXME: Only assert is different, can optimize for release build. */
static void free_lua_websocket_shuttle_internal(shuttle_t *shuttle)
{
	assert(!shuttle->disposed);
	free_shuttle(shuttle);
}

/* Launched in TX thread.
 * It can queue request to HTTP thread or free everything itself. */
void free_shuttle_from_tx(shuttle_t *shuttle)
{
#ifdef SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD
	/* Can't call free_shuttle() from TX thread because it [potentially] uses per-thread pools. */
	stubborn_dispatch(shuttle->thread_ctx->queue_from_tx, &free_shuttle_internal, shuttle);
#else /* SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD */
	free_shuttle_internal(shuttle);
#endif /* SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD */
}

/* Launched in TX thread. */
static inline void free_lua_shuttle_from_tx(shuttle_t *shuttle)
{
	assert(!((lua_response_t *)&shuttle->payload)->upgraded_to_websocket);
	free_shuttle_from_tx(shuttle);
}

/* Launched in TX thread. */
static inline void free_lua_websocket_shuttle_from_tx(shuttle_t *shuttle)
{
	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	assert(response->upgraded_to_websocket);
	if (response->recv_fiber != NULL) {
		response->is_recv_fiber_cancelled = true;
		assert(response->is_recv_fiber_waiting);
		fiber_wakeup(response->recv_fiber);
		fiber_yield();
		struct lua_State *const L = luaT_state();
		luaL_unref(L, LUA_REGISTRYINDEX, response->lua_recv_handler_ref);
		luaL_unref(L, LUA_REGISTRYINDEX, response->lua_recv_state_ref);
	}
#ifdef SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD
	/* Can't call free_shuttle() from TX thread because it [potentially] uses per-thread pools. */
	stubborn_dispatch(shuttle->thread_ctx->queue_from_tx, &free_lua_websocket_shuttle_internal, shuttle);
#else /* SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD */
	free_lua_websocket_shuttle_internal(shuttle);
#endif /* SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD */
}

/* Launched in HTTP server thread or in TX thread when !SHOULD_FREE_RECV_DATA_IN_HTTP_SERVER_THREAD. */
static inline void free_recv_data(recv_data_t *recv_data)
{
	free(recv_data);
}

/* Launched in HTTP server thread or in TX thread when !SHOULD_FREE_RECV_DATA_IN_HTTP_SERVER_THREAD. */
static void free_lua_websocket_recv_data_internal(recv_data_t *recv_data)
{
	free_recv_data(recv_data);
}

/* Launched in TX thread. */
static inline void free_lua_websocket_recv_data_from_tx(recv_data_t *recv_data)
{
#ifdef SHOULD_FREE_RECV_DATA_IN_HTTP_SERVER_THREAD
	/* Can't call free_recv_data() from TX thread because it [potentially] uses per-thread pools w/o mutexes. */
	stubborn_dispatch_recv(recv_data->parent_shuttle->thread_ctx->queue_from_tx, &free_lua_websocket_recv_data_internal, recv_data);
#else /* SHOULD_FREE_RECV_DATA_IN_HTTP_SERVER_THREAD */
	free_lua_websocket_recv_data_internal(recv_data);
#endif /* SHOULD_FREE_RECV_DATA_IN_HTTP_SERVER_THREAD */
}

/* Launched in TX thread */
static void cancel_processing_lua_req_in_tx(shuttle_t *shuttle)
{
	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	assert(!response->upgraded_to_websocket);

	/* We do not use fiber_cancel() because it causes exception
	 * in Lua code so Lua handler have to use pcall() and even
	 * that is not 100% guarantee because such exception
	 * can theoretically happen before pcall().
	 * Also we have unref Lua state. */
	if (response->fiber == NULL)
		free_lua_shuttle_from_tx(shuttle);
	else if (response->waiter != NULL) {
		assert(!response->fiber_done);
		response->cancelled = true;
		fiber_wakeup(response->waiter->fiber);
	} else if (response->fiber_done)
		free_lua_shuttle_from_tx(shuttle);
	else
		response->cancelled = true;
		; /* Fiber would clean up because we have set cancelled=true */
}

/* Launched in HTTP server thread */
static void free_shuttle_lua(shuttle_t *shuttle)
{
	lua_response_t *const response = (lua_response_t *)(&shuttle->payload);
	if (!response->upgraded_to_websocket) {
		shuttle->disposed = true;
		stubborn_dispatch(get_curr_thread_ctx()->queue_to_tx, &cancel_processing_lua_req_in_tx, shuttle);
	}
}

/* Launched in TX thread */
static void continue_processing_lua_req_in_tx(lua_response_t *response)
{
	assert(response->fiber != NULL);
	assert(!response->fiber_done);
	assert(response->waiter != NULL);
	assert(response->waiter->fiber != NULL);
	fiber_wakeup(response->waiter->fiber);
}

/* Launched in HTTP server thread when H2O has sent everything and asks for more */
static void proceed_sending_lua(h2o_generator_t *self, h2o_req_t *req)
{
	shuttle_t *const shuttle = get_shuttle_from_generator_lua(self);
	thread_ctx_t *const thread_ctx = get_curr_thread_ctx();
	stubborn_dispatch_lua(thread_ctx->queue_to_tx, continue_processing_lua_req_in_tx, (lua_response_t *)&shuttle->payload);
}

static inline void send_lua(h2o_req_t *req, lua_response_t *const response)
{
	h2o_iovec_t buf;
	buf.base = (void *)response->un.resp.any.payload;
	buf.len = response->un.resp.any.payload_len;
	h2o_send(req, &buf, 1, response->un.resp.any.is_last_send ? H2O_SEND_STATE_FINAL : H2O_SEND_STATE_IN_PROGRESS);
}

/* Launched in HTTP server thread to postprocess first response (with HTTP headers) */
static void postprocess_lua_req_first(shuttle_t *shuttle)
{
	lua_response_t *const response = (lua_response_t *)(&shuttle->payload);
	if (shuttle->disposed)
		return;
	h2o_req_t *const req = shuttle->never_access_this_req_from_tx_thread;
	req->res.status = response->un.resp.first.http_code;
	req->res.reason = "OK"; /* FIXME? */
	const unsigned num_headers = response->un.resp.first.num_headers;
	unsigned header_idx;
	for (header_idx = 0; header_idx < num_headers; ++header_idx) {
		const http_header_entry_t *const header = &response->un.resp.first.headers[header_idx];
		h2o_add_header_by_str(&req->pool, &req->res.headers, header->name, header->name_len,
				1, /* FIXME: Benchmark whether this faster than 0 */
				NULL, /* FIXME: Do we need orig_name? */
				header->value, header->value_len);

	}

	response->un.resp.any.generator = (h2o_generator_t){ proceed_sending_lua, NULL }; /* Do not use stop_sending, we handle everything in free_shuttle_lua(). */
	h2o_start_response(req, &response->un.resp.any.generator);
	send_lua(req, response);
}

/* Launched in HTTP server thread to postprocess response (w/o HTTP headers) */
static void postprocess_lua_req_others(shuttle_t *shuttle)
{
	lua_response_t *const response = (lua_response_t *)(&shuttle->payload);
	if (shuttle->disposed)
		return;
	h2o_req_t *const req = shuttle->never_access_this_req_from_tx_thread;
	send_lua(req, response);
}

static inline void add_http_header_to_lua_response(lua_first_response_only_t *response, const char *key, size_t key_len, const char *value, size_t value_len)
{
	if (response->num_headers >= conf.max_headers_lua)
		/* FIXME: Misconfiguration, should we log something? */
		return;

	response->headers[response->num_headers++] = (http_header_entry_t){key, value, key_len, value_len};
}

/* Launched in TX thread.
 * Makes sure earlier queued sends to HTTP thread are done. */
static void take_shuttle_ownership_lua(lua_response_t *response)
{
	if (response->waiter == NULL)
		return;

	/* Other fiber(s) are already waiting for shuttle return or taking ownership,
	 * add ourself into tail of waiting list. */
	waiter_t waiter = { .next = NULL, .fiber = fiber_self() };
	waiter_t *last_waiter = response->waiter;
	/* FIXME: It may be more efficient to use double-linked list if we expect a lot of competing fibers. */
	while (last_waiter->next != NULL)
		last_waiter = last_waiter->next;
	last_waiter->next = &waiter;
	fiber_yield();
}

/* Launched in TX thread.
 * Caller must call take_shuttle_ownership_lua() before filling in shuttle and calling us.
 */
static inline void wait_for_lua_shuttle_return(lua_response_t *response)
{
	/* Add us into head of waiting list. */
	waiter_t waiter = { .next = response->waiter, .fiber = fiber_self() };
	response->waiter = &waiter;
	fiber_yield();
	assert(response->waiter == &waiter);
	response->waiter = waiter.next;
	if (response->waiter) {
		struct fiber *fiber = response->waiter->fiber;
		response->waiter = response->waiter->next;
		fiber_wakeup(fiber);
	}
}

/* Launched in TX thread. */
static inline int get_default_http_code(lua_response_t *response)
{
	assert(!response->sent_something);
	return 200; /* FIXME: Could differ depending on HTTP request type. */
}

/* Launched in TX thread */
static int payload_writer_write(lua_State *L)
{
	/* Lua parameters: self, payload, is_last. */
	const unsigned num_params = lua_gettop(L);
	if (num_params < 2)
		goto Error;

	lua_getfield(L, 1, "shuttle");
	if (!lua_islightuserdata(L, -1))
		goto Error;
	shuttle_t *const shuttle = (shuttle_t *)lua_touserdata(L, -1);

	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	take_shuttle_ownership_lua(response);
	if (response->cancelled) {
		/* Returning Lua true because connection has already been closed. */
		lua_pushboolean(L, true);
		return 1;
	}

	size_t payload_len;
	response->un.resp.any.payload = lua_tolstring(L, 2, &payload_len);
	response->un.resp.any.payload_len = payload_len;

	bool is_last;
	if (num_params >= 3)
		is_last	= lua_toboolean(L, 3);
	else
		is_last = false;

	if (!response->sent_something) {
		response->un.resp.first.http_code = get_default_http_code(response);

		lua_getfield(L, 1, "headers");
		const unsigned headers_lua_index = num_params + 1 + 1;
		fill_http_headers(L, response, headers_lua_index);

		response->un.resp.any.is_last_send = is_last;
		response->sent_something = true;
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
			lua_pushlightuserdata(L, shuttle);
			lua_setfield(L, -2, "shuttle");
		}
		return 1;
	}

	response->un.resp.any.is_last_send = is_last;
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

/* Launched in TX thread. */
static void fill_http_headers(lua_State *L, lua_response_t *response, int param_lua_idx)
{
	if (lua_isnil(L, param_lua_idx))
		return;

	lua_pushnil(L); /* Start of table. */
	while (lua_next(L, param_lua_idx)) {
		size_t key_len;
		size_t value_len;
		const char *const key = lua_tolstring(L, -2, &key_len);
		const char *const value = lua_tolstring(L, -1, &value_len);

		add_http_header_to_lua_response(&response->un.resp.first, key, key_len, value, value_len);

		/* Remove value, keep key for next iteration. */
		lua_pop(L, 1);
	}
}

/* Launched in TX thread */
static int header_writer_write_header(lua_State *L)
{
	/* Lua parameters: self, code, headers, payload, is_last. */
	const unsigned num_params = lua_gettop(L);
	if (num_params < 2)
		goto Error;

	lua_getfield(L, 1, "shuttle");

	if (!lua_islightuserdata(L, -1))
		goto Error;
	shuttle_t *const shuttle = (shuttle_t *)lua_touserdata(L, -1);

	bool is_last;
	if (num_params >= 5)
		is_last	= lua_toboolean(L, 5);
	else
		is_last = false;

	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	take_shuttle_ownership_lua(response);
	if (response->sent_something)
		goto Error;
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

	int is_integer;
	response->un.resp.first.http_code = lua_tointegerx(L, 2, &is_integer);
	if (!is_integer)
		goto Error;

	unsigned headers_lua_index;
	if (num_params >= 3)
		headers_lua_index = 3;
	else {
		lua_getfield(L, 1, "headers");
		headers_lua_index = num_params + 1 + 1;
	}
	fill_http_headers(L, response, headers_lua_index);

	if (num_params >= 4) {
		size_t payload_len;
		response->un.resp.any.payload = lua_tolstring(L, 4, &payload_len);
		response->un.resp.any.payload_len = payload_len;
	} else {
		response->un.resp.any.payload_len = 0;
	}

	response->un.resp.any.is_last_send = is_last;
	response->sent_something = true;
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
		lua_pushlightuserdata(L, shuttle);
		lua_setfield(L, -2, "shuttle");
	}
	return 1;

Error: /* FIXME: Error message? */
	lua_pushnil(L);
	return 1;
}

/* Launched in TX thread */
static void cancel_processing_lua_websocket_in_tx(lua_response_t *response)
{
	assert(response->fiber != NULL);
	assert(!response->fiber_done);
	response->cancelled = true;
}

static inline char *get_websocket_recv_location(recv_data_t *const recv_data)
{
	return recv_data->payload;
}

/* Launched in TX thread. */
static void process_lua_websocket_received_data_in_tx(recv_data_t *recv_data)
{
	shuttle_t *const shuttle = recv_data->parent_shuttle;
	assert(shuttle != NULL);
	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	assert(response->fiber != NULL);
	assert(!response->fiber_done);

	/* FIXME: Should we do this check in HTTP server thread? */
	if (response->recv_fiber != NULL) {
		if (response->is_recv_fiber_waiting) {
			response->recv_data = recv_data;
			fiber_wakeup(response->recv_fiber);
			fiber_yield();
		} else
			fprintf(stderr, "User WebSocket recv handler for \"\%s\" is NOT allowed to yield, data has been lost\n", response->site_path);
	} else
		free_lua_websocket_recv_data_from_tx(recv_data);
}

/* Launched in HTTP server thread. */
static void websocket_msg_callback(h2o_websocket_conn_t *conn, const struct wslay_event_on_msg_recv_arg *arg)
{
	shuttle_t *const shuttle = conn->data;
	if (arg == NULL) {
		lua_response_t *const response = (lua_response_t *)&shuttle->payload;
		assert(conn == response->ws_conn);
		h2o_websocket_close(conn);
		response->ws_conn = NULL;
		stubborn_dispatch_lua(get_curr_thread_ctx()->queue_to_tx, cancel_processing_lua_websocket_in_tx, response);
		return;
	}

	if (wslay_is_ctrl_frame(arg->opcode))
		return;

	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	size_t bytes_remain = arg->msg_length;
	const unsigned char *pos = arg->msg;
	while (1) {
		/* FIXME: Need flag about splitting to parts.
		 * Probably should have upper limit on a number of active recv_data - we can eat A LOT of memory. */
		const unsigned bytes_to_send = bytes_remain > conf.max_recv_bytes_lua_websocket ? conf.max_recv_bytes_lua_websocket : bytes_remain;
		recv_data_t *const recv_data = prepare_websocket_recv_data(shuttle, bytes_to_send);
		memcpy(get_websocket_recv_location(recv_data), pos, bytes_to_send);
		stubborn_dispatch_recv(get_curr_thread_ctx()->queue_to_tx, process_lua_websocket_received_data_in_tx, recv_data);
		if (response->ws_conn == NULL)
			/* Handler has closed connection already. */
			break;
		if (!(bytes_remain -= bytes_to_send))
			break;
		pos += bytes_to_send;
	}
}

/* Launched in HTTP server thread to postprocess upgrade to WebSocket. */
static void postprocess_lua_req_upgrade_to_websocket(shuttle_t *shuttle)
{
	lua_response_t *const response = (lua_response_t *)(&shuttle->payload);
	if (shuttle->disposed)
		return;
	h2o_req_t *const req = shuttle->never_access_this_req_from_tx_thread;

	const unsigned num_headers = response->un.resp.first.num_headers;
	unsigned header_idx;
	for (header_idx = 0; header_idx < num_headers; ++header_idx) {
		const http_header_entry_t *const header = &response->un.resp.first.headers[header_idx];
		h2o_add_header_by_str(&req->pool, &req->res.headers, header->name, header->name_len,
				1, /* FIXME: Benchmark whether this faster than 0. */
				NULL, /* FIXME: Do we need orig_name? */
				header->value, header->value_len);

	}
	response->upgraded_to_websocket = true;
	response->ws_conn = h2o_upgrade_to_websocket(req, response->ws_client_key, shuttle, websocket_msg_callback);
	/* anchor_dispose()/free_shuttle_lua() will be called by h2o. */
	stubborn_dispatch_lua(get_curr_thread_ctx()->queue_to_tx, continue_processing_lua_req_in_tx, response);
}

/* Launched in HTTP server thread to postprocess first response (with HTTP headers). */
static void postprocess_lua_req_websocket_send_text(lua_response_t *response)
{
	/* Do not check shuttle->disposed, this is a WebSocket now. */

	struct wslay_event_msg msgarg = { .opcode = WSLAY_TEXT_FRAME, .msg = (unsigned char *)response->un.resp.any.payload, .msg_length = response->un.resp.any.payload_len, };
	if (wslay_event_queue_msg(response->ws_conn->ws_ctx, &msgarg) || wslay_event_send(response->ws_conn->ws_ctx))
		response->ws_send_failed = true;
	stubborn_dispatch_lua(get_curr_thread_ctx()->queue_to_tx, continue_processing_lua_req_in_tx, response);
}

/* Launched in TX thread */
static int websocket_send_text(lua_State *L)
{
	/* Lua parameters: self, payload. */
	const unsigned num_params = lua_gettop(L);
	if (num_params < 2)
		goto Error;

	lua_getfield(L, 1, "shuttle");
	if (!lua_islightuserdata(L, -1))
		goto Error;
	shuttle_t *const shuttle = (shuttle_t *)lua_touserdata(L, -1);

	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	if (response->in_recv_handler) {
		fprintf(stderr, "User WebSocket recv handler for \"\%s\" is NOT allowed to call yielding functions\n", response->site_path);
		goto Error;
	}
	take_shuttle_ownership_lua(response);
	if (response->cancelled || response->ws_send_failed) {
		/* Returning Lua true because connection has already been closed or previous send failed. */
		lua_pushboolean(L, true);
		return 1;
	}

	size_t payload_len;
	response->un.resp.any.payload = lua_tolstring(L, 2, &payload_len);
	response->un.resp.any.payload_len = payload_len;

	stubborn_dispatch_lua(shuttle->thread_ctx->queue_from_tx, &postprocess_lua_req_websocket_send_text, response);
	wait_for_lua_shuttle_return(response);

	/* Returning Lua true if send failed or connection has already been closed. */
	lua_pushboolean(L, response->ws_send_failed ? true : response->cancelled);
	return 1;

Error: /* FIXME: Error message. */
	lua_pushboolean(L, true);
	return 1;
}

/* Launched in HTTP server thread. */
static void close_websocket(lua_response_t *const response)
{
	if (response->ws_conn != NULL) {
		h2o_websocket_close(response->ws_conn);
		response->ws_conn = NULL;
	}
	stubborn_dispatch_lua(get_curr_thread_ctx()->queue_to_tx, continue_processing_lua_req_in_tx, response);
}

/* Launched in TX thread. */
static int close_lua_websocket(lua_State *L)
{
	/* Lua parameters: self. */
	const unsigned num_params = lua_gettop(L);
	if (num_params < 1)
		goto Error;

	lua_getfield(L, 1, "shuttle");
	if (!lua_islightuserdata(L, -1))
		goto Error;
	shuttle_t *const shuttle = (shuttle_t *)lua_touserdata(L, -1);

	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	if (response->in_recv_handler) {
		fprintf(stderr, "User WebSocket recv handler for \"\%s\" is NOT allowed to call yielding functions\n", response->site_path);
		goto Error;
	}
	take_shuttle_ownership_lua(response);
	if (response->cancelled)
		return 0;

	response->cancelled = true;
	stubborn_dispatch_lua(shuttle->thread_ctx->queue_from_tx, &close_websocket, response);
	wait_for_lua_shuttle_return(response);
	return 0;

Error: /* FIXME: Error message. */
	return 0;
}

/* Launched in TX thread. */
static int
lua_websocket_recv_fiber_func(va_list ap)
{
	shuttle_t *const shuttle = va_arg(ap, shuttle_t *);
	lua_State *const L = va_arg(ap, lua_State *);
	lua_response_t *const response = (lua_response_t *)&shuttle->payload;

	while (1) {
		response->is_recv_fiber_waiting = true;
		fiber_yield();
		if (response->is_recv_fiber_cancelled)
			/* FIXME: Can we leak recv_data? */
			return 0;
		response->is_recv_fiber_waiting = false;

		lua_rawgeti(L, LUA_REGISTRYINDEX, response->lua_recv_handler_ref); /* User handler function, written in Lua. */

		recv_data_t *const recv_data = response->recv_data;
		assert(recv_data->parent_shuttle == shuttle);
		/* First param for Lua WebSocket recv handler - data. */
		lua_pushlstring(L, get_websocket_recv_location(recv_data), recv_data->payload_bytes);

		/* N. b.: WebSocket recv handler is NOT allowed to yield. */
		response->in_recv_handler = true;
		if (lua_pcall(L, 1, 0, 0) != LUA_OK)
			/* FIXME: Should probably log this instead(?).
			 * Should we stop calling handler? */
			fprintf(stderr, "User WebSocket recv handler for \"\%s\" failed with error \"%s\"\n", response->site_path, lua_tostring(L, -1));
		response->in_recv_handler = false;
		free_lua_websocket_recv_data_from_tx(recv_data);
		fiber_wakeup(response->tx_fiber);
	}

	return 0;
}

/* Launched in TX thread */
static int header_writer_upgrade_to_websocket(lua_State *L)
{
	/* Lua parameters: self, headers, recv_function. */
	const unsigned num_params = lua_gettop(L);
	if (num_params < 1)
		goto Error;

	lua_getfield(L, 1, "shuttle");
	if (!lua_islightuserdata(L, -1))
		goto Error;
	shuttle_t *const shuttle = (shuttle_t *)lua_touserdata(L, -1);

	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	take_shuttle_ownership_lua(response);
	if (response->sent_something)
		goto Error;
	if (response->cancelled) {
		/* Can't send anything, connection has been closed. */
		lua_pushnil(L);
		return 1;
	}

	if (num_params >= 2 && !lua_isnil(L, 2)) {
		lua_pushnil(L); /* Start of table. */
		while (lua_next(L, 2)) {
			size_t key_len;
			size_t value_len;
			const char *const key = lua_tolstring(L, -2, &key_len);
			const char *const value = lua_tolstring(L, -1, &value_len);

			add_http_header_to_lua_response(&response->un.resp.first, key, key_len, value, value_len);

			/* Remove value, keep key for next iteration. */
			lua_pop(L, 1);
		}
	}

	if (num_params != 3 || lua_isnil(L, 3) || lua_type(L, 3) != LUA_TFUNCTION)
		response->recv_fiber = NULL;
	else {
		lua_pop(L, 1);
		response->lua_recv_handler_ref = luaL_ref(L, LUA_REGISTRYINDEX);
		struct lua_State *const new_L = lua_newthread(L);
		response->lua_recv_state_ref = luaL_ref(L, LUA_REGISTRYINDEX);
		if ((response->recv_fiber = fiber_new("HTTP Lua WebSocket recv fiber", &lua_websocket_recv_fiber_func)) == NULL) {
			luaL_unref(L, LUA_REGISTRYINDEX, response->lua_recv_handler_ref);
			luaL_unref(L, LUA_REGISTRYINDEX, response->lua_recv_state_ref);
			lua_pushnil(L);
			return 1;
		}
		response->is_recv_fiber_waiting = false;
		response->is_recv_fiber_cancelled = false;
		fiber_start(response->recv_fiber, shuttle, new_L);
	}

	response->sent_something = true;
	response->ws_send_failed = false;
	response->in_recv_handler = false;
	stubborn_dispatch(shuttle->thread_ctx->queue_from_tx, &postprocess_lua_req_upgrade_to_websocket, shuttle);
	wait_for_lua_shuttle_return(response);

	if (response->cancelled)
		lua_pushnil(L);
	else {
		lua_createtable(L, 0, 3);
		lua_pushcfunction(L, websocket_send_text);
		lua_setfield(L, -2, "send_text");
		lua_pushcfunction(L, close_lua_websocket);
		lua_setfield(L, -2, "close");
		lua_pushlightuserdata(L, shuttle);
		lua_setfield(L, -2, "shuttle");
	}
	return 1;

Error: /* FIXME: Error message? */
	lua_pushnil(L);
	return 1;
}

/* Launched in TX thread. */
static inline void fill_received_headers(lua_State *L, lua_response_t *response)
{
	assert(!response->sent_something);
	const received_http_header_handle_t *const handles = (received_http_header_handle_t *)&response->un.req.buffer[response->un.req.path_len];
	const unsigned num_headers = response->un.req.num_headers;
	lua_createtable(L, 0, num_headers);
	unsigned current_offset = response->un.req.path_len + num_headers * sizeof(received_http_header_handle_t);
	unsigned header_idx;
	for (header_idx = 0; header_idx < num_headers; ++header_idx) {
		const received_http_header_handle_t *const handle = &handles[header_idx];
		lua_pushlstring(L, &response->un.req.buffer[current_offset + handle->name_size + 1], handle->value_size);
		lua_setfield(L, -2, &response->un.req.buffer[current_offset]); /* N. b.: it must be NULL-terminated! */
		current_offset += handle->name_size + 1 + handle->value_size;
	}
}

/* Launched in TX thread */
static int close_lua_req(lua_State *L)
{
	/* Lua parameters: self. */
	const unsigned num_params = lua_gettop(L);
	if (num_params < 1)
		goto Error;

	lua_getfield(L, 1, "shuttle");
	if (!lua_islightuserdata(L, -1))
		goto Error;
	shuttle_t *const shuttle = (shuttle_t *)lua_touserdata(L, -1);
	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	take_shuttle_ownership_lua(response);
	if (response->cancelled)
		return 0;

	response->un.resp.any.payload_len = 0;
	response->un.resp.any.is_last_send = true;
	if (response->sent_something)
		stubborn_dispatch(shuttle->thread_ctx->queue_from_tx, &postprocess_lua_req_others, shuttle);
	else {
		response->un.resp.first.http_code = get_default_http_code(response);
		response->sent_something = true;
		stubborn_dispatch(shuttle->thread_ctx->queue_from_tx, &postprocess_lua_req_first, shuttle);
	}
	wait_for_lua_shuttle_return(response);
	return 0;

Error: /* FIXME: Error message */
	return 0;
}

/* Launched in TX thread */
static int
lua_fiber_func(va_list ap)
{
	shuttle_t *const shuttle = va_arg(ap, shuttle_t *);
	lua_State *const L = va_arg(ap, lua_State *);

	lua_response_t *const response = (lua_response_t *)&shuttle->payload;

	lua_rawgeti(L, LUA_REGISTRYINDEX, response->un.req.lua_handler_ref); /* User handler function, written in Lua. */

	/* First param for Lua handler - query */
	lua_createtable(L, 0, 7);
	lua_pushinteger(L, response->un.req.version_major);
	lua_setfield(L, -2, "version_major");
	lua_pushinteger(L, response->un.req.version_minor);
	lua_setfield(L, -2, "version_minor");
	lua_pushlstring(L, response->un.req.buffer, response->un.req.path_len);
	lua_setfield(L, -2, "path");
	lua_pushinteger(L, (response->un.req.query_at == LUA_QUERY_NONE) ? -1 : (response->un.req.query_at + 1)); /* Lua indexes start from 1 */
	lua_setfield(L, -2, "query_at");
	lua_pushlstring(L, response->un.req.method, response->un.req.method_len);
	lua_setfield(L, -2, "method");
	lua_pushboolean(L, !!response->un.req.ws_client_key_len);
	lua_setfield(L, -2, "is_websocket");
	fill_received_headers(L, response);
	lua_setfield(L, -2, "headers");

	/* We have finished parsing request, now can write to response (it is union). */
	response->un.resp.first.num_headers = 0;

	/* Second param for Lua handler - header_writer */
	lua_createtable(L, 0, 6);
	lua_pushcfunction(L, header_writer_write_header);
	lua_setfield(L, -2, "write_header");
	lua_pushcfunction(L, payload_writer_write);
	lua_setfield(L, -2, "write");
	lua_pushcfunction(L, header_writer_upgrade_to_websocket);
	lua_setfield(L, -2, "upgrade_to_websocket");
	lua_pushcfunction(L, close_lua_req);
	lua_setfield(L, -2, "close");
	lua_pushlightuserdata(L, shuttle);
	lua_setfield(L, -2, "shuttle");
	lua_createtable(L, 0, 2);
	lua_setfield(L, -2, "headers");

	const int lua_state_ref = response->lua_state_ref;
	if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
		/* FIXME: Should probably log this instead(?) */
		fprintf(stderr, "User handler for \"\%s\" failed with error \"%s\"\n", response->site_path, lua_tostring(L, -1));

		if (response->cancelled) {
			/* No point trying to send something, connection has already been closed. */
			/* There would be no more calls from HTTP thread, must clean up. */
			if (response->upgraded_to_websocket)
				free_lua_websocket_shuttle_from_tx(shuttle);
			else
				free_lua_shuttle_from_tx(shuttle);
		} else if (response->upgraded_to_websocket) {
			take_shuttle_ownership_lua(response);
			stubborn_dispatch_lua(shuttle->thread_ctx->queue_from_tx, &close_websocket, response);
			wait_for_lua_shuttle_return(response);
			free_lua_websocket_shuttle_from_tx(shuttle);
		} else {
			take_shuttle_ownership_lua(response);
			response->un.resp.any.is_last_send = true;
			if (response->sent_something) {
				/* Do not add anything to user output to prevent corrupt HTML etc. */
				response->un.resp.any.payload_len = 0;
				stubborn_dispatch(shuttle->thread_ctx->queue_from_tx, &postprocess_lua_req_others, shuttle);
			} else {
				static const char key[] = "content-type";
				static const char value[] = "text/plain; charset=utf-8";
				add_http_header_to_lua_response(&response->un.resp.first, key, sizeof(key) - 1, value, sizeof(value) - 1);
				static const char error_str[] = "Path handler execution error";
				response->un.resp.first.http_code = 500;
				response->un.resp.any.payload = error_str;
				response->un.resp.any.payload_len = sizeof(error_str) - 1;
				/* Not setting sent_something because no one would check it. */
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
	} else if (response->cancelled) {
		/* There would be no more calls from HTTP thread, must clean up */
		if (response->upgraded_to_websocket)
			free_lua_websocket_shuttle_from_tx(shuttle);
		else
			free_lua_shuttle_from_tx(shuttle);
	} else if (response->upgraded_to_websocket) {
		take_shuttle_ownership_lua(response);
		stubborn_dispatch_lua(shuttle->thread_ctx->queue_from_tx, &close_websocket, response);
		wait_for_lua_shuttle_return(response);
		free_lua_websocket_shuttle_from_tx(shuttle);
	} else if (lua_isnil(L, -1))
		response->fiber_done = true;
		/* cancel_processing_lua_req_in_tx() is not yet called, it would clean up because we have set fiber_done=true */
	else {
		const bool old_sent_something = response->sent_something;
		if (!old_sent_something) {
			lua_getfield(L, -1, "status");
			if (lua_isnil(L, -1))
				response->un.resp.first.http_code = get_default_http_code(response);
			else {
				int is_integer;
				response->un.resp.first.http_code = lua_tointegerx(L, -1, &is_integer);
				if (!is_integer)
					response->un.resp.first.http_code = get_default_http_code(response);
			}
			lua_getfield(L, -2, "headers");
			fill_http_headers(L, response, -2);
			lua_pop(L, 2); /* headers, status. */
			response->sent_something = true;
		}

		lua_getfield(L, -1, "body");
		if (!lua_isnil(L, -1)) {
			size_t payload_len;
			response->un.resp.any.payload = lua_tolstring(L, -1, &payload_len);
			response->un.resp.any.payload_len = payload_len;
		} else
			response->un.resp.any.payload_len = 0;

		response->un.resp.any.is_last_send = true;
		take_shuttle_ownership_lua(response);
		stubborn_dispatch(shuttle->thread_ctx->queue_from_tx, old_sent_something ? postprocess_lua_req_others : postprocess_lua_req_first, shuttle);
		wait_for_lua_shuttle_return(response);

		if (response->cancelled) {
			/* There would be no more calls from HTTP thread, must clean up */
			if (response->upgraded_to_websocket)
				free_lua_websocket_shuttle_from_tx(shuttle);
			else
				free_lua_shuttle_from_tx(shuttle);
		} else
			response->fiber_done = true;
			/* cancel_processing_lua_req_in_tx() is not yet called, it would clean up because we have set fiber_done=true */
	}

	luaL_unref(luaT_state(), LUA_REGISTRYINDEX, lua_state_ref);
	return 0;
}

/* Launched in TX thread */
static void process_lua_req_in_tx(shuttle_t *shuttle)
{
	lua_response_t *const response = (lua_response_t *)&shuttle->payload;

#define RETURN_WITH_ERROR(err) \
	do { \
		luaL_unref(L, LUA_REGISTRYINDEX, response->lua_state_ref); \
		static const char key[] = "content-type"; \
		static const char value[] = "text/plain; charset=utf-8"; \
		response->un.resp.first.num_headers = 0; \
		add_http_header_to_lua_response(&response->un.resp.first, key, sizeof(key) - 1, value, sizeof(value) - 1); \
		static const char error_str[] = err; \
		response->un.resp.any.is_last_send = true; \
		response->un.resp.first.http_code = 500; \
		response->un.resp.any.payload = error_str; \
		response->un.resp.any.payload_len = sizeof(error_str) - 1; \
		/* Not setting sent_something because no one would check it. */ \
		stubborn_dispatch(shuttle->thread_ctx->queue_from_tx, &postprocess_lua_req_first, shuttle); \
		return; \
	} while (0)

	struct lua_State *const L = luaT_state();
	struct lua_State *const new_L = lua_newthread(L);
	response->lua_state_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	if ((response->fiber = fiber_new("HTTP Lua fiber", &lua_fiber_func)) == NULL)
		RETURN_WITH_ERROR("Failed to create fiber");
	response->fiber_done = false;
	response->tx_fiber = fiber_self();
	fiber_start(response->fiber, shuttle, new_L);
}
#undef RETURN_WITH_ERROR

/* Launched in HTTP server thread */
static int lua_req_handler(lua_h2o_handler_t *self, h2o_req_t *req)
{
	shuttle_t *const shuttle = prepare_shuttle(req);
	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	if ((response->un.req.method_len = req->method.len) > sizeof(response->un.req.method)) {
		/* Error */
		free_shuttle_with_anchor(shuttle);
		req->res.status = 500;
		req->res.reason = "Method name is too long";
		h2o_send_inline(req, H2O_STRLIT("Method name is too long\n"));
		return 0;
	}
	if ((response->un.req.path_len = req->path.len) > conf.max_path_len_lua) {
		/* Error */
		free_shuttle_with_anchor(shuttle);
		req->res.status = 500;
		req->res.reason = "Request is too long";
		h2o_send_inline(req, H2O_STRLIT("Request is too long\n"));
		return 0;
	}

	const char *ws_client_key;
	(void)h2o_is_websocket_handshake(req, &ws_client_key);
	if (ws_client_key == NULL)
		response->un.req.ws_client_key_len = 0;
	else {
		response->un.req.ws_client_key_len = WS_CLIENT_KEY_LEN;
		memcpy(response->ws_client_key, ws_client_key, response->un.req.ws_client_key_len);
	}

	memcpy(response->un.req.method, req->method.base, response->un.req.method_len);
	memcpy(response->un.req.buffer, req->path.base, response->un.req.path_len);

	static_assert(LUA_QUERY_NONE < (1ULL << (8 * sizeof(response->un.req.query_at))));
	response->un.req.query_at = (req->query_at == SIZE_MAX) ? LUA_QUERY_NONE : req->query_at;
	response->un.req.version_major = req->version >> 8;
	response->un.req.version_minor = req->version & 0xFF;

	const h2o_header_t *const headers = req->headers.entries;
	const size_t num_headers = req->headers.size;
	const unsigned bytes_remain_in_buffer = conf.max_path_len_lua - response->un.req.path_len;

	/* response->un.req.buffer[] format:
	 *
	 * char path[req->path.len]
	 * FIXME: Alignment to at least to header_offset_t should be here (compatibility/performance).
	 * received_http_header_handle_t handles[num_headers]
	 * {repeat num_headers times} char name[handles[i].name_size], '\0', char value[handles[i].value_size]
	 *
	 * '\0' is for lua_setfield().
	 * */
	const unsigned max_headers = bytes_remain_in_buffer / sizeof(received_http_header_handle_t);
	if (num_headers > max_headers) {
		/* Error. */
		free_shuttle_with_anchor(shuttle);
		req->res.status = 500;
		req->res.reason = "Too many headers";
		h2o_send_inline(req, H2O_STRLIT("Too many headers\n"));
		return 0;
	}
	unsigned current_offset = response->un.req.path_len;
	const unsigned max_offset = conf.max_path_len_lua;
	received_http_header_handle_t *const handles = (received_http_header_handle_t *)&response->un.req.buffer[current_offset];
	current_offset += num_headers * sizeof(received_http_header_handle_t);
	unsigned header_idx;
	for (header_idx = 0; header_idx < num_headers; ++header_idx) {
		const h2o_header_t *const header = &headers[header_idx];
		if (current_offset + header->name->len + 1 + header->value.len > max_offset) {
			/* Error. */
			free_shuttle_with_anchor(shuttle);
			req->res.status = 500;
			req->res.reason = "Too large headers";
			h2o_send_inline(req, H2O_STRLIT("Too large headers\n"));
			return 0;
		}
		received_http_header_handle_t *const handle = &handles[header_idx];
		handle->name_size = header->name->len;
		handle->value_size = header->value.len;
		memcpy(&response->un.req.buffer[current_offset], header->name->base, handle->name_size);
		current_offset += handle->name_size;
		response->un.req.buffer[current_offset] = 0;
		++current_offset;
		memcpy(&response->un.req.buffer[current_offset], header->value.base, handle->value_size);
		current_offset += handle->value_size;
	}
	response->un.req.num_headers = num_headers;

	response->sent_something = false;
	response->cancelled = false;
	response->waiter = NULL;
	response->un.req.lua_handler_ref = self->lua_handler_ref;
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
	response->upgraded_to_websocket = false;

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

/* N. b.: *path must live until server is shutdown */
static h2o_pathconf_t *register_lua_handler(h2o_hostconf_t *hostconf, const char *path, size_t path_len, int lua_handler_ref)
{
	/* These functions never return NULL, dying instead */
	h2o_pathconf_t *pathconf = h2o_config_register_path(hostconf, path, 0);
	lua_h2o_handler_t *handler = (lua_h2o_handler_t *)h2o_create_handler(pathconf, sizeof(*handler));
	handler->super.on_req = (int (*)(h2o_handler_t *, h2o_req_t *))lua_req_handler;
	handler->lua_handler_ref = lua_handler_ref;
	handler->path = path;
	handler->path_len = path_len;
	return pathconf;
}

/* Launched in HTTP server thread. */
static inline shuttle_t *alloc_shuttle(thread_ctx_t *thread_ctx)
{
	/* FIXME: Use per-thread pools */
	(void)thread_ctx;
	shuttle_t *const shuttle = (shuttle_t *)malloc(conf.shuttle_size);
	if (shuttle == NULL)
		h2o_fatal("no memory");
	return shuttle;
}

/* Launched in HTTP server thread or in TX thread when !SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD. */
void free_shuttle(shuttle_t *shuttle)
{
	free(shuttle);
}

/* Launched in HTTP server thread. */
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

/* Launched in HTTP server thread. */
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
#ifdef DISABLE_HTTP2
	/* Disable HTTP/2 e. g. to test WebSockets. */
	static const h2o_iovec_t my_alpn_protocols[] = { {H2O_STRLIT("http/1.1")}, {NULL} } ;
	h2o_ssl_register_alpn_protocols(ssl_ctx, my_alpn_protocols);
#else

	h2o_ssl_register_alpn_protocols(ssl_ctx, h2o_http2_alpn_protocols);
#endif /* DISABLE_HTTP2 */
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
int cfg(lua_State *L)
{
	enum {
		LUA_STACK_IDX_TABLE = 1,
		LUA_STACK_IDX_LUA_SITES = -2,
	};
	memset(&conf.globalconf, 0, sizeof(conf.globalconf));

	if (lua_gettop(L) < 1)
		goto Error;

	lua_getfield(L, LUA_STACK_IDX_TABLE, "c_sites_func");
	const path_desc_t *path_descs;
	if (lua_isnil(L, -1)) {
		path_descs = NULL;
		goto Skip_c_sites;
	}
	if (lua_type(L, -1) != LUA_TFUNCTION)
		goto Error;

	lua_getfield(L, LUA_STACK_IDX_TABLE, "c_sites_func_param");
	if (lua_pcall(L, 1, 1, 0) != LUA_OK)
		goto Error;

	int is_integer;
	path_descs = (path_desc_t *)lua_tointegerx(L, -1, &is_integer);
	if (!is_integer)
		goto Error;
Skip_c_sites:
	lua_getfield(L, LUA_STACK_IDX_TABLE, "c_sites_func");

#define PROCESS_OPTIONAL_PARAM(name) \
	lua_getfield(L, LUA_STACK_IDX_TABLE, #name); \
	uint64_t name; \
	if (lua_isnil(L, -1)) \
		name = DEFAULT_##name; \
	else { \
		name = lua_tointegerx(L, -1, &is_integer); \
		if (!is_integer) \
			goto Error; \
		if (name > MAX_##name) \
			name = MAX_##name; \
		else if (name < MIN_##name) \
			name = MIN_##name; \
	}

	/* N. b.: These macros can "goto Error". */
	PROCESS_OPTIONAL_PARAM(threads);
	PROCESS_OPTIONAL_PARAM(max_conn_per_thread);
	PROCESS_OPTIONAL_PARAM(shuttle_size);

#undef PROCESS_OPTIONAL_PARAM

	conf.tx_fiber_should_work = 1;
	/* FIXME: Add sanity checks, especially shuttle_size - it must >sizeof(shuttle_t) (accounting for Lua payload) and aligned */
	conf.num_threads = threads;
	conf.shuttle_size = shuttle_size;
	conf.recv_data_size = shuttle_size; /* FIXME: Can differ from shuttle_size. */
	conf.max_headers_lua = (conf.shuttle_size - sizeof(shuttle_t) - offsetof(lua_response_t, un.resp.first.headers)) / sizeof(http_header_entry_t);
	conf.max_path_len_lua = conf.shuttle_size - sizeof(shuttle_t) - offsetof(lua_response_t, un.req.buffer);
	conf.max_recv_bytes_lua_websocket = conf.recv_data_size - (uintptr_t)get_websocket_recv_location(NULL);
	conf.max_conn_per_thread = max_conn_per_thread;
	conf.num_accepts = conf.max_conn_per_thread / 16;
	if (conf.num_accepts < 8)
		conf.num_accepts = 8;

	if ((conf.thread_ctxs = (thread_ctx_t *)malloc(conf.num_threads * sizeof(thread_ctx_t))) == NULL)
		goto Error;

	h2o_config_init(&conf.globalconf);
	h2o_hostconf_t *hostconf = h2o_config_register_host(&conf.globalconf, h2o_iovec_init(H2O_STRLIT("default")), H2O_DEFAULT_PORT_FOR_PROTOCOL_USED); //x x x: customizable
	if (hostconf == NULL)
		goto Error;

	if (path_descs != NULL) {
		const path_desc_t *path_desc = path_descs;
		if (path_desc->path == NULL)
			goto Error; /* Need at least one */

		do {
			register_handler(hostconf, path_desc->path, path_desc->handler);
		} while ((++path_desc)->path != NULL);
	}

	lua_site_t *lua_sites = NULL; /* FIXME: Free allocated memory - including malloc'ed path - when shutting down. */
	lua_getfield(L, LUA_STACK_IDX_TABLE, "sites");
	unsigned lua_site_idx = 0;
	if (lua_isnil(L, -1))
		goto Skip_lua_sites;
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

		lua_getfield(L, -2, "handler");
		if (lua_type(L, -1) != LUA_TFUNCTION)
			goto Error;

		register_lua_handler(hostconf, lua_site->path, path_len, luaL_ref(L, LUA_REGISTRYINDEX));

		++lua_site_idx;

		/* Remove path string and site array value, keep key for next iteration. */
		lua_pop(L, 2);
	}

Skip_lua_sites:
	lua_getfield(L, LUA_STACK_IDX_TABLE, "handler");
	if (lua_isnil(L, -1))
		goto Skip_main_lua_handler;
	if (lua_type(L, -1) != LUA_TFUNCTION)
		goto Error;

	lua_site_t *const new_lua_sites = realloc(lua_sites, sizeof(lua_site_t) * (lua_site_idx + 1));
	if (new_lua_sites == NULL)
		goto Error;
	lua_sites = new_lua_sites;
	lua_site_t *const lua_site = &lua_sites[lua_site_idx];
	if ((lua_site->path = malloc(1 + 1)) == NULL)
		goto Error;
	lua_site->path[0] = '/';
	lua_site->path[1] = 0;
	register_lua_handler(hostconf, lua_site->path, 2, luaL_ref(L, LUA_REGISTRYINDEX));
	++lua_site_idx;

Skip_main_lua_handler:
	;
	unsigned short port = DEFAULT_LISTEN_PORT;
	lua_getfield(L, LUA_STACK_IDX_TABLE, "listen");
	if (lua_isnil(L, -1))
		goto Skip_listen;
	if (lua_type(L, -1) != LUA_TTABLE)
		goto Error;

	lua_pushnil(L); /* Start of table. */
	while (lua_next(L, -2)) {
		lua_getfield(L, -1, "port");
		int is_integer;
		const uint64_t candidate = lua_tointegerx(L, -1, &is_integer);
		if (!is_integer || !candidate || candidate >= 65535)
			goto Error;
		port = candidate; /* Silently overwrite for now (FIXME: Multilisten). */
		/* Remove port and value, keep key for next iteration. */
		lua_pop(L, 2);
	}

Skip_listen:
	;
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
			if ((conf.listener_cfgs[listener_idx].fd = open_listener_ipv4("127.0.0.1", port)) == -1) /* FIXME: customizable. */
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

	if (path_descs != NULL) {
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

unsigned get_shuttle_size(void)
{
	assert(conf.shuttle_size >= MIN_shuttle_size);
	assert(conf.shuttle_size <= MAX_shuttle_size);
	return conf.shuttle_size;
}

static const struct luaL_Reg mylib[] = {
	{"cfg", cfg},
	{NULL, NULL}
};

int luaopen_httpng(lua_State *L)
{
	luaL_newlib(L, mylib);
	return 1;
}
