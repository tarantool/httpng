#include <fcntl.h>
#include <float.h>

#include <lauxlib.h>
#include <module.h>
#include <semaphore.h>

#include <xtm/xtm_api.h>

#ifdef USE_LIBUV
#define H2O_USE_LIBUV 1
#else
#define H2O_USE_EPOLL 1 /* FIXME */
#include <h2o/evloop_socket.h>
#endif /* USE_LIBUV */
#include <h2o.h>
#include <h2o/websocket.h>
#include "../third_party/h2o/deps/cloexec/cloexec.h"
#include "httpng_sem.h"

#ifdef USE_LIBUV
#include <uv.h>
#include <h2o/socket/uv-binding.h>
#endif /* USE_LIBUV */

#ifndef USE_LIBUV
/* evloop requires initing ctx in http thread (uses thread local vars).
 * Can't easily do the same for libuv - a lot of initializiation is
 * required and some functions can return errors.
 * */
#define INIT_CTX_IN_HTTP_THREAD
#endif /* USE_LIBUV */

#ifndef lengthof
#define lengthof(array) (sizeof(array) / sizeof((array)[0]))
#endif

#define STATIC_ASSERT(x, desc) do { \
		enum { \
			/* Will trigger zero division. */ \
			__static_assert_placeholder = 1 / !!(x), \
		}; \
	} while(0);

#ifdef TCP_FASTOPEN
#define H2O_DEFAULT_LENGTH_TCP_FASTOPEN_QUEUE 4096
#else
#define H2O_DEFAULT_LENGTH_TCP_FASTOPEN_QUEUE 0
#endif /* TCP_FASTOPEN */

/* Failing HTTP requests is fine, but failing to respond from TX thread
 * is not so queue size must be larger */
#define QUEUE_TO_TX_ITEMS (1 << 12) /* Must be power of 2 */
#define QUEUE_FROM_TX_ITEMS (QUEUE_TO_TX_ITEMS << 1) /* Must be power of 2 */

/* We would need this when (if) alloc would be performed from thread pools
 * w/o mutexes. */
#define SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD
#undef SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD

/* We would need this when (if) alloc would be performed from thread pools
 * w/o mutexes. */
#define SHOULD_FREE_RECV_DATA_IN_HTTP_SERVER_THREAD
#undef SHOULD_FREE_RECV_DATA_IN_HTTP_SERVER_THREAD

/* When disabled, HTTP requests with body not fitting into shuttle are failed.
 * N. b.: h2o allocates memory for the WHOLE body in any case. */
#define SPLIT_LARGE_BODY
//#undef SPLIT_LARGE_BODY

#define USE_HTTPS 1
//#define USE_HTTPS 0

/* For debugging. FIXME: Make runtime configurable from Lua. */
#define DISABLE_HTTP2
#undef DISABLE_HTTP2

#define H2O_DEFAULT_PORT_FOR_PROTOCOL_USED 65535
#define H2O_CONTENT_LENGTH_UNSPECIFIED SIZE_MAX

#define LUA_QUERY_NONE UINT_MAX

#define WS_CLIENT_KEY_LEN 24 /* Hardcoded in H2O. */

#define DEFAULT_threads 1
#define DEFAULT_max_conn_per_thread 65536
#define DEFAULT_shuttle_size 65536
#define DEFAULT_max_body_len (1024 * 1024)

/* Limits are quite relaxed for now. */
#define MIN_threads 1
#define MIN_max_conn_per_thread 1
#define MIN_shuttle_size (sizeof(shuttle_t) + sizeof(uintptr_t))
#define MIN_max_body_len 0

/* Limits are quite relaxed for now. */
#define MAX_threads 16 /* More than 4 is hardly useful (Lua). */
#define MAX_max_conn_per_thread (1024 * 1024)
#define MAX_shuttle_size (16 * 1024 * 1024)
#define MAX_max_body_len (64ULL * 1024 * 1024 * 1024)

#define DEFAULT_LISTEN_PORT 8080

/* N.b.: for SSL3 to work you should probably use custom OpenSSL build. */
#define SSL3_STR "ssl3"
#define TLS1_STR "tls1"
#define TLS1_0_STR "tls1.0"
#define TLS1_1_STR "tls1.1"
#define TLS1_2_STR "tls1.2"
#define TLS1_3_STR "tls1.3"

#define ADD_NEW_SITE_GENERATION_SHIFT 2

#define my_container_of(ptr, type, member) ({ \
	const typeof( ((type *)0)->member  ) *__mptr = \
		(typeof( &((type *)0)->member  ))(ptr); \
	(type *)( (char *)__mptr - offsetof(type,member)  );})

struct listener_ctx;

typedef struct {
#ifdef USE_LIBUV
	uv_tcp_t super;
#else /* USE_LIBUV */
	struct st_h2o_evloop_socket_t super;
#endif /* USE_LIBUV */
	h2o_linklist_t accepted_list;
} our_sock_t;

typedef struct {
	h2o_globalconf_t globalconf;
	h2o_context_t ctx;
	struct listener_ctx *listener_ctxs;
	struct xtm_queue *queue_to_tx;
	struct xtm_queue *queue_from_tx;
	struct fiber *fiber_to_wake_on_shutdown;
	h2o_hostconf_t *hostconf;
#ifndef USE_LIBUV
	h2o_socket_t *sock_from_tx;
#endif /* USE_LIBUV */
#ifdef USE_LIBUV
	uv_loop_t loop;
	uv_poll_t uv_poll_from_tx;
	uv_async_t async;
#else /* USE_LIBUV */
	h2o_loop_t *loop;
	struct {
		int write_fd;
		h2o_socket_t *read_socket;
	} async;
#endif /* USE_LIBUV */
	h2o_linklist_t accepted_sockets;
	httpng_sem_t can_be_terminated;
	unsigned num_connections;
	unsigned idx;
	volatile unsigned active_lua_fibers;
	unsigned listeners_created;
	pthread_t tid;
	bool http_and_tx_lua_handlers_flushed;
	bool shutdown_requested; /* Tarantool asked us to shut down. */
	bool use_graceful_shutdown;
	bool do_not_exit_tx_fiber;
	bool should_notify_tx_done;
	bool tx_done_notification_received;
	bool tx_fiber_should_exit;
	volatile bool tx_fiber_finished;
	volatile bool thread_finished;
#ifndef USE_LIBUV
	bool queue_from_tx_fd_consumed;
#endif /* USE_LIBUV */
} thread_ctx_t;

struct anchor;
typedef struct {
	h2o_req_t *never_access_this_req_from_tx_thread;
	struct anchor *anchor;
	thread_ctx_t *thread_ctx;

	/* never_access_this_req_from_tx_thread can only be used
	 * if disposed is false. */
	char disposed;

	/* For use by handlers, initialized to false for new shuttles. */
	char stopped;

	char unused[sizeof(void *) - 2 * sizeof(char)];

	char payload[];
} shuttle_t;

typedef struct anchor {
	shuttle_t *shuttle;

	/* Can be NULL; it should set shuttle->disposed to true. */
	void (*user_free_shuttle)(shuttle_t *);
} anchor_t;

/* Written directly into h2o_create_handler()->on_req. */
typedef int (*req_handler_t)(h2o_handler_t *, h2o_req_t *);

typedef int (*init_userdata_in_tx_t)(void *); /* Returns 0 on success. */

typedef struct {
	const char *path;
	req_handler_t handler;
	init_userdata_in_tx_t init_userdata_in_tx;
	void *init_userdata_in_tx_param;
} path_desc_t;

typedef struct listener_ctx {
	h2o_accept_ctx_t accept_ctx;
#ifdef USE_LIBUV
	uv_tcp_t uv_tcp_listener;
#else /* USE_LIBUV */
	h2o_socket_t *sock;
#endif /* USE_LIBUV */
	thread_ctx_t *thread_ctx;
	int fd;
} listener_ctx_t;

typedef struct {
	int fd;
	bool is_opened;
} listener_cfg_t;

typedef struct waiter {
	struct waiter *next;
	struct fiber *fiber;
} waiter_t;

typedef struct {
	h2o_handler_t super;
	int lua_handler_ref;
	const char *path;
	size_t path_len;
} lua_h2o_handler_t;

/* N. b.: must be relocatable when server is running
 * (do not store e. g. string buffers here).
 * It is better to not touch it from handlers at all. */
typedef struct {
	char *path;
	lua_h2o_handler_t *(lua_handlers[MAX_threads]);
	h2o_pathconf_t *(pathconfs[MAX_threads]);
	int lua_handler_ref;
	int old_lua_handler_ref;
	int new_lua_handler_ref;
	unsigned path_len;
	unsigned generation;
} lua_site_t;

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
	size_t content_length;
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

/* FIXME: Make it ushort and add sanity checks. */
typedef unsigned header_offset_t;

typedef struct {
	header_offset_t name_size;
	header_offset_t value_size;
} received_http_header_handle_t;

typedef struct {
#ifdef SPLIT_LARGE_BODY
	size_t offset_within_body; /* For use by HTTP server thread. */
#endif /* SPLIT_LARGE_BODY */
	int lua_handler_ref; /* Reference to user Lua handler. */
	unsigned path_len;
	unsigned query_at;
	unsigned num_headers;
	unsigned body_len;
	unsigned char method_len;
	unsigned char ws_client_key_len;
	unsigned char version_major;
	unsigned char version_minor;
#ifdef SPLIT_LARGE_BODY
	bool is_body_incomplete;
#endif /* SPLIT_LARGE_BODY */
	char method[7];
	char buffer[]; /* "path" from h2o_req_t goes first. */
} lua_first_request_only_t;

typedef struct {
	const char *site_path;
	struct fiber *fiber;
	struct fiber *recv_fiber; /* Fiber for WebSocket recv handler. */
	h2o_websocket_conn_t *ws_conn;
	recv_data_t *recv_data; /* For WebSocket recv. */
	struct fiber *tx_fiber; /* The one which services requests
				 * from "our" HTTP server thread. */
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

typedef struct {
	unsigned extra_sites;
} add_site_t;

static struct {
	listener_cfg_t *listener_cfgs;
	thread_ctx_t *thread_ctxs;
	struct fiber **tx_fiber_ptrs;
	SSL_CTX *ssl_ctx;
	lua_site_t *lua_sites;
	unsigned lua_site_count;
	unsigned shuttle_size;
	unsigned recv_data_size;
	unsigned num_listeners;
#ifndef USE_LIBUV
	unsigned num_accepts;
#endif /* USE_LIBUV */
	unsigned max_conn_per_thread;
	unsigned num_desired_threads;
	unsigned num_threads;
	unsigned max_headers_lua;
	unsigned max_path_len_lua;
	unsigned max_recv_bytes_lua_websocket;
	unsigned generation;
	volatile unsigned add_new_sites_counter;
	int tfo_queues;
	int on_shutdown_ref;
	int idx_of_root_site; /* ...in lua_sites; < 0 means none. */
#ifdef SPLIT_LARGE_BODY
	bool use_body_split;
#endif /* SPLIT_LARGE_BODY */
	bool configured;
	bool cfg_in_progress;
	bool hot_reload_in_progress;
	bool is_on_shutdown_setup;
	bool is_shutdown_in_progress;
} conf = {
	.tfo_queues = H2O_DEFAULT_LENGTH_TCP_FASTOPEN_QUEUE,
	.on_shutdown_ref = LUA_REFNIL,
	.idx_of_root_site = -1,
};

__thread thread_ctx_t *curr_thread_ctx;

/* Must be called in HTTP server thread.
 * Should only be called if disposed==true
 * (anchor_dispose() does not set disposed=true for performance reasons).
 * Expected usage: handling disposed==true in postprocessing,
 * setting it as anchor->user_free_shuttle. */
extern void free_shuttle(shuttle_t *);

/* Must be called in HTTP server thread.
 * Should only be called if disposed==false.
 * Expected usage: when req handler can't or wouldn't queue request
 * to TX thread. */
extern void free_shuttle_with_anchor(shuttle_t *);

extern shuttle_t *prepare_shuttle(h2o_req_t *);
static void fill_http_headers(lua_State *L, lua_response_t *response,
	int param_lua_idx);

/* Called when dispatch must not fail */
extern void stubborn_dispatch_uni(struct xtm_queue *queue, void *func,
	void *param);

#ifndef USE_LIBUV
static void on_async_read(h2o_socket_t *sock, const char *err);
static void init_async(thread_ctx_t *thread_ctx);
#endif /* USE_LIBUV */
static void close_async(thread_ctx_t *thread_ctx);
static void async_cb(void *param);
static int on_shutdown_callback(lua_State *L);

static inline void my_xtm_delete_queue_from_tx(thread_ctx_t *thread_ctx)
{
#ifndef USE_LIBUV
	if (thread_ctx->queue_from_tx_fd_consumed)
		xtm_delete_ex(thread_ctx->queue_from_tx);
	else
#endif /* USE_LIBUV */
		xtm_delete(thread_ctx->queue_from_tx);
}

__attribute__((weak))
void complain_loudly_about_leaked_fds(void)
{
}

static inline bool lua_isstring_strict(lua_State *L, int idx)
{
	return lua_type(L, idx) == LUA_TSTRING;
}

static inline void h2o_linklist_insert_fast(h2o_linklist_t *pos,
	h2o_linklist_t *node)
{
    node->prev = pos->prev;
    node->next = pos;
    node->prev->next = node;
    node->next->prev = node;
}

static inline void h2o_linklist_unlink_fast(h2o_linklist_t *node)
{
    node->next->prev = node->prev;
    node->prev->next = node->next;
}

static inline void
xtm_fun_invoke_all(struct xtm_queue *queue)
{
	int rc = xtm_fun_invoke(queue, 1);
	while (rc >= 0 && xtm_msg_count(queue) > 0)
		rc = xtm_fun_invoke(queue, 0);
}

/* Launched in HTTP server thread. */
static inline thread_ctx_t *get_curr_thread_ctx(void)
{
	return curr_thread_ctx;
}

/* Called when dispatch must not fail. */
static inline void stubborn_dispatch(struct xtm_queue *queue,
	void (*func)(shuttle_t *), shuttle_t *shuttle)
{
	stubborn_dispatch_uni(queue, (void *)func, (void *)shuttle);
}

/* FIXME: Use lua_tointegerx() when we would no longer care about
 * older Tarantool versions. */
static inline lua_Integer my_lua_tointegerx(lua_State *L, int idx, int *ok)
{
	return (*ok = lua_isnumber(L, idx)) ? lua_tointeger(L, idx) : 0;
}

static inline shuttle_t *get_shuttle_from_generator_lua(
	h2o_generator_t *generator)
{
	lua_response_t *const response = container_of(generator,
		lua_response_t, un.resp.any.generator);
	return (shuttle_t *)((char *)response - offsetof(shuttle_t, payload));
}

/* Called when dispatch must not fail */
void stubborn_dispatch_uni(struct xtm_queue *queue, void *func, void *param)
{
	while (xtm_fun_dispatch(queue, (void (*)(void*))func, param, 0)) {
		/* Error; we must not fail so retry a little later. */
		fiber_sleep(0);
	}
}

/* Called when dispatch must not fail. */
static inline void stubborn_dispatch_lua(struct xtm_queue *queue,
	void (*func)(lua_response_t *), lua_response_t *param)
{
	stubborn_dispatch_uni(queue, (void *)func, param);
}

/* Called when dispatch must not fail. */
static inline void stubborn_dispatch_recv(struct xtm_queue *queue,
	void (*func)(recv_data_t *), recv_data_t *param)
{
	stubborn_dispatch_uni(queue, (void *)func, param);
}

/* Called when dispatch must not fail. */
static inline void stubborn_dispatch_thr_to_tx(thread_ctx_t *thread_ctx,
	void (*func)(thread_ctx_t *))
{
	stubborn_dispatch_uni(thread_ctx->queue_to_tx,
		(void *)func, thread_ctx);
}

/* Called when dispatch must not fail. */
static inline void stubborn_dispatch_thr_from_tx(
	thread_ctx_t *thread_ctx, void (*func)(thread_ctx_t *))
{
	stubborn_dispatch_uni(thread_ctx->queue_from_tx,
		(void *)func, thread_ctx);
}

/* Called when dispatch must not fail. */
static inline void stubborn_dispatch_to_http_add_site(
	thread_ctx_t *thread_ctx, void (*func)(add_site_t *),
	add_site_t *param)
{
	stubborn_dispatch_uni(thread_ctx->queue_from_tx,
		(void *)func, param);
}

/* Launched in HTTP server thread. */
static inline recv_data_t *alloc_recv_data(void)
{
	/* FIXME: Use per-thread pools? */
	recv_data_t *const recv_data = (recv_data_t *)
		malloc(conf.recv_data_size);
	if (recv_data == NULL)
		h2o_fatal("no memory");
	return recv_data;
}

/* Launched in HTTP server thread. */
static inline recv_data_t *prepare_websocket_recv_data(shuttle_t *parent,
	unsigned payload_bytes)
{
	recv_data_t *const recv_data = alloc_recv_data();
	recv_data->parent_shuttle = parent;
	recv_data->payload_bytes = payload_bytes;
	return recv_data;
}

/* Launched in HTTP server thread or in TX thread when
 * !SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD. */
static void free_shuttle_internal(shuttle_t *shuttle)
{
	assert(shuttle->disposed);
	free_shuttle(shuttle);
}

/* Launched in HTTP server thread or in TX thread when
 * !SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD.
 * FIXME: Only assert is different, can optimize for release build. */
static void free_lua_websocket_shuttle_internal(shuttle_t *shuttle)
{
	assert(!shuttle->disposed);
	free_shuttle(shuttle);
}

/* Launched in TX thread. */
void free_shuttle_from_tx_in_http_thr(shuttle_t *shuttle)
{
	stubborn_dispatch(shuttle->thread_ctx->queue_from_tx,
		&free_shuttle_internal, shuttle);
}

/* Launched in TX thread.
 * It can queue request to HTTP server thread or free everything itself. */
void free_shuttle_from_tx(shuttle_t *shuttle)
{
#ifdef SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD
	/* Can't call free_shuttle() from TX thread because it
	 * [potentially] uses per-thread pools. */
	free_shuttle_from_tx_in_http_thr(shuttle);
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
static inline void free_lua_shuttle_from_tx_in_http_thr(shuttle_t *shuttle)
{
	assert(!((lua_response_t *)&shuttle->payload)->upgraded_to_websocket);
	free_shuttle_from_tx_in_http_thr(shuttle);
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
		luaL_unref(L, LUA_REGISTRYINDEX,
			response->lua_recv_handler_ref);
		luaL_unref(L, LUA_REGISTRYINDEX, response->lua_recv_state_ref);
	}
#ifdef SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD
	/* Can't call free_shuttle() from TX thread because it
	 * [potentially] uses per-thread pools. */
	stubborn_dispatch(shuttle->thread_ctx->queue_from_tx,
		&free_lua_websocket_shuttle_internal, shuttle);
#else /* SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD */
	free_lua_websocket_shuttle_internal(shuttle);
#endif /* SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD */
}

/* Launched in HTTP server thread or in TX thread when
 * !SHOULD_FREE_RECV_DATA_IN_HTTP_SERVER_THREAD. */
static inline void free_recv_data(recv_data_t *recv_data)
{
	free(recv_data);
}

/* Launched in HTTP server thread or in TX thread when
 * !SHOULD_FREE_RECV_DATA_IN_HTTP_SERVER_THREAD. */
static void free_lua_websocket_recv_data_internal(recv_data_t *recv_data)
{
	free_recv_data(recv_data);
}

/* Launched in TX thread. */
static inline void free_lua_websocket_recv_data_from_tx(recv_data_t *recv_data)
{
#ifdef SHOULD_FREE_RECV_DATA_IN_HTTP_SERVER_THREAD
	/* Can't call free_recv_data() from TX thread because it
	 * [potentially] uses per-thread pools w/o mutexes. */
	stubborn_dispatch_recv(recv_data->parent_shuttle->thread_ctx
		->queue_from_tx,
		&free_lua_websocket_recv_data_internal, recv_data);
#else /* SHOULD_FREE_RECV_DATA_IN_HTTP_SERVER_THREAD */
	free_lua_websocket_recv_data_internal(recv_data);
#endif /* SHOULD_FREE_RECV_DATA_IN_HTTP_SERVER_THREAD */
}

/* Launched in TX thread. */
static void cancel_processing_lua_req_in_tx(shuttle_t *shuttle)
{
	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	assert(!response->upgraded_to_websocket);

	/* We do not use fiber_cancel() because it causes exception
	 * in Lua code so Lua handler have to use pcall() and even
	 * that is not 100% guarantee because such exception
	 * can theoretically happen before pcall().
	 * Also we have unref Lua state.
	 * Shuttle must be freed from HTTP thread because
	 * it can be already queued. */
	if (response->fiber == NULL)
		free_lua_shuttle_from_tx_in_http_thr(shuttle);
	else if (response->waiter != NULL) {
		assert(!response->fiber_done);
		response->cancelled = true;
		fiber_wakeup(response->waiter->fiber);
	} else if (response->fiber_done)
		free_lua_shuttle_from_tx_in_http_thr(shuttle);
	else
		response->cancelled = true;
		; /* Fiber would clean up because we have set cancelled=true */
}

/* Launched in HTTP server thread. */
static void free_shuttle_lua(shuttle_t *shuttle)
{
	lua_response_t *const response = (lua_response_t *)(&shuttle->payload);
	if (!response->upgraded_to_websocket) {
		shuttle->disposed = true;
		stubborn_dispatch(get_curr_thread_ctx()->queue_to_tx,
			&cancel_processing_lua_req_in_tx, shuttle);
	}
}

/* Launched in TX thread. */
static void continue_processing_lua_req_in_tx(lua_response_t *response)
{
	assert(response->fiber != NULL);
	assert(!response->fiber_done);
	assert(response->waiter != NULL);
	assert(response->waiter->fiber != NULL);
	fiber_wakeup(response->waiter->fiber);
}

/* Launched in HTTP server thread when H2O has sent everything
 * and asks for more. */
static void proceed_sending_lua(h2o_generator_t *self, h2o_req_t *req)
{
	shuttle_t *const shuttle = get_shuttle_from_generator_lua(self);
	thread_ctx_t *const thread_ctx = get_curr_thread_ctx();
	stubborn_dispatch_lua(thread_ctx->queue_to_tx,
		continue_processing_lua_req_in_tx,
		(lua_response_t *)&shuttle->payload);
}

static inline void send_lua(h2o_req_t *req, lua_response_t *const response)
{
	h2o_iovec_t buf;
	buf.base = (char *)response->un.resp.any.payload;
	buf.len = response->un.resp.any.payload_len;
	h2o_send(req, &buf, 1, response->un.resp.any.is_last_send
		? H2O_SEND_STATE_FINAL : H2O_SEND_STATE_IN_PROGRESS);
}

/* Launched in HTTP server thread to postprocess first response
 * (with HTTP headers). */
static void postprocess_lua_req_first(shuttle_t *shuttle)
{
	lua_response_t *const response = (lua_response_t *)(&shuttle->payload);
	if (shuttle->disposed)
		return;
	h2o_req_t *const req = shuttle->never_access_this_req_from_tx_thread;
	req->res.status = response->un.resp.first.http_code;
	req->res.reason = "OK"; /* FIXME: Customizable? */
	const unsigned num_headers = response->un.resp.first.num_headers;
	unsigned header_idx;
	for (header_idx = 0; header_idx < num_headers; ++header_idx) {
		const http_header_entry_t *const header =
			&response->un.resp.first.headers[header_idx];
		h2o_add_header_by_str(&req->pool, &req->res.headers,
			header->name, header->name_len,

			/* FIXME: Should benchmark whether this
			 * faster than 0. */
			1,

			NULL, /* FIXME: Do we need orig_name? */
			header->value, header->value_len);
	}

	response->un.resp.any.generator = (h2o_generator_t){
		proceed_sending_lua,

		/* Do not use stop_sending, we handle everything
		 * in free_shuttle_lua(). */
		NULL
	};
	req->res.content_length = response->un.resp.first.content_length;
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

static inline void add_http_header_to_lua_response(
	lua_first_response_only_t *response, const char *key, size_t key_len,
	const char *value, size_t value_len)
{
	if (response->num_headers >= conf.max_headers_lua)
		/* FIXME: Misconfiguration, should we log something? */
		return;

	response->headers[response->num_headers++] = (http_header_entry_t)
		{key, value, (unsigned)key_len, (unsigned)value_len};
}

/* Launched in TX thread.
 * Makes sure earlier queued sends to HTTP server thread are done. */
static void take_shuttle_ownership_lua(lua_response_t *response)
{
	if (response->waiter == NULL)
		return;

	/* Other fiber(s) are already waiting for shuttle return or taking
	 * ownership, add ourself into tail of waiting list. */
	waiter_t waiter = { .next = NULL, .fiber = fiber_self() };
	waiter_t *last_waiter = response->waiter;
	/* FIXME: It may be more efficient to use double-linked list
	 * if we expect a lot of competing fibers. */
	while (last_waiter->next != NULL)
		last_waiter = last_waiter->next;
	last_waiter->next = &waiter;
	fiber_yield();
}

/* Launched in TX thread.
 * Caller must call take_shuttle_ownership_lua() before filling in shuttle
 * and calling us. */
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

/* Launched in TX thread. */
static int payload_writer_write(lua_State *L)
{
	/* Lua parameters: self, payload, is_last. */
	const unsigned num_params = lua_gettop(L);
	if (num_params < 2)
		return luaL_error(L, "Not enough parameters");

	lua_getfield(L, 1, "shuttle");
	if (!lua_islightuserdata(L, -1))
		return luaL_error(L, "shuttle is invalid");
	shuttle_t *const shuttle = (shuttle_t *)lua_touserdata(L, -1);

	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	take_shuttle_ownership_lua(response);
	if (response->cancelled) {
		/* Returning Lua true because connection has already
		 * been closed. */
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

	response->un.resp.any.is_last_send = is_last;
	if (!response->sent_something) {
		response->un.resp.first.http_code =
			get_default_http_code(response);

		lua_getfield(L, 1, "headers");
		const unsigned headers_lua_index = num_params + 1 + 1;
		fill_http_headers(L, response, headers_lua_index);

		response->sent_something = true;
		stubborn_dispatch(shuttle->thread_ctx->queue_from_tx,
			&postprocess_lua_req_first, shuttle);
	} else
		stubborn_dispatch(shuttle->thread_ctx->queue_from_tx,
			&postprocess_lua_req_others, shuttle);
	wait_for_lua_shuttle_return(response);

	/* Returning Lua true if connection has already been closed. */
	lua_pushboolean(L, response->cancelled);
	return 1;
}

/* Launched in TX thread. */
static void fill_http_headers(lua_State *L, lua_response_t *response,
	int param_lua_idx)
{
	response->un.resp.first.content_length =
		H2O_CONTENT_LENGTH_UNSPECIFIED;
	if (lua_isnil(L, param_lua_idx))
		return;

	lua_pushnil(L); /* Start of table. */
	while (lua_next(L, param_lua_idx)) {
		size_t key_len;
		size_t value_len;
		const char *const key = lua_tolstring(L, -2, &key_len);
		const char *const value = lua_tolstring(L, -1, &value_len);

		static const char content_length_str[] = "content-length";
		char temp[32];
		if (key_len == sizeof(content_length_str) - 1 &&
		    !strncasecmp(key, content_length_str, key_len) &&
		    value_len < sizeof(temp)) {
			memcpy(temp, value, value_len);
			temp[value_len] = 0;
			errno = 0;
			const long long candidate = strtoll(temp, NULL, 10);
			if (errno)
				add_http_header_to_lua_response(
					&response->un.resp.first,
					key, key_len, value, value_len);
			else
				/* h2o would add this header
				 * and disable chunked. */
				response->un.resp.first.content_length =
					candidate;
		} else
			add_http_header_to_lua_response(
				&response->un.resp.first,
				key, key_len, value, value_len);

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
		return luaL_error(L, "Not enough parameters");

	lua_getfield(L, 1, "shuttle");

	if (!lua_islightuserdata(L, -1))
		return luaL_error(L, "shuttle is invalid");
	shuttle_t *const shuttle = (shuttle_t *)lua_touserdata(L, -1);

	bool is_last;
	if (num_params >= 5)
		is_last	= lua_toboolean(L, 5);
	else
		is_last = false;

	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	take_shuttle_ownership_lua(response);
	if (response->sent_something)
		return luaL_error(L, "Handler has already written header");
	if (response->cancelled) {
		/* Can't send anything, connection has been closed.
		 * Returning Lua true because connection has already
		 * been closed. */
		lua_pushboolean(L, true);
		return 1;
	}

	int is_integer;
	response->un.resp.first.http_code =
		my_lua_tointegerx(L, 2, &is_integer);
	if (!is_integer)
		return luaL_error(L, "HTTP code is not an integer");

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
		response->un.resp.any.payload =
			lua_tolstring(L, 4, &payload_len);
		response->un.resp.any.payload_len = payload_len;
	} else
		response->un.resp.any.payload_len = 0;

	response->un.resp.any.is_last_send = is_last;
	response->sent_something = true;
	stubborn_dispatch(shuttle->thread_ctx->queue_from_tx,
		&postprocess_lua_req_first, shuttle);
	wait_for_lua_shuttle_return(response);

	/* Returning Lua true if connection has already been closed. */
	lua_pushboolean(L, response->cancelled);
	return 1;
}

/* Launched in TX thread. */
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
			fprintf(stderr, "User WebSocket recv handler for "
				"\"\%s\" is NOT allowed to yield, data has "
				"been lost\n", response->site_path);
	} else
		free_lua_websocket_recv_data_from_tx(recv_data);
}

/* Launched in HTTP server thread. */
static void websocket_msg_callback(h2o_websocket_conn_t *conn,
	const struct wslay_event_on_msg_recv_arg *arg)
{
	shuttle_t *const shuttle = (shuttle_t*)conn->data;
	if (arg == NULL) {
		lua_response_t *const response =
			(lua_response_t *)&shuttle->payload;
		assert(conn == response->ws_conn);
		h2o_websocket_close(conn);
		response->ws_conn = NULL;
		stubborn_dispatch_lua(get_curr_thread_ctx()->queue_to_tx,
			cancel_processing_lua_websocket_in_tx, response);
		return;
	}

	if (wslay_is_ctrl_frame(arg->opcode))
		return;

	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	size_t bytes_remain = arg->msg_length;
	const unsigned char *pos = arg->msg;
	while (1) {
		/* FIXME: Need flag about splitting to parts.
		 * Probably should have upper limit on a number of active
		 * recv_data - we can eat A LOT of memory. */
		const unsigned bytes_to_send = bytes_remain >
			conf.max_recv_bytes_lua_websocket
			? conf.max_recv_bytes_lua_websocket : bytes_remain;
		recv_data_t *const recv_data =
			prepare_websocket_recv_data(shuttle, bytes_to_send);
		memcpy(get_websocket_recv_location(recv_data), pos,
			bytes_to_send);
		stubborn_dispatch_recv(get_curr_thread_ctx()->queue_to_tx,
			process_lua_websocket_received_data_in_tx, recv_data);
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
		const http_header_entry_t *const header =
			&response->un.resp.first.headers[header_idx];
		h2o_add_header_by_str(&req->pool, &req->res.headers,
			header->name, header->name_len,
			1, /* FIXME: Benchmark whether this faster than 0. */
			NULL, /* FIXME: Do we need orig_name? */
			header->value, header->value_len);

	}
	response->upgraded_to_websocket = true;
	response->ws_conn = h2o_upgrade_to_websocket(req,
		response->ws_client_key, shuttle, websocket_msg_callback);
	/* anchor_dispose()/free_shuttle_lua() will be called by h2o. */
	stubborn_dispatch_lua(get_curr_thread_ctx()->queue_to_tx,
		continue_processing_lua_req_in_tx, response);
}

/* Launched in HTTP server thread. */
static void postprocess_lua_req_websocket_send_text(lua_response_t *response)
{
	/* Do not check shuttle->disposed, this is a WebSocket now. */

	struct wslay_event_msg msgarg = {
		.opcode = WSLAY_TEXT_FRAME,
		.msg = (unsigned char *)response->un.resp.any.payload,
		.msg_length = response->un.resp.any.payload_len,
	};
	if (wslay_event_queue_msg(response->ws_conn->ws_ctx, &msgarg) ||
	    wslay_event_send(response->ws_conn->ws_ctx))
		response->ws_send_failed = true;
	stubborn_dispatch_lua(get_curr_thread_ctx()->queue_to_tx,
		continue_processing_lua_req_in_tx, response);
}

/* Launched in TX thread */
static int websocket_send_text(lua_State *L)
{
	/* Lua parameters: self, payload. */
	const unsigned num_params = lua_gettop(L);
	if (num_params < 2)
		return luaL_error(L, "Not enough parameters");

	lua_getfield(L, 1, "shuttle");
	if (!lua_islightuserdata(L, -1))
		return luaL_error(L, "shuttle is invalid");
	shuttle_t *const shuttle = (shuttle_t *)lua_touserdata(L, -1);

	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	if (response->in_recv_handler) {
		return luaL_error(L, "User WebSocket recv handler for "
			"\"%s\" is NOT allowed to call yielding functions",
			response->site_path);
	}
	take_shuttle_ownership_lua(response);
	if (response->cancelled || response->ws_send_failed) {
		/* Returning Lua true because connection has already
		 * been closed or previous send failed. */
		lua_pushboolean(L, true);
		return 1;
	}

	size_t payload_len;
	response->un.resp.any.payload = lua_tolstring(L, 2, &payload_len);
	response->un.resp.any.payload_len = payload_len;

	stubborn_dispatch_lua(shuttle->thread_ctx->queue_from_tx,
		&postprocess_lua_req_websocket_send_text, response);
	wait_for_lua_shuttle_return(response);

	/* Returning Lua true if send failed or connection has already
	 * been closed. */
	lua_pushboolean(L, response->ws_send_failed || response->cancelled);
	return 1;
}

/* Launched in HTTP server thread. */
static void close_websocket(lua_response_t *const response)
{
	if (response->ws_conn != NULL) {
		h2o_websocket_close(response->ws_conn);
		response->ws_conn = NULL;
	}
	stubborn_dispatch_lua(get_curr_thread_ctx()->queue_to_tx,
		continue_processing_lua_req_in_tx, response);
}

/* Launched in TX thread. */
static int close_lua_websocket(lua_State *L)
{
	/* Lua parameters: self. */
	const unsigned num_params = lua_gettop(L);
	if (num_params < 1)
		return luaL_error(L, "Not enough parameters");

	lua_getfield(L, 1, "shuttle");
	if (!lua_islightuserdata(L, -1))
		return luaL_error(L, "shuttle is invalid");
	shuttle_t *const shuttle = (shuttle_t *)lua_touserdata(L, -1);

	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	if (response->in_recv_handler) {
		return luaL_error(L, "User WebSocket recv handler for "
			"\"%s\" is NOT allowed to call yielding functions",
			response->site_path);
	}
	take_shuttle_ownership_lua(response);
	if (response->cancelled)
		return 0;

	response->cancelled = true;
	stubborn_dispatch_lua(shuttle->thread_ctx->queue_from_tx,
		&close_websocket, response);
	wait_for_lua_shuttle_return(response);
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
		if (response->is_recv_fiber_cancelled) {
			/* FIXME: Can we leak recv_data? */
			fiber_wakeup(response->fiber);
			return 0;
		}
		response->is_recv_fiber_waiting = false;

		/* User handler function, written in Lua. */
		lua_rawgeti(L, LUA_REGISTRYINDEX,
			response->lua_recv_handler_ref);

		recv_data_t *const recv_data = response->recv_data;
		assert(recv_data->parent_shuttle == shuttle);
		/* First param for Lua WebSocket recv handler - data. */
		lua_pushlstring(L, get_websocket_recv_location(recv_data),
			recv_data->payload_bytes);

		/* N. b.: WebSocket recv handler is NOT allowed to yield. */
		response->in_recv_handler = true;
		if (lua_pcall(L, 1, 0, 0) != LUA_OK)
			/* FIXME: Should probably log this instead(?).
			 * Should we stop calling handler? */
			fprintf(stderr, "User WebSocket recv handler for "
				"\"\%s\" failed with error \"%s\"\n",
				response->site_path, lua_tostring(L, -1));
		response->in_recv_handler = false;
		free_lua_websocket_recv_data_from_tx(recv_data);
		fiber_wakeup(response->tx_fiber);
	}

	return 0;
}

/* Launched in TX thread. */
static int header_writer_upgrade_to_websocket(lua_State *L)
{
	/* Lua parameters: self, headers, recv_function. */
	const unsigned num_params = lua_gettop(L);
	if (num_params < 1)
		return luaL_error(L, "Not enough parameters");

	lua_getfield(L, 1, "shuttle");
	if (!lua_islightuserdata(L, -1))
		return luaL_error(L, "shuttle is invalid");
	shuttle_t *const shuttle = (shuttle_t *)lua_touserdata(L, -1);

	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	take_shuttle_ownership_lua(response);
	if (response->sent_something)
		return luaL_error(L, "Unable to upgrade to WebSockets "
			"after sending HTTP headers");
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
			const char *const value =
				lua_tolstring(L, -1, &value_len);

			add_http_header_to_lua_response(
					&response->un.resp.first,
					key, key_len, value, value_len);

			/* Remove value, keep key for next iteration. */
			lua_pop(L, 1);
		}
	}

	if (num_params != 3 || lua_isnil(L, 3) ||
	    lua_type(L, 3) != LUA_TFUNCTION)
		response->recv_fiber = NULL;
	else {
		lua_pop(L, 1);
		response->lua_recv_handler_ref =
			luaL_ref(L, LUA_REGISTRYINDEX);
		struct lua_State *const new_L = lua_newthread(L);
		response->lua_recv_state_ref = luaL_ref(L, LUA_REGISTRYINDEX);
		if ((response->recv_fiber =
		    fiber_new("HTTP Lua WebSocket recv fiber",
			    &lua_websocket_recv_fiber_func)) == NULL) {
			luaL_unref(L, LUA_REGISTRYINDEX,
				response->lua_recv_handler_ref);
			luaL_unref(L, LUA_REGISTRYINDEX,
				response->lua_recv_state_ref);
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
	stubborn_dispatch(shuttle->thread_ctx->queue_from_tx,
		&postprocess_lua_req_upgrade_to_websocket, shuttle);
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
}

#ifdef SPLIT_LARGE_BODY
/* Launched in HTTP server thread. */
static void retrieve_more_body(shuttle_t *const shuttle)
{
	if (shuttle->disposed)
		return;
	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	assert(response->un.req.is_body_incomplete);
	const h2o_req_t *const req =
		shuttle->never_access_this_req_from_tx_thread;
	assert(response->un.req.offset_within_body < req->entity.len);
	const size_t bytes_still_in_req =
		req->entity.len - response->un.req.offset_within_body;
	unsigned bytes_to_copy;
	const unsigned offset = response->un.req.offset_within_body;
	if (bytes_still_in_req > conf.max_path_len_lua) {
		bytes_to_copy = conf.max_path_len_lua;
		response->un.req.offset_within_body += bytes_to_copy;
	} else {
		bytes_to_copy = bytes_still_in_req;
		response->un.req.is_body_incomplete = false;
	}
	response->un.req.body_len = bytes_to_copy;
	memcpy(&response->un.req.buffer, &req->entity.base[offset],
		bytes_to_copy);

	stubborn_dispatch_lua(get_curr_thread_ctx()->queue_to_tx,
		continue_processing_lua_req_in_tx, response);
}
#endif /* SPLIT_LARGE_BODY */

/* Launched in TX thread.
 * Returns !0 in case of error. */
static inline int fill_received_headers_and_body(lua_State *L,
	shuttle_t *shuttle)
{
	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	assert(!response->sent_something);
	const received_http_header_handle_t *const handles =
		(received_http_header_handle_t *)&response->un.req.buffer[
			response->un.req.path_len];
	const unsigned num_headers = response->un.req.num_headers;
	lua_createtable(L, 0, num_headers);
	unsigned current_offset = response->un.req.path_len + num_headers *
		sizeof(received_http_header_handle_t);
	unsigned header_idx;
	for (header_idx = 0; header_idx < num_headers; ++header_idx) {
		const received_http_header_handle_t *const handle =
			&handles[header_idx];
		lua_pushlstring(L, &response->un.req.buffer[current_offset +
			handle->name_size + 1], handle->value_size);

		/* N. b.: it must be NULL-terminated. */
		lua_setfield(L, -2, &response->un.req.buffer[current_offset]);

		current_offset += handle->name_size + 1 + handle->value_size;
	}
	lua_setfield(L, -2, "headers");
#ifdef SPLIT_LARGE_BODY
	if (!response->un.req.is_body_incomplete)
#endif /* SPLIT_LARGE_BODY */
	{
		lua_pushlstring(L, &response->un.req.buffer[current_offset],
			response->un.req.body_len);
		lua_setfield(L, -2, "body");
		return 0;
	}

#ifdef SPLIT_LARGE_BODY
	/* FIXME: Should use content-length to preallocate enough memory and
	 * avoid allocations and copying. Or we can just allocate in
	 * HTTP server thread and pass pointer. */
	char *body_buf = (char *)malloc(response->un.req.body_len);
	if (body_buf == NULL)
		/* There was memory allocation failure.
		 * FIXME: Should log this. */
		return 1;

	memcpy(body_buf, &response->un.req.buffer[current_offset],
		response->un.req.body_len);

	size_t body_offset = response->un.req.body_len;
	do {
		/* FIXME: Not needed on first iteration. */
		take_shuttle_ownership_lua(response);

		stubborn_dispatch(shuttle->thread_ctx
			->queue_from_tx, &retrieve_more_body, shuttle);
		wait_for_lua_shuttle_return(response);
		if (response->cancelled) {
			free(body_buf);
			return 1;
		}

		{
			char *const new_body_buf = (char *)realloc(body_buf,
				body_offset + response->un.req.body_len);
			if (new_body_buf == NULL) {
				free(body_buf);
				/* There was memory allocation failure.
				 * FIXME: Should log this. */
				return 1;
			}
			body_buf = new_body_buf;
		}
		memcpy(&body_buf[body_offset], &response->un.req.buffer,
			response->un.req.body_len);
		body_offset += response->un.req.body_len;
	} while (response->un.req.is_body_incomplete);

	lua_pushlstring(L, body_buf, body_offset);
	free(body_buf);
	lua_setfield(L, -2, "body");
	return 0;
#endif /* SPLIT_LARGE_BODY */
}

/* Launched in TX thread. */
static int close_lua_req(lua_State *L)
{
	/* Lua parameters: self. */
	const unsigned num_params = lua_gettop(L);
	if (num_params < 1)
		return luaL_error(L, "Not enough parameters");

	lua_getfield(L, 1, "shuttle");
	if (!lua_islightuserdata(L, -1))
		return luaL_error(L, "shuttle is invalid");
	shuttle_t *const shuttle = (shuttle_t *)lua_touserdata(L, -1);
	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	take_shuttle_ownership_lua(response);
	if (response->cancelled)
		return 0;

	response->un.resp.any.payload_len = 0;
	response->un.resp.any.is_last_send = true;
	if (response->sent_something)
		stubborn_dispatch(shuttle->thread_ctx->queue_from_tx,
			&postprocess_lua_req_others, shuttle);
	else {
		response->un.resp.first.http_code =
			get_default_http_code(response);
		response->sent_something = true;
		response->un.resp.first.content_length =
			H2O_CONTENT_LENGTH_UNSPECIFIED;
		stubborn_dispatch(shuttle->thread_ctx->queue_from_tx,
			&postprocess_lua_req_first, shuttle);
	}
	wait_for_lua_shuttle_return(response);
	return 0;
}


/* Launched in TX thread. */
static inline void process_handler_failure_not_ws(shuttle_t *shuttle)
{
	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	take_shuttle_ownership_lua(response);
	if (response->cancelled) {
		/* There would be no more calls from HTTP server thread,
		 * must clean up. */
		free_lua_shuttle_from_tx(shuttle);
		return;
	}

	response->un.resp.any.is_last_send = true;
	if (response->sent_something) {
		/* Do not add anything to user output to prevent
		 * corrupt HTML etc. */
		response->un.resp.any.payload_len = 0;
		stubborn_dispatch(shuttle->thread_ctx->queue_from_tx,
			&postprocess_lua_req_others, shuttle);
	} else {
		static const char key[] = "content-type";
		static const char value[] = "text/plain; charset=utf-8";
		add_http_header_to_lua_response(&response->un.resp.first, key,
			sizeof(key) - 1, value, sizeof(value) - 1);
		static const char error_str[] = "Path handler execution error";
		response->un.resp.first.http_code = 500;
		response->un.resp.any.payload = error_str;
		response->un.resp.any.payload_len = sizeof(error_str) - 1;
		response->un.resp.first.content_length = sizeof(error_str) - 1;
		/* Not setting sent_something because no one would check it. */
		stubborn_dispatch(shuttle->thread_ctx->queue_from_tx,
			&postprocess_lua_req_first, shuttle);
	}
	wait_for_lua_shuttle_return(response);
	if (response->cancelled)
		/* There would be no more calls from HTTP server thread,
		 * must clean up. */
		free_lua_shuttle_from_tx(shuttle);
	else
		response->fiber_done = true;
		/* cancel_processing_lua_req_in_tx() is not yet called,
		 * it would clean up because we have set fiber_done=true. */
}


/* Launched in TX thread. */
static inline void process_handler_success_not_ws_with_send(lua_State *L,
	shuttle_t *shuttle)
{
	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	const bool old_sent_something = response->sent_something;
	if (!old_sent_something) {
		lua_getfield(L, -1, "status");
		if (lua_isnil(L, -1))
			response->un.resp.first.http_code =
				get_default_http_code(response);
		else {
			int is_integer;
			response->un.resp.first.http_code =
				my_lua_tointegerx(L, -1, &is_integer);
			if (!is_integer)
				response->un.resp.first.http_code =
					get_default_http_code(response);
		}
		lua_getfield(L, -2, "headers");
		fill_http_headers(L, response, lua_gettop(L));
		lua_pop(L, 2); /* headers, status. */
		response->sent_something = true;
	}

	lua_getfield(L, -1, "body");
	if (!lua_isnil(L, -1)) {
		size_t payload_len;
		response->un.resp.any.payload =
			lua_tolstring(L, -1, &payload_len);
		response->un.resp.any.payload_len = payload_len;
	} else
		response->un.resp.any.payload_len = 0;

	response->un.resp.any.is_last_send = true;
	take_shuttle_ownership_lua(response);
	if (response->cancelled) {
		/* There would be no more calls from HTTP server
		 * thread, must clean up. */
		if (response->upgraded_to_websocket)
			free_lua_websocket_shuttle_from_tx(shuttle);
		else
			free_lua_shuttle_from_tx(shuttle);
		return;
	}

	if (old_sent_something)
		stubborn_dispatch(shuttle->thread_ctx->queue_from_tx,
			postprocess_lua_req_others, shuttle);
	else {
		response->un.resp.first.content_length =
			response->un.resp.any.payload_len;
		stubborn_dispatch(shuttle->thread_ctx->queue_from_tx,
			postprocess_lua_req_first, shuttle);
	}
	wait_for_lua_shuttle_return(response);
	if (response->cancelled) {
		/* There would be no more calls from HTTP
		 * server thread, must clean up. */
		if (response->upgraded_to_websocket)
			free_lua_websocket_shuttle_from_tx(shuttle);
		else
			free_lua_shuttle_from_tx(shuttle);
	} else
		response->fiber_done = true;
		/* cancel_processing_lua_req_in_tx() is not yet called,
		 * it would clean up because we have set fiber_done=true */
}


/* Launched in TX thread. */
static int get_query(lua_State *L)
{
	/* Lua parameters: self. */
	const unsigned num_params = lua_gettop(L);
	if (num_params < 1)
		return luaL_error(L, "Not enough parameters");

	/* Do not extract data from shuttle -
	 * they may have already been overwritten. */

	lua_getfield(L, -1, "query_at");
	if (!lua_isnumber(L, -1))
		return luaL_error(L, "query_at is not an integer");
	const int64_t query_at = lua_tointeger(L, -1);
	if (-1 == query_at) {
		lua_pushnil(L);
		return 1;
	}
	lua_getfield(L, -2, "path");
	if (!lua_isstring_strict(L, -1))
		return luaL_error(L, "path is not a string");

	size_t len;
	const char *path = lua_tolstring(L, -1, &len);
	if ((uint64_t)query_at - 1 > (uint64_t)len)
		return luaL_error(L, "query_at value is invalid");

	/* N.b.: query_at is 1-based; we also skip '?'. */
	lua_pushlstring(L, path + query_at, len - query_at);
	return 1;
}

/* Launched in TX thread. */
static void exit_tx_fiber(thread_ctx_t *thread_ctx)
{
	thread_ctx->tx_fiber_should_exit = true;
}

/* Launched in HTTP server thread. */
static void tx_done(thread_ctx_t *thread_ctx)
{
#ifdef USE_LIBUV
	uv_stop(&thread_ctx->loop);
#endif /* USE_LIBUV */
	thread_ctx->tx_done_notification_received = true;
	if (thread_ctx->do_not_exit_tx_fiber)
		return;
	stubborn_dispatch_thr_to_tx(thread_ctx, exit_tx_fiber);
}

/* Launched in TX thread. */
static int
lua_fiber_func(va_list ap)
{
	shuttle_t *const shuttle = va_arg(ap, shuttle_t *);
	lua_State *const L = va_arg(ap, lua_State *);

	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	thread_ctx_t *const thread_ctx = shuttle->thread_ctx;

	/* User handler function, written in Lua. */
	lua_rawgeti(L, LUA_REGISTRYINDEX, response->un.req.lua_handler_ref);

	/* First param for Lua handler - req. */
	lua_createtable(L, 0, 7);
	lua_pushinteger(L, response->un.req.version_major);
	lua_setfield(L, -2, "version_major");
	lua_pushinteger(L, response->un.req.version_minor);
	lua_setfield(L, -2, "version_minor");
	lua_pushlstring(L, response->un.req.buffer, response->un.req.path_len);
	lua_setfield(L, -2, "path");

	/* Lua indexes start from 1. */
	lua_pushinteger(L, (response->un.req.query_at == LUA_QUERY_NONE)
		? -1 : (response->un.req.query_at + 1));

	lua_setfield(L, -2, "query_at");
	lua_pushcfunction(L, get_query);
	lua_setfield(L, -2, "query");

	lua_pushlstring(L, response->un.req.method,
		response->un.req.method_len);
	lua_setfield(L, -2, "method");
	lua_pushboolean(L, !!response->un.req.ws_client_key_len);
	lua_setfield(L, -2, "is_websocket");
	const int lua_state_ref = response->lua_state_ref;
	if (fill_received_headers_and_body(L, shuttle)) {
		/* We can get cancellation notification,
		 * can't safely free shuttle in this thread. */
		free_lua_shuttle_from_tx_in_http_thr(shuttle);
		goto Done;
	}

	/* We have finished parsing request, now can write to response
	 * (it is union). */
	response->un.resp.first.num_headers = 0;

	/* Second param for Lua handler - io. */
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

	if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
		/* FIXME: Should probably log this instead(?) */
		fprintf(stderr, "User handler for \"%s\" failed with error "
			"\"%s\"\n", response->site_path, lua_tostring(L, -1));

		if (response->cancelled) {
			/* No point trying to send something, connection
			 * has already been closed.
			 * There would be no more calls from HTTP server
			 * thread, must clean up. */
			if (response->upgraded_to_websocket)
				free_lua_websocket_shuttle_from_tx(shuttle);
			else
				free_lua_shuttle_from_tx_in_http_thr(shuttle);
		} else if (response->upgraded_to_websocket) {
			take_shuttle_ownership_lua(response);
			stubborn_dispatch_lua(thread_ctx->queue_from_tx,
				&close_websocket, response);
			wait_for_lua_shuttle_return(response);
			free_lua_websocket_shuttle_from_tx(shuttle);
		} else {
			process_handler_failure_not_ws(shuttle);
		}
	} else if (response->cancelled) {
		/* There would be no more calls from HTTP server thread,
		 * must clean up. */
		if (response->upgraded_to_websocket)
			free_lua_websocket_shuttle_from_tx(shuttle);
		else
			free_lua_shuttle_from_tx_in_http_thr(shuttle);
	} else if (response->upgraded_to_websocket) {
		take_shuttle_ownership_lua(response);
		assert(!response->cancelled);
		stubborn_dispatch_lua(thread_ctx->queue_from_tx,
			&close_websocket, response);
		wait_for_lua_shuttle_return(response);
		free_lua_websocket_shuttle_from_tx(shuttle);
	} else if (lua_isnil(L, -1))
		response->fiber_done = true;
		/* cancel_processing_lua_req_in_tx() is not yet called,
		 * it would clean up because we have set fiber_done=true. */
	else
		process_handler_success_not_ws_with_send(L, shuttle);

Done:
	luaL_unref(luaT_state(), LUA_REGISTRYINDEX, lua_state_ref);
	if (--thread_ctx->active_lua_fibers == 0 &&
	    thread_ctx->should_notify_tx_done)
		stubborn_dispatch_thr_from_tx(thread_ctx, &tx_done);

	return 0;
}

/* Launched in TX thread. */
static void process_lua_req_in_tx(shuttle_t *shuttle)
{
	lua_response_t *const response = (lua_response_t *)&shuttle->payload;

#define RETURN_WITH_ERROR(err) \
	do { \
		luaL_unref(L, LUA_REGISTRYINDEX, response->lua_state_ref); \
		static const char key[] = "content-type"; \
		static const char value[] = "text/plain; charset=utf-8"; \
		response->un.resp.first.num_headers = 0; \
		add_http_header_to_lua_response(&response->un.resp.first, \
			key, sizeof(key) - 1, value, sizeof(value) - 1); \
		static const char error_str[] = err; \
		response->un.resp.any.is_last_send = true; \
		response->un.resp.first.http_code = 500; \
		response->un.resp.any.payload = error_str; \
		response->un.resp.any.payload_len = sizeof(error_str) - 1; \
		/* Not setting sent_something */ \
		/* because no one would check it. */ \
		response->un.resp.first.content_length = \
			sizeof(error_str) - 1; \
		stubborn_dispatch(shuttle->thread_ctx->queue_from_tx, \
			&postprocess_lua_req_first, shuttle); \
		return; \
	} while (0)

	struct lua_State *const L = luaT_state();
	struct lua_State *const new_L = lua_newthread(L);
	response->lua_state_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	if ((response->fiber = fiber_new("HTTP Lua fiber", &lua_fiber_func))
	    == NULL)
		RETURN_WITH_ERROR("Failed to create fiber");
	response->fiber_done = false;
	response->tx_fiber = fiber_self();
	++shuttle->thread_ctx->active_lua_fibers;
	fiber_start(response->fiber, shuttle, new_L);
}
#undef RETURN_WITH_ERROR

/* Launched in HTTP server thread */
static int lua_req_handler(lua_h2o_handler_t *self, h2o_req_t *req)
{
	shuttle_t *const shuttle = prepare_shuttle(req);
	lua_response_t *const response = (lua_response_t *)&shuttle->payload;
	if ((response->un.req.method_len = req->method.len) >
	    sizeof(response->un.req.method)) {
		/* Error. */
		free_shuttle_with_anchor(shuttle);
		req->res.status = 500;
		req->res.reason = "Method name is too long";
		h2o_send_inline(req, H2O_STRLIT("Method name is too long\n"));
		return 0;
	}
	if ((response->un.req.path_len = req->path.len) >
	    conf.max_path_len_lua) {
		/* Error. */
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
		memcpy(response->ws_client_key, ws_client_key,
			response->un.req.ws_client_key_len);
	}

	memcpy(response->un.req.method, req->method.base,
		response->un.req.method_len);
	memcpy(response->un.req.buffer, req->path.base,
		response->un.req.path_len);

	STATIC_ASSERT(LUA_QUERY_NONE <
		(1ULL << (8 * sizeof(response->un.req.query_at))),
		".query_at field is not large enough to store LUA_QUERY_NONE");
	response->un.req.query_at = (req->query_at == SIZE_MAX)
		? LUA_QUERY_NONE : req->query_at;
	response->un.req.version_major = req->version >> 8;
	response->un.req.version_minor = req->version & 0xFF;

	const h2o_header_t *const headers = req->headers.entries;
	const size_t num_headers = req->headers.size;
	unsigned current_offset = response->un.req.path_len;

	/* response->un.req.buffer[] format:
	 *
	 * char path[req->path.len]
	 * FIXME: Alignment to at least to header_offset_t should be here
	 *   (compatibility/performance).
	 * received_http_header_handle_t handles[num_headers]
	 * {repeat num_headers times} char name[handles[i].name_size], '\0',
	 *   char value[handles[i].value_size]
	 * char body[]
	 *
	 * '\0' is for lua_setfield().
	 * */
	const unsigned max_offset = conf.max_path_len_lua;
	const size_t headers_size = num_headers *
		sizeof(received_http_header_handle_t);
	const unsigned headers_payload_offset = current_offset + headers_size;
	if (headers_payload_offset > max_offset) {
	TooLargeHeaders:
		/* Error. */
		free_shuttle_with_anchor(shuttle);
		req->res.status = 431;
		req->res.reason = "Request Header Fields Too Large";
		h2o_send_inline(req,
			H2O_STRLIT("Request Header Fields Too Large\n"));
		return 0;
	}
	received_http_header_handle_t *const handles =
		(received_http_header_handle_t *)&response->un.req.buffer[
			current_offset];
	current_offset += num_headers * sizeof(received_http_header_handle_t);
	unsigned header_idx;
	for (header_idx = 0; header_idx < num_headers; ++header_idx) {
		const h2o_header_t *const header = &headers[header_idx];
		if (current_offset + header->name->len + 1 +
		    header->value.len > max_offset)
			goto TooLargeHeaders;
		received_http_header_handle_t *const handle =
			&handles[header_idx];
		handle->name_size = header->name->len;
		handle->value_size = header->value.len;
		memcpy(&response->un.req.buffer[current_offset],
			header->name->base, handle->name_size);
		current_offset += handle->name_size;
		response->un.req.buffer[current_offset] = 0;
		++current_offset;
		memcpy(&response->un.req.buffer[current_offset],
			header->value.base, handle->value_size);
		current_offset += handle->value_size;
	}

	unsigned body_bytes_to_copy;
	if (current_offset + req->entity.len > max_offset) {
#ifdef SPLIT_LARGE_BODY
		if (conf.use_body_split) {
			response->un.req.is_body_incomplete = true;
			body_bytes_to_copy = max_offset - current_offset;
			response->un.req.offset_within_body =
				body_bytes_to_copy;
		} else
#endif /* SPLIT_LARGE_BODY */
		{
			/* Error. */
			free_shuttle_with_anchor(shuttle);
			req->res.status = 413;
			req->res.reason = "Payload Too Large";
			h2o_send_inline(req,
				H2O_STRLIT("Payload Too Large\n"));
			return 0;
		}
	} else {
#ifdef SPLIT_LARGE_BODY
		response->un.req.is_body_incomplete = false;
#endif /* SPLIT_LARGE_BODY */
		body_bytes_to_copy = req->entity.len;
	}

	response->un.req.num_headers = num_headers;
	response->un.req.body_len = body_bytes_to_copy;
	memcpy(&response->un.req.buffer[current_offset],
		req->entity.base, body_bytes_to_copy);

	response->sent_something = false;
	response->cancelled = false;
	response->upgraded_to_websocket = false;
	response->waiter = NULL;
	response->un.req.lua_handler_ref = self->lua_handler_ref;
	response->site_path = self->path;

	thread_ctx_t *const thread_ctx = get_curr_thread_ctx();
	if (xtm_fun_dispatch(thread_ctx->queue_to_tx,
	    (void(*)(void *))&process_lua_req_in_tx, shuttle, 0)) {
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

static h2o_pathconf_t *register_handler(h2o_hostconf_t *hostconf,
	const char *path, int (*on_req)(h2o_handler_t *, h2o_req_t *))
{
	/* These functions never return NULL, dying instead */
	h2o_pathconf_t *pathconf = h2o_config_register_path(hostconf, path, 0);
	h2o_handler_t *handler =
		h2o_create_handler(pathconf, sizeof(*handler));
	handler->on_req = on_req;
	return pathconf;
}

/* Launched in TX thread. */
static void register_lua_handler_part_one(lua_site_t *lua_site,
	const char *path, int lua_handler_ref)
{
	memcpy(lua_site->path, path, lua_site->path_len);
	lua_site->path[lua_site->path_len] = 0;
	lua_site->lua_handler_ref = lua_handler_ref;
}

/* Can be launched in TX thread or HTTP server thread. */
static h2o_pathconf_t *register_lua_handler_part_two(h2o_hostconf_t *hostconf,
	lua_site_t *lua_site, unsigned thread_idx)
{
	/* These functions never return NULL, dying instead */
	h2o_pathconf_t *pathconf =
		h2o_config_register_path(hostconf, lua_site->path, 0);
	lua_h2o_handler_t *handler = (lua_h2o_handler_t *)
		h2o_create_handler(pathconf, sizeof(*handler));
	handler->super.on_req =
		(int (*)(h2o_handler_t *, h2o_req_t *))lua_req_handler;
	handler->lua_handler_ref = lua_site->lua_handler_ref;
	handler->path = lua_site->path;
	handler->path_len = lua_site->path_len;
	lua_site->lua_handlers[thread_idx] = handler;
	lua_site->pathconfs[thread_idx] = pathconf;
	return pathconf;
}

/* Launched in TX thread. */
static void register_lua_handler(lua_site_t *lua_site,
	const char *path, int lua_handler_ref)
{
	register_lua_handler_part_one(lua_site, path, lua_handler_ref);
	unsigned thread_idx;
	for (thread_idx = 0; thread_idx < MAX_threads; ++thread_idx)
		register_lua_handler_part_two(conf.thread_ctxs[thread_idx]
			.hostconf, lua_site, thread_idx);
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

/* Launched in HTTP server thread or in TX thread
 * when !SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD. */
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

/* Launched in HTTP server thread. */
static void anchor_dispose(void *param)
{
	anchor_t *const anchor = (anchor_t*)param;
	shuttle_t *const shuttle = anchor->shuttle;
	if (shuttle != NULL) {
		if (anchor->user_free_shuttle != NULL)
			anchor->user_free_shuttle(shuttle);
		else
			shuttle->disposed = true;
	}

	/* Probably should implemented support for "stubborn" anchors - 
	 * optionally wait for TX processing to finish so TX thread can
	 * access h2o_req_t directly thus avoiding copying LARGE buffers,
	 * it only makes sense in very specific cases because it stalls
	 * the whole thread if such request is gone. */
}

/* Launched in HTTP server thread. */
shuttle_t *prepare_shuttle(h2o_req_t *req)
{
	anchor_t *const anchor = (anchor_t *)h2o_mem_alloc_shared(&req->pool,
		sizeof(anchor_t), &anchor_dispose);
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

static void on_underlying_socket_free(void *data)
{
	h2o_linklist_unlink_fast(&my_container_of(data,
		our_sock_t, super)->accepted_list);
	thread_ctx_t *const thread_ctx = get_curr_thread_ctx();
	--thread_ctx->num_connections;
#ifdef USE_LIBUV
	free(data);
#endif /* USE_LIBUV */
}

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

	thread_ctx_t *const thread_ctx = get_curr_thread_ctx();
	if (thread_ctx->num_connections >= conf.max_conn_per_thread)
		return;

	/* FIXME: Pools instead of malloc? */
	our_sock_t *const conn = h2o_mem_alloc(sizeof(*conn));
	if (uv_tcp_init(uv_listener->loop, &conn->super)) {
		free(conn);
		return;
	}

	if (uv_accept(uv_listener, (uv_stream_t *)&conn->super)) {
		uv_close((uv_handle_t *)conn, (uv_close_cb)free);
		return;
	}

	h2o_linklist_insert_fast(&thread_ctx->accepted_sockets,
		&conn->accepted_list);
	++thread_ctx->num_connections;

	listener_ctx_t *const listener_ctx =
		(listener_ctx_t *)uv_listener->data;
	h2o_accept(&listener_ctx->accept_ctx,
		h2o_uv_socket_create((uv_stream_t *)&conn->super,
			(uv_close_cb)on_underlying_socket_free));
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
		struct st_h2o_evloop_socket_t *const sock =
			h2o_evloop_socket_accept_ex(listener,
				sizeof(our_sock_t));
		if (sock == NULL)
			return;

		our_sock_t *const item =
			container_of(sock, our_sock_t, super);
		h2o_linklist_insert_fast(&thread_ctx->accepted_sockets,
			&item->accepted_list);

		++thread_ctx->num_connections;

		sock->super.on_close.cb = on_underlying_socket_free;
		sock->super.on_close.data = sock;

		h2o_accept(&listener_ctx->accept_ctx, &sock->super);
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
	(void)result; /* To build w/disabled assert(). */
}

/* Returns file descriptor or -1 on error. */
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

	int flags = SOCK_STREAM;
#ifdef SOCK_CLOEXEC
	flags |= SOCK_CLOEXEC;
#endif /* SOCK_CLOEXEC */
	if ((fd = socket(AF_INET, flags, 0)) == -1) {
		return -1;
	}
#ifndef SOCK_CLOEXEC
	if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
		close(fd);
		return -1;
	}
#endif /* SOCK_CLOEXEC */

	int reuseaddr_flag = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr_flag,
	    sizeof(reuseaddr_flag)) != 0 ||
            bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    listen(fd, SOMAXCONN) != 0) {
		close(fd);
		/* TODO: Log error. */
		return -1;
	}

#ifdef TCP_DEFER_ACCEPT
	{
		/* We are only interested in connections
		 * when actual data is received. */
		int flag = 1;
		if (setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &flag,
		    sizeof(flag)) != 0) {
			close(fd);
			/* TODO: Log error. */
			return -1;
		}
	}
#endif /* TCP_DEFER_ACCEPT */

	if (conf.tfo_queues > 0) {
		/* TCP extension to do not wait for SYN/ACK for "known"
		 * clients. */
#ifdef TCP_FASTOPEN
		int tfo_queues;
#ifdef __APPLE__
		/* In OS X, the option value for TCP_FASTOPEN must be 1
		 * if is's enabled. */
		tfo_queues = 1;
#else
		tfo_queues = conf.tfo_queues;
#endif /* __APPLE__ */
		if (setsockopt(fd, IPPROTO_TCP, TCP_FASTOPEN,
		    (const void *)&tfo_queues, sizeof(tfo_queues)) != 0) {
			/* TODO: Log warning. */
		}
#else
		assert(!".tfo_queues not zero on platform w/o TCP_FASTOPEN");
#endif /* TCP_FASTOPEN */
	}

	return fd;
}

static SSL_CTX *setup_ssl(const char *cert_file, const char *key_file,
	int level, long min_proto_version)
{
#ifndef OPENSSL_VERSION_NUMBER
#error "OPENSSL_VERSION_NUMBER is not defined"
#endif /* OPENSSL_VERSION_NUMBER */
#if OPENSSL_VERSION_NUMBER < 0x1010000fL
#error "OpenSSL 1.1.* is required"
#endif /* OPENSSL_VERSION_NUMBER < 0x1010000fL */
	if (!SSL_load_error_strings())
		return NULL;
	SSL_library_init(); /* Always succeeds */
	OpenSSL_add_all_algorithms();

	SSL_CTX *ssl_ctx = SSL_CTX_new(SSLv23_server_method());
	if (ssl_ctx == NULL)
		return NULL;

	SSL_CTX_set_min_proto_version(ssl_ctx, min_proto_version);

	SSL_CTX_set_security_level(ssl_ctx, level);

	if (SSL_CTX_use_certificate_file(ssl_ctx, cert_file,
	    SSL_FILETYPE_PEM) != 1) {
		SSL_CTX_free(ssl_ctx);
		return NULL;
	}
	if (SSL_CTX_use_PrivateKey_file(ssl_ctx, key_file,
	    SSL_FILETYPE_PEM) != 1) {
		SSL_CTX_free(ssl_ctx);
		return NULL;
	}

/* Setup protocol negotiation methods. */
#if H2O_USE_NPN
	h2o_ssl_register_npn_protocols(ssl_ctx, h2o_http2_npn_protocols);
#endif /* H2O_USE_NPN */
#if H2O_USE_ALPN
#ifdef DISABLE_HTTP2
	/* Disable HTTP/2 e. g. to test WebSockets. */
	static const h2o_iovec_t my_alpn_protocols[] = {
		{H2O_STRLIT("http/1.1")}, {NULL}
	};
	h2o_ssl_register_alpn_protocols(ssl_ctx, my_alpn_protocols);
#else /* DISABLE_HTTP2 */

	h2o_ssl_register_alpn_protocols(ssl_ctx, h2o_http2_alpn_protocols);
#endif /* DISABLE_HTTP2 */
#endif /* H2O_USE_ALPN */

	return ssl_ctx;
}

/* Launched in TX thread. */
static void
register_listener_cfgs_socket(int fd, unsigned listener_idx)
{
	assert(listener_idx < conf.num_listeners);
	listener_cfg_t *const listener_cfg =
		&conf.listener_cfgs[listener_idx];
	assert(!listener_cfg->is_opened);
	listener_cfg->fd = fd;
	listener_cfg->is_opened = true;
}

/* Launched in TX thread. */
static void
close_listener_cfgs_sockets(void)
{
	unsigned listener_idx;
	for (listener_idx = 0; listener_idx < conf.num_listeners;
	    ++listener_idx) {
		listener_cfg_t *const listener_cfg =
		    &conf.listener_cfgs[listener_idx];
		if (listener_cfg->is_opened) {
			close(listener_cfg->fd);
			listener_cfg->is_opened = false;
		}
	}
}

/* Launched in TX thread. */
static bool
prepare_listening_sockets(thread_ctx_t *thread_ctx)
{
#ifdef USE_LIBUV
#error "prepare_listening_sockets() not implemented for libuv yet"
#else /* USE_LIBUV */
	unsigned listener_idx;
	for (listener_idx = 0; listener_idx < conf.num_listeners;
	    ++listener_idx) {
		listener_ctx_t *const listener_ctx =
		    &thread_ctx->listener_ctxs[listener_idx];
		const listener_cfg_t *const listener_cfg =
		    &conf.listener_cfgs[listener_idx];

		assert(listener_cfg->is_opened);
		memset(listener_ctx, 0, sizeof(*listener_ctx));
		listener_ctx->thread_ctx = thread_ctx;
		listener_ctx->accept_ctx.ssl_ctx = conf.ssl_ctx;
		listener_ctx->accept_ctx.ctx = &thread_ctx->ctx;
		listener_ctx->accept_ctx.hosts = thread_ctx->globalconf.hosts;
		listener_ctx->sock = NULL;

		if (thread_ctx->idx) {
			if ((listener_ctx->fd = dup(listener_cfg->fd)) < 0)
				/* FIXME: Should report. */
				return false;
		} else
			listener_ctx->fd = listener_cfg->fd;
		set_cloexec(listener_ctx->fd);
		thread_ctx->listeners_created++;
	}
	return true;
#endif /* USE_LIBUV */
}

/* Can be launched in TX thread or HTTP server thread. */
static void
close_listening_sockets(thread_ctx_t *thread_ctx)
{
#ifdef USE_LIBUV
#error "close_listening_sockets() not implemented for libuv yet"
#else /* USE_LIBUV */
	listener_ctx_t *const listener_ctxs =
			thread_ctx->listener_ctxs;

	unsigned listener_idx;
	for (listener_idx = 0; listener_idx < thread_ctx->listeners_created;
	    ++listener_idx) {
		listener_ctx_t *const listener_ctx =
		    &listener_ctxs[listener_idx];
		close(listener_ctx->fd);
	}
	thread_ctx->listeners_created = 0;
	if (thread_ctx->idx == 0) {
		for (listener_idx = 0; listener_idx < conf.num_listeners;
		    ++listener_idx) {
			listener_cfg_t *const listener_cfg =
			    &conf.listener_cfgs[listener_idx];
			listener_cfg->is_opened = false;
		}
	}
#endif /* USE_LIBUV */
}

/* Launched in HTTP server thread. */
static void
listening_sockets_stop_read(thread_ctx_t *thread_ctx)
{
#ifdef USE_LIBUV
#error "listening_sockets_stop_read() not implemented for libuv yet"
#else /* USE_LIBUV */
	unsigned listener_idx;
	for (listener_idx = 0; listener_idx < thread_ctx->listeners_created;
	    ++listener_idx) {
		listener_ctx_t *const listener_ctx =
		    &thread_ctx->listener_ctxs[listener_idx];
		h2o_socket_read_stop(listener_ctx->sock);
		h2o_socket_close(listener_ctx->sock);
		listener_ctx->sock = NULL;
	}
	thread_ctx->listeners_created = 0;
#endif /* USE_LIBUV */
}

/* Launched in HTTP server thread. */
static void
listening_sockets_start_read(thread_ctx_t *thread_ctx)
{
#ifdef USE_LIBUV
#error "listening_sockets_start_read() not implemented for libuv yet"
#else /* USE_LIBUV */
	unsigned listener_idx;
	for (listener_idx = 0; listener_idx < thread_ctx->listeners_created;
	    ++listener_idx) {
		listener_ctx_t *const listener_ctx =
		   &thread_ctx->listener_ctxs[listener_idx];
		listener_ctx->sock = h2o_evloop_socket_create(
				thread_ctx->ctx.loop,
				listener_ctx->fd, H2O_SOCKET_FLAG_DONT_READ);
		listener_ctx->sock->data = listener_ctx;
		h2o_socket_read_start(listener_ctx->sock, on_accept);
	}
#endif /* USE_LIBUV */
}

/* Launched in TX thread. */
static void
reset_thread_ctx(unsigned idx)
{
	thread_ctx_t *const thread_ctx = &conf.thread_ctxs[idx];

	thread_ctx->should_notify_tx_done = false;
	thread_ctx->tx_fiber_should_exit = false;
	thread_ctx->shutdown_requested = false;
	thread_ctx->tx_done_notification_received = false;
	thread_ctx->tx_fiber_finished = false;
	thread_ctx->thread_finished = false;
#ifndef USE_LIBUV
	thread_ctx->queue_from_tx_fd_consumed = false;
#endif /* USE_LIBUV */
}

/* Returns false in case of error. */
static bool init_worker_thread(unsigned thread_idx)
{
#ifdef USE_LIBUV
	int fd_consumed = 0;
#endif /* USE_LIBUV */
	thread_ctx_t *const thread_ctx = &conf.thread_ctxs[thread_idx];
	if ((thread_ctx->queue_from_tx = xtm_create(QUEUE_FROM_TX_ITEMS))
	    == NULL)
		/* FIXME: Report. */
		goto alloc_xtm_failed;

	if ((thread_ctx->listener_ctxs = (listener_ctx_t *)
	    malloc(conf.num_listeners * sizeof(listener_ctx_t))) == NULL)
		/* FIXME: Report. */
		goto alloc_ctxs_failed;

	memset(&thread_ctx->ctx, 0, sizeof(thread_ctx->ctx));
#ifdef USE_LIBUV
	uv_loop_init(&thread_ctx->loop);
#ifndef INIT_CTX_IN_HTTP_THREAD
	h2o_context_init(&thread_ctx->ctx, &thread_ctx->loop,
		&thread_ctx->globalconf);
#endif /* INIT_CTX_IN_HTTP_THREAD */
#else /* USE_LIBUV */
	/* Can't call h2o_context_init() here, this must be done
	 * from HTTP thread because it (indirectly)
	 * uses thread-local variables. */
#ifndef INIT_CTX_IN_HTTP_THREAD
	h2o_context_init(&thread_ctx->ctx, h2o_evloop_create(),
		&thread_ctx->globalconf);
#endif /* INIT_CTX_IN_HTTP_THREAD */
#endif /* USE_LIBUV */
	h2o_linklist_init_anchor(&thread_ctx->accepted_sockets);

	if (!prepare_listening_sockets(thread_ctx))
		goto prepare_listening_sockets_failed;

#ifdef USE_LIBUV
	if (uv_tcp_init(thread_ctx->ctx.loop, &listener_ctx->uv_tcp_listener))
		/* FIXME: Should report. */
		goto uv_tcp_init_failed;
	if (uv_tcp_open(&listener_ctx->uv_tcp_listener, listener_ctx->fd))
		/* FIXME: Should report. */
		goto uv_tcp_open_failed;
	fd_consumed = 1;
	listener_ctx->uv_tcp_listener.data = listener_ctx;
	if (uv_listen((uv_stream_t *)&listener_ctx->uv_tcp_listener,
	    SOMAXCONN, on_accept))
		/* FIXME: Should report. */
		goto uv_listen_failed;

	if (uv_poll_init(thread_ctx->ctx.loop, &thread_ctx->uv_poll_from_tx,
	    xtm_fd(thread_ctx->queue_from_tx)))
		/* FIXME: Should report. */
		goto uv_poll_init_failed;
	if (uv_poll_start(&thread_ctx->uv_poll_from_tx, UV_READABLE,
	    on_call_from_tx))
		goto uv_poll_start_failed;
#endif /* USE_LIBUV */

	return true;

#ifdef USE_LIBUV
uv_poll_start_failed:
	uv_close((uv_handle_t *)&thread_ctx->uv_poll_from_tx, NULL);
uv_poll_init_failed:
uv_listen_failed:
uv_tcp_open_failed:
	uv_close((uv_handle_t *)&listener_ctx->uv_tcp_listener, NULL);

uv_tcp_init_failed:
	if (!fd_consumed && thread_idx)
		close(listener_ctx->fd);
#endif /* USE_LIBUV */

prepare_listening_sockets_failed:
	close_listening_sockets(thread_ctx);
#ifdef USE_LIBUV
	uv_loop_close(&thread_ctx->loop);
#ifndef INIT_CTX_IN_HTTP_THREAD
	h2o_context_dispose(&thread_ctx->ctx);
#endif /* INIT_CTX_IN_HTTP_THREAD */
#else /* USE_LIBUV */
#ifndef INIT_CTX_IN_HTTP_THREAD
	h2o_context_dispose(&thread_ctx->ctx);
#endif /* INIT_CTX_IN_HTTP_THREAD */
	h2o_evloop_destroy(thread_ctx->ctx.loop);
#endif /* USE_LIBUV */

	free(thread_ctx->listener_ctxs);
alloc_ctxs_failed:
	my_xtm_delete_queue_from_tx(thread_ctx);
alloc_xtm_failed:
	return false;
}

/* Launched in TX thread. */
static void finish_processing_lua_reqs_in_tx(thread_ctx_t *thread_ctx)
{
	if (thread_ctx->active_lua_fibers == 0)
		stubborn_dispatch_thr_from_tx(thread_ctx, &tx_done);
	else
		thread_ctx->should_notify_tx_done = true;
}

/* Launched in HTTP server thread. */
static inline void tell_close_connection(our_sock_t *item)
{
	static const char err[] = "shutting down";
	/* Using read callback is faster and futureproof but, alas,
	 * we can't do this if it is NULL. */
#ifdef USE_LIBUV
	struct st_h2o_uv_socket_t *const uv_sock = item->super.data;

	/* This is not really safe (st_h2o_uv_socket_t can be changed
	 * so h2o_socket_t is no longer first member - unlikely but possible)
	 * but the alternative is to include A LOT of h2o internal headers. */
	h2o_socket_t *const sock = (h2o_socket_t *)uv_sock;
#else /* USE_LIBUV */
	h2o_socket_t *const sock = &item->super.super;
#endif /* USE_LIBUV */
	h2o_socket_read_stop(sock);
	/* Alas, we have to find proper handler ourself. */
	switch (sock->proto) {
	case SOCK_PROTO_SSL:
		h2o_ssl_on_handshake_complete(sock, err);
		break;
	case SOCK_PROTO_HTTP1:
		h2o_http1_reqread_on_read(sock, err);
		break;
	case SOCK_PROTO_HTTP2:
		h2o_http2_close_connection_now((h2o_http2_conn_t *)sock->data);
		break;
	case SOCK_PROTO_EXPECT_PROXY:
		/* FIXME: Looks like this would never happen. */
		h2o_on_read_proxy_line(sock, err);
		break;
	case SOCK_PROTO_WEBSOCKET:
		h2o_websocket_on_recv(sock, err);
		break;
	default:
		assert(!"Invalid sock proto");
	}
}

/* Launched in HTTP server thread. */
static void close_existing_connections(thread_ctx_t *thread_ctx)
{
	our_sock_t *item =
		container_of(thread_ctx->accepted_sockets.next,
			our_sock_t, accepted_list);
	while (&item->accepted_list != &thread_ctx->accepted_sockets) {
		our_sock_t *const next = container_of(item->accepted_list.next,
			our_sock_t, accepted_list);
		tell_close_connection(item);
		item = next;
	}
}

/* Launched in HTTP server thread. */
static void prepare_for_shutdown(thread_ctx_t *thread_ctx)
{
	/* FIXME: If we want to send something through existing
	 * connections, should do it now (accepts are already
	 * blocked). */

	if (thread_ctx->use_graceful_shutdown)
		/* FIXME: Modify libh2o to be even more graceful?
		 * Force it to close all connections w/o active requests,
		 * not just ignore keep-alive flag.
		 * FIXME: Can't use h2o_context_request_shutdown() because it
		 * activates timeout which we do not cancel thus causing
		 * assert. */
		thread_ctx->ctx.shutdown_requested = 1;
	else
		close_existing_connections(thread_ctx);
	thread_ctx->do_not_exit_tx_fiber = thread_ctx->use_graceful_shutdown;

	fprintf(stderr, "Thread #%u: shutdown request received, "
		"waiting for TX processing to complete...\n",
		thread_ctx->idx);
	stubborn_dispatch_thr_to_tx(thread_ctx,
		&finish_processing_lua_reqs_in_tx);
}

/* Launched in HTTP server thread. */
static void handle_graceful_shutdown(thread_ctx_t *thread_ctx)
{
	if (!thread_ctx->use_graceful_shutdown)
		return;

	close_existing_connections(thread_ctx);

	/* There can still be requests in flight. */
	thread_ctx->tx_done_notification_received = false;
	thread_ctx->do_not_exit_tx_fiber = false;
	stubborn_dispatch_thr_to_tx(thread_ctx,
		&finish_processing_lua_reqs_in_tx);
#ifdef USE_LIBUV
	uv_run(&thread_ctx->loop, UV_RUN_DEFAULT);
#else /* USE_LIBUV */
	h2o_evloop_t *const loop = thread_ctx->ctx.loop;
	while (!thread_ctx->tx_done_notification_received)
		h2o_evloop_run(loop, 1);
#endif /* USE_LIBUV */
}

/* This is HTTP server thread main function. */
static void *worker_func(void *param)
{
	/* FIXME: SIGTERM should terminate loop. */
	const unsigned thread_idx = (unsigned)(uintptr_t)param;
	thread_ctx_t *const thread_ctx = &conf.thread_ctxs[thread_idx];
	curr_thread_ctx = thread_ctx;
#ifdef USE_LIBUV
#ifdef INIT_CTX_IN_HTTP_THREAD
	h2o_context_init(&thread_ctx->ctx, &thread_ctx->loop,
		&thread_ctx->globalconf);
#endif /* INIT_CTX_IN_HTTP_THREAD */
	uv_async_init(&thread_ctx->loop, &thread_ctx->async,
		(uv_async_cb)async_cb);
#else /* USE_LIBUV */
#ifdef INIT_CTX_IN_HTTP_THREAD
	h2o_context_init(&thread_ctx->ctx, h2o_evloop_create(),
		&thread_ctx->globalconf);
#endif /* INIT_CTX_IN_HTTP_THREAD */
	init_async(thread_ctx);
#endif /* USE_LIBUV */

	__sync_synchronize();
	httpng_sem_post(&thread_ctx->can_be_terminated);
#ifdef USE_LIBUV
#error "multilisten code doesn't support Libuv now"
	/* Process incoming connections/data and requests
	 * from TX thread. */
	uv_run(&thread_ctx->loop, UV_RUN_DEFAULT);

	assert(thread_ctx->shutdown_requested);

	/* FIXME: Need more than one. */
	listener_ctx_t *const listener_ctx = &thread_ctx->listener_ctxs[0];
	uv_read_stop((uv_stream_t *)&listener_ctx->uv_tcp_listener);
	uv_close((uv_handle_t *)&listener_ctx->uv_tcp_listener, NULL);

	prepare_for_shutdown(thread_ctx);

	/* Process remaining requests from TX thread. */
	uv_run(&thread_ctx->loop, UV_RUN_DEFAULT);
	assert(thread_ctx->tx_done_notification_received);
	handle_graceful_shutdown(thread_ctx);
#else /* USE_LIBUV */
	thread_ctx->sock_from_tx =
		h2o_evloop_socket_create(thread_ctx->ctx.loop,
			xtm_fd(thread_ctx->queue_from_tx),
			H2O_SOCKET_FLAG_DONT_READ);

	h2o_socket_read_start(thread_ctx->sock_from_tx, on_call_from_tx);
	thread_ctx->queue_from_tx_fd_consumed = true;
	listening_sockets_start_read(thread_ctx);
	h2o_evloop_t *loop = thread_ctx->ctx.loop;
	while (!thread_ctx->shutdown_requested)
		h2o_evloop_run(loop, INT32_MAX);

	listening_sockets_stop_read(thread_ctx);
	close_listening_sockets(thread_ctx);

	prepare_for_shutdown(thread_ctx);

	/* Process remaining requests from TX thread. */
	while (!thread_ctx->tx_done_notification_received)
		h2o_evloop_run(loop, 1);
	handle_graceful_shutdown(thread_ctx);

	h2o_socket_read_stop(thread_ctx->sock_from_tx);
	h2o_socket_close(thread_ctx->sock_from_tx);
#endif /* USE_LIBUV */

#ifdef INIT_CTX_IN_HTTP_THREAD
	h2o_context_dispose(&thread_ctx->ctx);
#endif /* INIT_CTX_IN_HTTP_THREAD */

	close_async(thread_ctx);
	httpng_sem_destroy(&thread_ctx->can_be_terminated);

	thread_ctx->thread_finished = true;
	__sync_synchronize();
	return NULL;
}

static void deinit_worker_thread(unsigned thread_idx)
{
	thread_ctx_t *const thread_ctx = &conf.thread_ctxs[thread_idx];

#ifdef USE_LIBUV
#error "multilisten code doesn't support Libuv now"
	/* FIXME: Need more than one. */
	listener_ctx_t *const listener_ctx = &thread_ctx->listener_ctxs[0];

	uv_read_stop((uv_stream_t *)&listener_ctx->uv_tcp_listener);
	uv_poll_stop(&thread_ctx->uv_poll_from_tx);
	uv_close((uv_handle_t *)&thread_ctx->uv_poll_from_tx, NULL);
	uv_close((uv_handle_t *)&listener_ctx->uv_tcp_listener, NULL);
#else /* USE_LIBUV */
	h2o_evloop_t *const loop = thread_ctx->ctx.loop;
	h2o_evloop_run(loop, 0); /* To actually free memory. */
#endif /* USE_LIBUV */

#ifdef USE_LIBUV
	uv_loop_close(&thread_ctx->loop);
#ifndef INIT_CTX_IN_HTTP_THREAD
	h2o_context_dispose(&thread_ctx->ctx);
#endif /* INIT_CTX_IN_HTTP_THREAD */
#else /* USE_LIBUV */
#ifndef INIT_CTX_IN_HTTP_THREAD
	h2o_context_dispose(&thread_ctx->ctx);
#endif /* INIT_CTX_IN_HTTP_THREAD */
	h2o_evloop_destroy(loop);
#endif /* USE_LIBUV */

	/* FIXME: Should flush these queues first. */
	my_xtm_delete_queue_from_tx(thread_ctx);
	free(thread_ctx->listener_ctxs);
}

/* Launched in TX thread. */
static int
tx_fiber_func(va_list ap)
{
	const unsigned fiber_idx = va_arg(ap, unsigned);
	/* This fiber processes requests from particular thread */
	thread_ctx_t *const thread_ctx = &conf.thread_ctxs[fiber_idx];
	struct xtm_queue *const queue_to_tx = thread_ctx->queue_to_tx;
	const int pipe_fd = xtm_fd(queue_to_tx);
	/* thread_ctx->tx_fiber_should_exit is read non-atomically for
	 * performance reasons so it should be changed in this thread by
	 * queueing corresponding function call. */
	while (!thread_ctx->tx_fiber_should_exit) {
		if (coio_wait(pipe_fd, COIO_READ, DBL_MAX) & COIO_READ) {
			xtm_fun_invoke_all(queue_to_tx);
		}
	}
	thread_ctx->tx_fiber_finished = true;
	if (thread_ctx->fiber_to_wake_on_shutdown != NULL) {
		struct fiber *const fiber = thread_ctx->fiber_to_wake_on_shutdown;
		thread_ctx->fiber_to_wake_on_shutdown = NULL;
		fiber_wakeup(fiber);
	}
	return 0;
}

/* Launched in HTTP server thread. */
static void async_cb(void *param)
{
	thread_ctx_t *const thread_ctx =
		my_container_of(param, thread_ctx_t, async);
	thread_ctx->shutdown_requested = true;
#ifdef USE_LIBUV
	uv_stop(&thread_ctx->loop);
#endif /* USE_LIBUV */
}

#ifndef USE_LIBUV
/* Launched in HTTP server thread. */
static void on_async_read(h2o_socket_t *sock, const char *err)
{
	if (err != NULL) {
		fprintf(stderr, "pipe error: %s\n", err);
		abort();
	}

	h2o_buffer_consume(&sock->input, sock->input->size);
	thread_ctx_t *const thread_ctx = get_curr_thread_ctx();
	async_cb(&thread_ctx->async);
}

/* Launched in HTTP server thread. */
static void init_async(thread_ctx_t *thread_ctx)
{
	h2o_loop_t *const loop = thread_ctx->ctx.loop;
	int fds[2];

	if (cloexec_pipe(fds) != 0) {
		perror("pipe");
		abort();
	}
	if (fcntl(fds[1], F_SETFL, O_NONBLOCK) != 0) {
		perror("fcntl");
		abort();
	}
	thread_ctx->async.write_fd = fds[1];
	thread_ctx->async.read_socket =
		h2o_evloop_socket_create(loop, fds[0], 0);
	h2o_socket_read_start(thread_ctx->async.read_socket,
		on_async_read);
}
#endif /* USE_LIBUV */

static void close_async(thread_ctx_t *thread_ctx)
{
#ifdef USE_LIBUV
	/* FIXME: Such call in h2o proper uses free()
	 * as callback - bug? */
	uv_close((uv_handle_t *)&thread_ctx->async, NULL);
#else /* USE_LIBUV */
	h2o_socket_read_stop(thread_ctx->async.read_socket);
	h2o_socket_close(thread_ctx->async.read_socket);
	close(thread_ctx->async.write_fd);
#endif /* USE_LIBUV */
}

/* Launched in TX thread. */
static void tell_thread_to_terminate_internal(thread_ctx_t *thread_ctx,
	bool use_graceful_shutdown)
{
	thread_ctx->use_graceful_shutdown = use_graceful_shutdown;
	httpng_sem_wait(&thread_ctx->can_be_terminated);
#ifdef USE_LIBUV
	uv_async_send(&thread_ctx->async);
#else /* USE_LIBUV */
	while (write(thread_ctx->async.write_fd, "", 1) < 0
	    && errno == EINTR)
		;
#endif /* USE_LIBUV */
}

/* Launched in TX thread. */
static inline void tell_thread_to_terminate(thread_ctx_t *thread_ctx)
{
	tell_thread_to_terminate_internal(thread_ctx, false);
}

/* Launched in TX thread. */
static inline void tell_thread_to_terminate_gracefully(
	thread_ctx_t *thread_ctx)
{
	tell_thread_to_terminate_internal(thread_ctx, true);
}

static void configure_shutdown_callback(lua_State *L, bool setup)
{
	if (lua_pcall(L, 2, 0, 0) == LUA_OK) {
		conf.is_on_shutdown_setup = setup;
		if (!setup) {
			luaL_unref(L, LUA_REGISTRYINDEX, conf.on_shutdown_ref);
			conf.on_shutdown_ref = LUA_REFNIL;
		}
	} else
		fprintf(stderr, "Warning: box.ctl.on_shutdown() failed: %s\n",
			lua_tostring(L, -1));
}

/* Launched in TX thread. */
static void setup_on_shutdown(lua_State *L, bool setup,
	bool called_from_callback)
{
	if (conf.on_shutdown_ref == LUA_REFNIL && !called_from_callback) {
		assert(setup);
		lua_pushcfunction(L, on_shutdown_callback);
		conf.on_shutdown_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	}

	lua_getglobal(L, "box");
	if (lua_type(L, -1) == LUA_TTABLE) {
		lua_getfield(L, -1, "ctl");
		if (lua_type(L, -1) == LUA_TTABLE) {
			lua_getfield(L, -1, "on_shutdown");
			if (lua_type(L, -1) == LUA_TFUNCTION) {
				if (setup) {
					lua_rawgeti(L, LUA_REGISTRYINDEX,
						conf.on_shutdown_ref);
					lua_pushnil(L);
					configure_shutdown_callback(L, setup);
				} else if (!called_from_callback) {
					lua_pushnil(L);
					lua_rawgeti(L, LUA_REGISTRYINDEX,
						conf.on_shutdown_ref);
					configure_shutdown_callback(L, setup);
				}
			} else
				fprintf(stderr,
	"Warning: global 'box.ctl.on_shutdown' is not a function\n");
		} else
			fprintf(stderr,
			"Warning: global 'box.ctl' is not a table\n");
	} else
		fprintf(stderr, "Warning: global 'box' is not a table\n");
}

/* Launched in TX thread. */
static void reap_finished_thread(thread_ctx_t *thread_ctx)
{
	pthread_join(thread_ctx->tid, NULL);

#ifdef USE_LIBUV
	uv_close((uv_handle_t *)&thread_ctx->uv_poll_from_tx, NULL);
	uv_loop_close(&thread_ctx->loop);
#ifndef INIT_CTX_IN_HTTP_THREAD
	h2o_context_dispose(&thread_ctx->ctx);
#endif /* INIT_CTX_IN_HTTP_THREAD */
#else /* USE_LIBUV */
#ifndef INIT_CTX_IN_HTTP_THREAD
	h2o_context_dispose(&thread_ctx->ctx);
#endif /* INIT_CTX_IN_HTTP_THREAD */
	h2o_evloop_destroy(thread_ctx->ctx.loop);
#endif /* USE_LIBUV */

	if (!thread_ctx->tx_fiber_finished) {
		assert(thread_ctx->fiber_to_wake_on_shutdown == NULL);
		thread_ctx->fiber_to_wake_on_shutdown = fiber_self();
		fiber_yield();
	}
	assert(thread_ctx->tx_fiber_finished);

	free(thread_ctx->listener_ctxs);
	xtm_delete(thread_ctx->queue_to_tx);
	my_xtm_delete_queue_from_tx(thread_ctx);
}

/* Launched in TX thread.
 * Returns error message in case of error. */
static const char *reap_gracefully_terminating_threads(void)
{
	unsigned thr_idx;
	for (thr_idx = conf.num_threads - 1;
	    thr_idx >= conf.num_desired_threads; --thr_idx) {
		thread_ctx_t *const thread_ctx = &conf.thread_ctxs[thr_idx];
		if (!thread_ctx->thread_finished) {
			conf.num_threads = thr_idx + 1;
		return "Unable to reconfigure until threads will shut down";
		}
		reap_finished_thread(thread_ctx);
	}
	conf.num_threads = conf.num_desired_threads;
	return NULL;
}

/* Launched in HTTP server thread.
 * N. b.: It may never be launched if thread terminates. */
static void become_ungraceful(thread_ctx_t *thread_ctx)
{
	close_existing_connections(thread_ctx);
}

/* Launched in TX thread. */
static void reap_terminating_threads_ungracefully(void)
{
	if (reap_gracefully_terminating_threads() == NULL)
		return;

	unsigned thr_idx;
	for (thr_idx = conf.num_threads - 1;
	    thr_idx >= conf.num_desired_threads; --thr_idx) {
		thread_ctx_t *const thread_ctx = &conf.thread_ctxs[thr_idx];
		if (thread_ctx->thread_finished)
			continue;
		stubborn_dispatch_thr_from_tx(thread_ctx, &become_ungraceful);
	}

	for (thr_idx = conf.num_threads - 1;
	    thr_idx >= conf.num_desired_threads; --thr_idx) {
		thread_ctx_t *const thread_ctx = &conf.thread_ctxs[thr_idx];
		while (thread_ctx->active_lua_fibers ||
		    !thread_ctx->thread_finished)
			fiber_sleep(0.001);
	}

	const char *const err = reap_gracefully_terminating_threads();
	assert(err == NULL);
}

/* Launched in TX thread. */
static int on_shutdown_internal(lua_State *L, bool called_from_callback)
{
	while (conf.cfg_in_progress)
		fiber_sleep(0.001);
	if (!conf.configured)
		return luaL_error(L, "Server is not launched");
	if (conf.is_shutdown_in_progress) {
		if (!called_from_callback)
			return luaL_error(L,
				"on_shutdown() is already in progress");
		fprintf(stderr,
			"Warning: on_shutdown() is already in progress\n");
		return 0;
	}
	conf.is_shutdown_in_progress = true;
	reap_terminating_threads_ungracefully();
	if (conf.is_on_shutdown_setup)
		setup_on_shutdown(L, false, called_from_callback);
	unsigned thr_idx;
	for (thr_idx = 0; thr_idx < conf.num_threads; ++thr_idx) {
		thread_ctx_t *const thread_ctx =
			&conf.thread_ctxs[thr_idx];
		tell_thread_to_terminate(thread_ctx);
	}

	for (thr_idx = 0; thr_idx < conf.num_threads; ++thr_idx) {
		thread_ctx_t *const thread_ctx =
			&conf.thread_ctxs[thr_idx];
		/* We must yield CPU to other fibers to finish. */
		while (thread_ctx->active_lua_fibers ||
		    !thread_ctx->thread_finished)
			fiber_sleep(0.001);
		reap_finished_thread(thread_ctx);
	}
	unsigned idx;
#ifdef USE_LIBUV
	for (idx = 0; idx < conf.num_listeners; ++idx) {
		close(conf.listener_cfgs[idx].fd);
		for (thr_idx = 1; thr_idx < conf.num_threads; ++thr_idx)
			close(conf.thread_ctxs[thr_idx].listener_ctxs[idx].fd);
	}
#endif /* USE_LIBUV */
	free(conf.listener_cfgs);
	for (thr_idx = 0; thr_idx < MAX_threads;
	    ++thr_idx)
		h2o_config_dispose(&conf.thread_ctxs[thr_idx].globalconf);
	free(conf.thread_ctxs);
	for (idx = 0; idx < conf.lua_site_count; ++idx)
		free(conf.lua_sites[idx].path);
	free(conf.lua_sites);
	conf.configured = false;
	conf.idx_of_root_site = -1;
	complain_loudly_about_leaked_fds();
	conf.is_shutdown_in_progress = false;
	return 0;
}

/* Launched in TX thread. */
static int on_shutdown_callback(lua_State *L)
{
	return on_shutdown_internal(L, true);
}

/* Launched in TX thread. */
static int on_shutdown_for_user(lua_State *L)
{
	return on_shutdown_internal(L, false);
}

/* Launched in TX thread. */
static void flush_tx_lua_handlers(thread_ctx_t *thread_ctx)
{
	thread_ctx->http_and_tx_lua_handlers_flushed = true;
}

/* Launched in HTTP server thread. */
static void flush_http_lua_handlers(thread_ctx_t *thread_ctx)
{
	stubborn_dispatch_thr_to_tx(thread_ctx, flush_tx_lua_handlers);
}

enum {
	LUA_STACK_IDX_TABLE = 1,
	LUA_STACK_IDX_LUA_SITES = -2,
};

/* Launched in TX thread. */
static void replace_lua_handler_ref(lua_site_t *site)
{
	assert(site->lua_handler_ref != site->new_lua_handler_ref);
	site->old_lua_handler_ref = site->lua_handler_ref;
	site->lua_handler_ref = site->new_lua_handler_ref;

	unsigned thread_idx;
	for (thread_idx = 0; thread_idx < MAX_threads;
	    ++thread_idx)
		site->lua_handlers[thread_idx]->lua_handler_ref =
			site->new_lua_handler_ref;
}

/* Launched in TX thread. */
static void flush_lua_ref_handlers(void)
{
	__sync_synchronize();
	/* Now we should call useless function in HTTP thread
	 * which would call useless function in TX thread to ensure
	 * that no one would use "old" lua_handler_ref. */
	unsigned thr_idx;
	for (thr_idx = 0; thr_idx < conf.num_threads; ++thr_idx) {
		thread_ctx_t *const thread_ctx =  &conf.thread_ctxs[thr_idx];
		assert(!thread_ctx->http_and_tx_lua_handlers_flushed);
		stubborn_dispatch_thr_from_tx(thread_ctx,
			&flush_http_lua_handlers);
	}
	for (thr_idx = 0; thr_idx < conf.num_threads; ++thr_idx) {
		thread_ctx_t *const thread_ctx =  &conf.thread_ctxs[thr_idx];

		while (!thread_ctx->http_and_tx_lua_handlers_flushed)
			fiber_sleep(0.001);

		/* For next reload. */
		thread_ctx->http_and_tx_lua_handlers_flushed = false;
	}
}

/* Launched in TX thread. */
static void
replace_lua_handlers(lua_State *L)
{
	unsigned idx;
	for (idx = 0; idx < conf.lua_site_count; ++idx) {
		lua_site_t *const site = &conf.lua_sites[idx];
		if (site->generation == conf.generation)
			replace_lua_handler_ref(site);
	}

	flush_lua_ref_handlers();

	for (idx = 0; idx < conf.lua_site_count; ++idx) {
		lua_site_t *const site = &conf.lua_sites[idx];
		if (site->generation == conf.generation)
			luaL_unref(L, LUA_REGISTRYINDEX,
				site->old_lua_handler_ref);
	}
}

/* Launched in TX thread.*/
static void done_with_new_site(void *param)
{
	(void)param;
	--conf.add_new_sites_counter;
}

/* Launched in HTTP server thread.
 * Moves root to the end of list.
 * FIXME: Support all kinds of "overlapped" paths maybe? */
static void reorder_paths(thread_ctx_t *thread_ctx)
{
	int root_idx = -1;
	int non_root_idx = -1;

	/* FIXME: More than one. */
	const h2o_hostconf_t *const hostconf = thread_ctx->hostconf;
	assert(hostconf != NULL);

	unsigned idx;
	for (idx = 0; idx < hostconf->paths.size; ++idx) {
		const h2o_pathconf_t *const pathconf =
			&hostconf->paths.entries[idx];
		assert(pathconf->path.base != NULL);
		if (pathconf->path.len == 1 && *pathconf->path.base == '/')
			root_idx = idx;
		else
			non_root_idx = idx;
	}
	if (root_idx < 0 || non_root_idx < 0)
		return;
	if (root_idx >= non_root_idx)
		return;

	const h2o_pathconf_t tmp = hostconf->paths.entries[root_idx];
	hostconf->paths.entries[root_idx] =
		hostconf->paths.entries[non_root_idx];
	hostconf->paths.entries[non_root_idx] = tmp;
}

/* Can be launched in TX thread or HTTP server thread. */
static void hot_reload_add_remove_sites_in_some_thr(thread_ctx_t *thread_ctx,
	add_site_t *new_site, bool is_tx_thread)
{
	/* FIXME: More than one. */
	h2o_hostconf_t *const hostconf = thread_ctx->hostconf;

	const unsigned added_generation =
		conf.generation - ADD_NEW_SITE_GENERATION_SHIFT;
	unsigned idx;
	for (idx = 0; idx < conf.lua_site_count +
	    new_site->extra_sites; ++idx) {
		lua_site_t *const lua_site = &conf.lua_sites[idx];
		if (lua_site->generation != added_generation) {
			if (lua_site->generation == conf.generation)
				continue;
			/* Remove.
			 * We do not maintain 1:1 correlation between indexes,
			 * have to search for this path. */
			const char *const path = lua_site->path;
			const unsigned path_len = lua_site->path_len;
			unsigned path_idx;
			for (path_idx = 0; path_idx < hostconf->paths.size;
			    ++path_idx) {
				if (hostconf->paths.entries[path_idx].path.len
				    == path_len && !memcmp(path,
					    hostconf->paths.entries[path_idx]
					    .path.base, path_len))
					goto found;
			}
			assert(false); /* Should never happen. */
			continue;

		found:
			h2o_config_dispose_pathconf(
				&hostconf->paths.entries[path_idx]);
			memmove(&hostconf->paths.entries[path_idx],
				&hostconf->paths.entries[path_idx + 1],
				(conf.lua_site_count + new_site->extra_sites
				- path_idx - 1)
				* sizeof(hostconf->paths.entries[0]));
			--hostconf->paths.size;
			/* We can adjust .capacity and do realloc(). */
			continue;
		}
		h2o_context_init_pathconf_context(&thread_ctx->ctx,
			register_lua_handler_part_two(thread_ctx->hostconf,
				lua_site, thread_ctx->idx));
	}
	reorder_paths(thread_ctx);
	if (is_tx_thread)
		return;
	__sync_synchronize(); /* We have written to *lua_site. */
	stubborn_dispatch_uni(thread_ctx->queue_to_tx,
		(void *)done_with_new_site, NULL);
}

/* Launched in HTTP server thread. */
static void hot_reload_add_remove_sites_in_http_thr(add_site_t *new_site)
{
	return hot_reload_add_remove_sites_in_some_thr(get_curr_thread_ctx(),
		new_site, false);
}

/* Launched in TX thread. */
static void hot_reload_add_remove_sites_in_tx_thr(thread_ctx_t *thread_ctx,
	add_site_t *new_site)
{
	return hot_reload_add_remove_sites_in_some_thr(thread_ctx,
		new_site, true);
}

/* Launched in TX thread.*/
static void hot_reload_add_remove_sites(unsigned extra_sites)
{
	add_site_t new_site = (add_site_t){extra_sites};
	conf.add_new_sites_counter = conf.num_threads;
	__sync_synchronize();
	unsigned idx;
	for (idx = 0; idx < conf.num_threads; ++idx) {
		stubborn_dispatch_to_http_add_site(&conf.thread_ctxs[idx],
			hot_reload_add_remove_sites_in_http_thr, &new_site);
	}
	for (; idx < MAX_threads; ++idx) {
		hot_reload_add_remove_sites_in_tx_thr(&conf.thread_ctxs[idx],
			&new_site);
	}
	while (conf.add_new_sites_counter != 0)
		fiber_sleep(0.001);
	flush_lua_ref_handlers();
}

/* Launched in TX thread. */
static void
prepare_thread_ctx(unsigned thread_idx)
{
	thread_ctx_t *const thread_ctx = &conf.thread_ctxs[thread_idx];
	thread_ctx->idx = thread_idx;
	thread_ctx->listeners_created = 0;
	thread_ctx->num_connections = 0;
	thread_ctx->active_lua_fibers = 0;
	thread_ctx->fiber_to_wake_on_shutdown = NULL;
	thread_ctx->http_and_tx_lua_handlers_flushed = false;
}

/* Launched in TX thread. */
static const char *hot_reload_add_threads(unsigned threads)
{
	const char *lerr = NULL;
	unsigned xtm_to_tx_idx;
	for (xtm_to_tx_idx = conf.num_threads; xtm_to_tx_idx < threads;
	    ++xtm_to_tx_idx)
		if ((conf.thread_ctxs[xtm_to_tx_idx].queue_to_tx =
		    xtm_create(QUEUE_TO_TX_ITEMS)) == NULL) {
			lerr = "Failed to create xtm queue";
			goto add_thr_xtm_to_tx_fail;
		}

	unsigned fiber_idx;
	for (fiber_idx = conf.num_threads; fiber_idx < threads; ++fiber_idx) {
		reset_thread_ctx(fiber_idx);

		char name[32];
		sprintf(name, "tx_h2o_fiber_%u", fiber_idx);
		if ((conf.tx_fiber_ptrs[fiber_idx] =
		    fiber_new(name, tx_fiber_func)) == NULL) {
			lerr = "Failed to create fiber";
			goto add_thr_fibers_fail;
		}
		fiber_set_joinable(conf.tx_fiber_ptrs[fiber_idx], true);
		fiber_start(conf.tx_fiber_ptrs[fiber_idx], fiber_idx);
	}

	unsigned thr_init_idx;
	for (thr_init_idx = conf.num_threads; thr_init_idx < threads;
	    ++thr_init_idx)
		if (!init_worker_thread(thr_init_idx)) {
			lerr = "Failed to init worker threads";
			goto add_thr_threads_init_fail;
		}

	__sync_synchronize();

	/* Start processing HTTP requests and requests from TX thread. */
	unsigned thr_launch_idx;
	for (thr_launch_idx = conf.num_threads; thr_launch_idx < threads;
	    ++thr_launch_idx) {
		thread_ctx_t *const thread_ctx =
			&conf.thread_ctxs[thr_launch_idx];
		httpng_sem_init(&thread_ctx->can_be_terminated, 0);
		if (pthread_create(&thread_ctx->tid,
		    NULL, worker_func, (void *)(uintptr_t)thr_launch_idx)) {
			lerr = "Failed to launch worker threads";
			goto add_thr_threads_launch_fail;
		}
	}

	return lerr;

add_thr_threads_launch_fail:
	;
	unsigned idx;
	for (idx = conf.num_threads; idx < thr_launch_idx; ++idx)
		tell_thread_to_terminate(&conf.thread_ctxs[idx]);

	for (idx = conf.num_threads; idx < thr_launch_idx; ++idx)
		pthread_join(conf.thread_ctxs[idx].tid, NULL);

add_thr_threads_init_fail:
	for (idx = conf.num_threads; idx < thr_init_idx; ++idx)
		deinit_worker_thread(idx);

add_thr_fibers_fail:
	for (idx = conf.num_threads; idx < fiber_idx; ++idx) {
		conf.thread_ctxs[idx].tx_fiber_should_exit = true;
		__sync_synchronize();
		fiber_cancel(conf.tx_fiber_ptrs[idx]);
	}
	for (idx = conf.num_threads; idx < fiber_idx; ++idx) {
		const thread_ctx_t *const thread_ctx = &conf.thread_ctxs[idx];
		while (!thread_ctx->tx_fiber_finished)
			fiber_sleep(0.001);
		assert(thread_ctx->tx_fiber_finished);
	}

add_thr_xtm_to_tx_fail:
	for (idx = conf.num_threads; idx < xtm_to_tx_idx; ++idx)
		xtm_delete(conf.thread_ctxs[idx].queue_to_tx);

	return lerr;
}

/* Launched in TX thread. */
static void hot_reload_remove_threads(unsigned threads)
{
	if (threads >= conf.num_threads)
		return;

	conf.num_desired_threads = threads;

	unsigned thr_idx;
	for (thr_idx = threads; thr_idx < conf.num_threads; ++thr_idx) {
		thread_ctx_t *const thread_ctx = &conf.thread_ctxs[thr_idx];
		tell_thread_to_terminate_gracefully(thread_ctx);
	}
}

/* Lua parameters: lua_sites, function_to_call, function_param */
int cfg(lua_State *L)
{
	const char *lerr = NULL; /* Error message for caller. */
	bool is_hot_reload;
	assert(!conf.cfg_in_progress);
	conf.cfg_in_progress = true;
	if (conf.is_shutdown_in_progress) {
		lerr = "shutdown is in progress";
		goto error_something;
	}
	if (conf.configured) {
		if (conf.hot_reload_in_progress) {
			lerr = "Reconfiguration is already in progress";
			goto error_something;
		}
		if ((lerr = reap_gracefully_terminating_threads()) != NULL)
			goto error_something;
		conf.hot_reload_in_progress = true;
		is_hot_reload = true;
	} else
		is_hot_reload = false;
	unsigned c_handlers = 0;
	const int prev_idx_of_root_site = conf.idx_of_root_site;

	if (lua_gettop(L) < 1) {
		lerr = "No parameters specified";
		goto error_no_parameters;
	}

	lua_getfield(L, LUA_STACK_IDX_TABLE, "c_sites_func");
	const path_desc_t *path_descs;
	if (lua_isnil(L, -1)) {
		path_descs = NULL;
		goto Skip_c_sites;
	}
	if (is_hot_reload) {
		lerr = "Reconfiguration can't be used with C handlers";
		goto error_hot_reload_c;
	}

	if (lua_type(L, -1) != LUA_TFUNCTION) {
		lerr = "c_sites_func must be a function";
		goto error_c_sites_func_not_a_function;
	}

	lua_getfield(L, LUA_STACK_IDX_TABLE, "c_sites_func_param");
	if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
		lerr = "c_sites_func() failed";
		goto error_c_sites_func_failed;
	}

	int is_integer;
	if (!lua_islightuserdata(L, -1)) {
		lerr = "c_sites_func() returned wrong data type";
		goto error_c_sites_func_wrong_return;
	}
	path_descs = (path_desc_t *)lua_touserdata(L, -1);
Skip_c_sites:

#define PROCESS_OPTIONAL_PARAM(name) \
	lua_getfield(L, LUA_STACK_IDX_TABLE, #name); \
	uint64_t name; \
	if (lua_isnil(L, -1)) \
		name = DEFAULT_##name; \
	else { \
		name = my_lua_tointegerx(L, -1, &is_integer); \
		if (!is_integer) { \
			lerr = "parameter " #name " is not a number"; \
			goto error_parameter_not_a_number; \
		} \
		if (name > MAX_##name) { \
			name = MAX_##name; \
			fprintf(stderr, "Warning: parameter \"" #name "\" " \
				"adjusted to %llu (upper limit)\n", \
				(unsigned long long)name); \
		} else if (name < MIN_##name) { \
			name = MIN_##name; \
			fprintf(stderr, "Warning: parameter \"" #name "\" " \
				"adjusted to %llu (lower limit)\n", \
				(unsigned long long)name); \
		} \
	}

	/* N. b.: These macros can use goto. */
	PROCESS_OPTIONAL_PARAM(threads);
	PROCESS_OPTIONAL_PARAM(max_conn_per_thread);
	PROCESS_OPTIONAL_PARAM(shuttle_size);
	PROCESS_OPTIONAL_PARAM(max_body_len);

#undef PROCESS_OPTIONAL_PARAM

#ifdef SPLIT_LARGE_BODY
	lua_getfield(L, LUA_STACK_IDX_TABLE, "use_body_split");
	const bool use_body_split =
		lua_isnil(L, -1) ? false : lua_toboolean(L, -1);
#endif /* SPLIT_LARGE_BODY */

	/* FIXME: Add sanity checks, especially shuttle_size -
	 * it must >sizeof(shuttle_t) (accounting for Lua payload)
	 * and aligned. */
	if (is_hot_reload) {
		if (conf.shuttle_size != shuttle_size) {
			lerr = "Reconfiguration can't change shuttle_size";
			goto error_hot_reload_shuttle_size;
		}
	} else {
		conf.num_desired_threads = conf.num_threads = threads;
		conf.shuttle_size = shuttle_size;
	}

	/* FIXME: Can differ from shuttle_size. */
	conf.recv_data_size = shuttle_size;

	conf.max_headers_lua = (conf.shuttle_size - sizeof(shuttle_t) -
		offsetof(lua_response_t, un.resp.first.headers)) /
		sizeof(http_header_entry_t);
	conf.max_path_len_lua = conf.shuttle_size - sizeof(shuttle_t) -
		offsetof(lua_response_t, un.req.buffer);
	conf.max_recv_bytes_lua_websocket = conf.recv_data_size -
		(uintptr_t)get_websocket_recv_location(NULL);

	if (is_hot_reload)
		goto Skip_inits_on_hot_reload;

	if ((conf.thread_ctxs = (thread_ctx_t *)malloc(MAX_threads *
	    sizeof(thread_ctx_t))) == NULL) {
		lerr = "Failed to allocate memory for thread contexts";
		goto thread_ctxs_alloc_failed;
	}

	unsigned idx;
	for (idx = 0; idx < MAX_threads; ++idx) {
		prepare_thread_ctx(idx);
	}

	unsigned config_init_idx;
	for (config_init_idx = 0; config_init_idx < MAX_threads;
	    ++config_init_idx) {
		thread_ctx_t *const thread_ctx =
			&conf.thread_ctxs[config_init_idx];
		h2o_config_init(&thread_ctx->globalconf);
		/* FIXME: Should make customizable. */
		if ((thread_ctx->hostconf =
		    h2o_config_register_host(&thread_ctx->globalconf,
			    h2o_iovec_init(H2O_STRLIT("default")),
			    H2O_DEFAULT_PORT_FOR_PROTOCOL_USED)) == NULL) {
			lerr = "libh2o host registration failed";
			goto register_host_failed;
		}
	}

	if (path_descs != NULL) {
		const path_desc_t *path_desc = path_descs;
		if (path_desc->path == NULL) {
			/* Need at least one. */
			lerr = "Empty C sites list";
			goto c_desc_empty;
		}

		do {
			unsigned thread_idx;
			for (thread_idx = 0; thread_idx < MAX_threads;
			    ++thread_idx)
				register_handler(conf.thread_ctxs[thread_idx]
					.hostconf, path_desc->path,
					path_desc->handler);
			++c_handlers;
		} while ((++path_desc)->path != NULL);
	}

Skip_inits_on_hot_reload:
	;
	lua_site_t *lua_sites = NULL;
	unsigned hot_reload_extra_sites = 0;
	lua_getfield(L, LUA_STACK_IDX_TABLE, "sites");
	unsigned lua_site_count = 0;
	const unsigned generation = ++conf.generation;
	if (lua_isnil(L, -1))
		goto Skip_lua_sites;
	if (!lua_istable(L, -1)) {
		lerr = "sites is not a table";
		goto invalid_sites_table;
	}
	lua_pushnil(L); /* Start of table. */
	while (lua_next(L, LUA_STACK_IDX_LUA_SITES)) {
		bool is_adding_site;
		if (!lua_istable(L, -1)) {
			lerr = "sites is not a table of tables";
			goto invalid_sites;
		}
		lua_getfield(L, -1, "path");
		if (lua_isnil(L, -1)) {
			lerr = "sites[].path is nil";
			goto invalid_sites;
		}
		if (!lua_isstring_strict(L, -1)) {
			/* Numbers are converted automatically,
			 * we do not want that. */
			lerr = "sites[].path is not a string";
			goto invalid_sites;
		}
		size_t path_len;
		const char *const path = lua_tolstring(L, -1, &path_len);
		if (path == NULL) {
			lerr = "sites[].path is not a string";
			goto invalid_sites;
		}

		lua_site_t *lua_site;
		unsigned lua_site_idx;
		if (is_hot_reload) {
			for (lua_site_idx = 0;
			    lua_site_idx < conf.lua_site_count;
			    ++lua_site_idx) {
				const lua_site_t *const lua_site =
					&conf.lua_sites[lua_site_idx];
				if (path_len == lua_site->path_len &&
				    !memcmp(lua_site->path, path, path_len)) {
					if (lua_site->generation ==
					    generation -
					    ADD_NEW_SITE_GENERATION_SHIFT) {
						lerr =
						"Can't add duplicate paths";
						goto invalid_sites;
					}
					is_adding_site = false;
					goto Skip_creating_sites_structs;
				}
			}
			for (; lua_site_idx < conf.lua_site_count +
			    hot_reload_extra_sites; ++lua_site_idx) {
				const lua_site_t *const lua_site =
					&conf.lua_sites[lua_site_idx];
				if (path_len == lua_site->path_len &&
				    !memcmp(lua_site->path, path, path_len)) {
					lerr = "Can't add duplicate paths";
					goto invalid_sites;
				}
			}
			lua_site_t *const new_lua_sites =
				(lua_site_t *)realloc(conf.lua_sites,
					sizeof(lua_site_t) *
					(conf.lua_site_count +
						hot_reload_extra_sites + 1));
			if (new_lua_sites == NULL)
				goto error_lua_sites_malloc;
			conf.lua_sites = new_lua_sites;
			const int created_entry_idx = conf.lua_site_count +
                                hot_reload_extra_sites++;
			lua_site = &conf.lua_sites[created_entry_idx];
			lua_site->generation =
				generation - ADD_NEW_SITE_GENERATION_SHIFT;
			if (conf.idx_of_root_site >= 0) {
				/* Swap entries. */
				lua_site_t *const old_root =
					&conf.lua_sites[conf.idx_of_root_site];
				const lua_site_t tmp = *old_root;
				*old_root = *lua_site;
				*lua_site = tmp;
				lua_site = old_root;
				conf.idx_of_root_site = created_entry_idx;
			} else if (path_len == 1 && *path == '/')
				conf.idx_of_root_site = created_entry_idx;
			is_adding_site = true;
			goto Alloc_lua_site_path;
		}

		if (path_len == 1 && *path == '/') {
			if (conf.idx_of_root_site >= 0) {
				lerr = "There can be only one \"/\"";
				goto invalid_sites;
			}
			conf.idx_of_root_site = lua_site_count;
		} else if (conf.idx_of_root_site >= 0) {
			/* FIXME: Move root instead? */
			lerr = "Can't add other paths after adding \"/\"";
			goto invalid_sites;
		} else {
			for (lua_site_idx = 0;
			    lua_site_idx < lua_site_count;
			    ++lua_site_idx) {
				const lua_site_t *const lua_site =
					&lua_sites[lua_site_idx];
				if (path_len == lua_site->path_len &&
				    !memcmp(lua_site->path, path, path_len)) {
					lerr = "Can't add duplicate paths";
					goto invalid_sites;
				}
			}
		}
		lua_site_t *const new_lua_sites =
			(lua_site_t *)realloc(lua_sites, sizeof(lua_site_t) *
				(lua_site_count + 1));
		if (new_lua_sites == NULL) {
		error_lua_sites_malloc:
			lerr = "Failed to allocate memory "
				"for Lua sites C array";
			goto invalid_sites;
		}
		lua_sites = new_lua_sites;
		lua_site = &lua_sites[lua_site_count++];

	Alloc_lua_site_path:
		lua_site->lua_handler_ref = LUA_REFNIL;
		if ((lua_site->path = (char *)malloc(path_len + 1)) == NULL) {
			lerr = "Failed to allocate memory "
				"for Lua sites C array path";
			goto invalid_sites;
		}
		lua_site->path_len = path_len;

	Skip_creating_sites_structs:
		lua_getfield(L, -2, "handler");
		if (lua_type(L, -1) != LUA_TFUNCTION) {
			lerr = "sites[].handler is not a function";
			goto invalid_sites;
		}

		if (is_hot_reload) {
			if (is_adding_site)
				register_lua_handler_part_one(lua_site,
					path, luaL_ref(L, LUA_REGISTRYINDEX));
			else {
				lua_site = &conf.lua_sites[lua_site_idx];
				if (lua_site->generation == generation) {
					lerr = "duplicated site description";
					goto invalid_sites;
				}
				lua_site->new_lua_handler_ref =
					luaL_ref(L, LUA_REGISTRYINDEX);
				lua_site->generation = generation;
			}
		} else {
			register_lua_handler(lua_site, path,
				luaL_ref(L, LUA_REGISTRYINDEX));
			lua_site->generation = generation;
		}

		/* Remove path string and site array value,
		 * keep key for next iteration. */
		lua_pop(L, 2);
	}

Skip_lua_sites:
	lua_getfield(L, LUA_STACK_IDX_TABLE, "handler");
	if (lua_isnil(L, -1)) {
		if (is_hot_reload)
			goto Apply_new_config_hot_reload;
		goto Skip_main_lua_handler;
	}
	if (lua_type(L, -1) != LUA_TFUNCTION) {
		lerr = "handler is not a function";
		goto invalid_handler;
	}

	if (is_hot_reload) {
		if (conf.idx_of_root_site >= 0)
			goto Primary_handler_found;

		lua_site_t *const new_lua_sites =
			(lua_site_t *)realloc(conf.lua_sites,
				sizeof(lua_site_t) * (conf.lua_site_count +
					hot_reload_extra_sites + 1));
		if (new_lua_sites == NULL)
			goto error_lua_primary_site_malloc;
		conf.lua_sites = new_lua_sites;
		lua_site_t *lua_site =
			&conf.lua_sites[conf.lua_site_count +
				hot_reload_extra_sites++];
		lua_site->generation =
			generation - ADD_NEW_SITE_GENERATION_SHIFT;

		lua_site->lua_handler_ref = LUA_REFNIL;
		if ((lua_site->path = (char *)malloc(1 + 1)) == NULL)
			goto error_lua_primary_site_path_malloc;
		lua_site->path_len = 1;
		register_lua_handler_part_one(lua_site,
			"/", luaL_ref(L, LUA_REGISTRYINDEX));
		goto Apply_new_config_hot_reload;

	Primary_handler_found:
		lua_site = &conf.lua_sites[conf.idx_of_root_site];
		if (lua_site->generation == generation) {
			lerr = "duplicated site description for /";
			goto invalid_sites;
		}
		lua_site->new_lua_handler_ref = luaL_ref(L, LUA_REGISTRYINDEX);
		lua_site->generation = generation;

		/* FIXME: Actually we should check other parameters for sanity,
		 * not doing that for easier merging of multilisten. */
		//goto Skip_creating_primary_handler_structs;

		goto Apply_new_config_hot_reload;
	}

	if (conf.idx_of_root_site >= 0) {
		lerr = "There can be only one \"/\"";
		goto invalid_handler;
	}
	conf.idx_of_root_site = lua_site_count;

	lua_site_t *const new_lua_sites = (lua_site_t *)realloc(lua_sites,
		sizeof(lua_site_t) * (lua_site_count + 1));
	if (new_lua_sites == NULL) {
	error_lua_primary_site_malloc:
		lerr = "Failed to allocate memory for Lua sites C array";
		goto invalid_handler;
	}
	lua_sites = new_lua_sites;
	lua_site_t *const lua_site = &lua_sites[lua_site_count++];
	lua_site->lua_handler_ref = LUA_REFNIL;
	if ((lua_site->path = (char *)malloc(1 + 1)) == NULL) {
	error_lua_primary_site_path_malloc:
		lerr = "Failed to allocate memory for Lua sites C array path";
		goto invalid_handler;
	}
	lua_site->path_len = 1;
	register_lua_handler(lua_site, "/",
		luaL_ref(L, LUA_REGISTRYINDEX));
	lua_site->generation = generation;

//Skip_creating_primary_handler_structs:
Skip_main_lua_handler:
	if (c_handlers + lua_site_count == 0) {
		lerr = "No handlers specified";
		goto no_handlers;
	}
	if (lua_site_count != 0 &&
	    shuttle_size < sizeof(shuttle_t) + sizeof(lua_response_t)) {
		lerr = "shuttle_size is too small for Lua handlers";
		goto no_handlers;
	}
	unsigned short port = DEFAULT_LISTEN_PORT;
	lua_getfield(L, LUA_STACK_IDX_TABLE, "listen");
	if (lua_isnil(L, -1))
		goto Skip_listen;
	if (lua_type(L, -1) != LUA_TTABLE) {
		lerr = "listen is not a table";
		goto listen_invalid;
	}

	lua_pushnil(L); /* Start of table. */
	while (lua_next(L, -2)) {
		if (!lua_istable(L, -1)) {
			lerr = "listen is not a table of tables";
			goto listen_invalid;
		}
		lua_getfield(L, -1, "port");
		int is_integer;
		const uint64_t candidate =
			my_lua_tointegerx(L, -1, &is_integer);
		if (!is_integer || !candidate || candidate >= 65535) {
			lerr = "invalid port specified";
			goto listen_invalid;
		}
		/* Silently overwrite for now (FIXME: Multilisten). */
		port = candidate;
		/* Remove port and value, keep key for next iteration. */
		lua_pop(L, 2);
	}

Skip_listen:
	;
	long min_proto_version;
	lua_getfield(L, LUA_STACK_IDX_TABLE, "min_proto_version");
	if (lua_isnil(L, -1)) {
		min_proto_version = TLS1_2_VERSION;
		fprintf(stderr, "Using default min_proto_version=tls1.2\n");
		goto Skip_min_proto_version;
	}

	if (!lua_isstring_strict(L, -1)) {
		lerr = "min_proto_version is not a string";
		goto min_proto_version_invalid;
	}
	size_t min_proto_version_len;
	const char *const min_proto_version_str =
		lua_tolstring(L, -1, &min_proto_version_len);
	if (min_proto_version_str == NULL) {
		lerr = "min_proto_version is not a string";
		goto min_proto_version_invalid;
	}

#define FILL_PROTO_STR(name, value) \
	{ (name), sizeof(name) - 1, (value) }

	{
		struct {
			char str[8];
			size_t len;
			long num;
		} protos[] = {
			FILL_PROTO_STR(SSL3_STR, SSL3_VERSION),
			FILL_PROTO_STR(TLS1_STR, TLS1_VERSION),
			FILL_PROTO_STR(TLS1_0_STR, TLS1_VERSION),
			FILL_PROTO_STR(TLS1_1_STR, TLS1_1_VERSION),
			FILL_PROTO_STR(TLS1_2_STR, TLS1_2_VERSION),
#ifdef TLS1_3_VERSION
			FILL_PROTO_STR(TLS1_3_STR, TLS1_3_VERSION),
#endif /* TLS1_3_VERSION */
		};
#undef FILL_PROTO_STR
		unsigned idx;
		for (idx = 0; idx < lengthof(protos); ++idx) {
			if (protos[idx].len == min_proto_version_len &&
			    !memcmp(&protos[idx].str, min_proto_version_str,
			    min_proto_version_len)) {
				min_proto_version = protos[idx].num;
				goto Proto_found;
			}
		}
		/* This is security, do not silently fall back to default. */
		lerr = "unknown min_proto_version specified";
		goto min_proto_version_invalid;
	Proto_found:
		;
	}

Skip_min_proto_version:
	lua_getfield(L, LUA_STACK_IDX_TABLE, "openssl_security_level");
	uint64_t openssl_security_level;
	if (lua_isnil(L, -1)) {
		openssl_security_level = 1;
		goto Skip_openssl_security_level;
	}
	openssl_security_level = my_lua_tointegerx(L, -1, &is_integer);
	if (!is_integer) {
		lerr = "openssl_security_level is not a number";
		goto invalid_openssl_security_level;
	}
	if (openssl_security_level > 5) {
		lerr = "openssl_security_level is invalid";
		goto invalid_openssl_security_level;
	}

Skip_openssl_security_level:
	;
	SSL_CTX *ssl_ctx;
	/* FIXME: Should use customizable file names. */
	if (USE_HTTPS) {
		if ((ssl_ctx = setup_ssl("examples/cert.pem",
		    "examples/key.pem", openssl_security_level,
		    min_proto_version)) == NULL) {
			lerr = "setup_ssl() failed (cert/key files not found?)";
			goto ssl_fail;
		}
	} else
		ssl_ctx = NULL;

#if 0
	/* FIXME: Should make customizable. */
	/* Never returns NULL. */
	h2o_logger_t *logger = h2o_access_log_register(&config.default_host,
		"/dev/stdout", NULL);
#endif

	conf.ssl_ctx = ssl_ctx;

	/* TODO: Implement more than one listener (HTTP/HTTPS,
	 * IPv4/IPv6, several IPs etc.) */
	conf.num_listeners = 1; /* FIXME: Make customizable. */
	if ((conf.listener_cfgs = (listener_cfg_t *)
	    calloc(conf.num_listeners, sizeof(listener_cfg_t))) == NULL) {
		lerr = "Failed to allocate memory for listener cfgs";
		goto listeners_alloc_fail;
	}

	{
		unsigned listener_idx;

		for (listener_idx = 0; listener_idx < conf.num_listeners;
		    ++listener_idx) {
			/* FIXME: Make customizable. */
			const int fd = open_listener_ipv4("0.0.0.0", port);
			if (fd < 0) {
				lerr = "Failed to listen";
				goto listeners_fail;
			}
			register_listener_cfgs_socket(fd, 0);
		}
	}

	if ((conf.tx_fiber_ptrs = (struct fiber **)
	    malloc(sizeof(struct fiber *) * MAX_threads)) == NULL) {
		lerr = "Failed to allocate memory for fiber pointers array";
		goto fibers_fail_alloc;
	}

	unsigned xtm_to_tx_idx;
	for (xtm_to_tx_idx = 0; xtm_to_tx_idx < conf.num_threads;
	    ++xtm_to_tx_idx)
		if ((conf.thread_ctxs[xtm_to_tx_idx].queue_to_tx =
		    xtm_create(QUEUE_TO_TX_ITEMS)) == NULL) {
			lerr = "Failed to create xtm queue";
			goto xtm_to_tx_fail;
		}

	unsigned fiber_idx;
	for (fiber_idx = 0; fiber_idx < conf.num_threads; ++fiber_idx) {
		char name[32];
		sprintf(name, "tx_h2o_fiber_%u", fiber_idx);
		if ((conf.tx_fiber_ptrs[fiber_idx] =
		    fiber_new(name, tx_fiber_func)) == NULL) {
			lerr = "Failed to create fiber";
			goto fibers_fail;
		}
		reset_thread_ctx(fiber_idx);
		fiber_set_joinable(conf.tx_fiber_ptrs[fiber_idx], true);
		fiber_start(conf.tx_fiber_ptrs[fiber_idx], fiber_idx);
	}

	if (path_descs != NULL) {
		const path_desc_t *path_desc = path_descs;
		do {
			if (path_desc->init_userdata_in_tx != NULL &&
			    path_desc->init_userdata_in_tx(
				    path_desc->init_userdata_in_tx_param)) {
				lerr = "Failed to init userdata";
				goto userdata_init_fail;
			}
		} while ((++path_desc)->path != NULL);
	}

	unsigned thr_init_idx;
	for (thr_init_idx = 0; thr_init_idx < conf.num_threads;
	    ++thr_init_idx)
		if (!init_worker_thread(thr_init_idx)) {
			lerr = "Failed to init worker threads";
			goto threads_init_fail;
		}

	goto Apply_new_config;

After_applying_new_config:
	__sync_synchronize();

	/* Start processing HTTP requests and requests from TX thread. */
	unsigned thr_launch_idx;
	for (thr_launch_idx = 0; thr_launch_idx < conf.num_threads;
	    ++thr_launch_idx) {
		thread_ctx_t *const thread_ctx =
			&conf.thread_ctxs[thr_launch_idx];
		httpng_sem_init(&thread_ctx->can_be_terminated, 0);
		if (pthread_create(&thread_ctx->tid,
		    NULL, worker_func, (void *)(uintptr_t)thr_launch_idx)) {
			lerr = "Failed to launch worker threads";
			goto threads_launch_fail;
		}
	}

	conf.lua_sites = lua_sites;
	conf.lua_site_count = lua_site_count;
	if (!conf.is_on_shutdown_setup)
		setup_on_shutdown(L, true, false);
	conf.configured = true;
	conf.cfg_in_progress = false;
	return 0;

Apply_new_config_hot_reload:
	if (threads > conf.num_threads) {
		lerr = hot_reload_add_threads(threads);
		if (lerr != NULL)
			goto failed_to_add_threads;
		conf.num_desired_threads = conf.num_threads = threads;
	}

	hot_reload_add_remove_sites(hot_reload_extra_sites);

	unsigned removed_sites = 0;
	for (idx = 0; idx < conf.lua_site_count + hot_reload_extra_sites -
	    removed_sites;) {
		lua_site_t *const lua_site = &conf.lua_sites[idx];
		if (lua_site->generation != generation &&
		    lua_site->generation != generation -
			    ADD_NEW_SITE_GENERATION_SHIFT) {
			if (conf.idx_of_root_site == idx)
				conf.idx_of_root_site = -1;
			free(lua_site->path);
			luaL_unref(L, LUA_REGISTRYINDEX,
				lua_site->lua_handler_ref);
			memmove(lua_site, lua_site + 1, (conf.lua_site_count
				+ hot_reload_extra_sites - ++removed_sites - idx)
				* sizeof(*lua_site));
			if (conf.idx_of_root_site >= 0) {
				if (conf.idx_of_root_site > idx)
					--conf.idx_of_root_site;
				else if (conf.idx_of_root_site == idx)
					conf.idx_of_root_site = -1;
			}
		} else
			++idx;
	}
	if (removed_sites > 0) {
		size_t new_size = (conf.lua_site_count
			+ hot_reload_extra_sites
			- removed_sites) * sizeof(lua_site_t);
		if (new_size == 0)
			new_size = 1; /* realloc(0) frees block. */
		lua_site_t *const new_lua_sites =
			(lua_site_t *)realloc(conf.lua_sites, new_size);
		if (new_lua_sites != NULL)
			conf.lua_sites = new_lua_sites;
	}

Apply_new_config:
	conf.use_body_split = use_body_split;
	conf.max_conn_per_thread = max_conn_per_thread;
#ifndef USE_LIBUV
	conf.num_accepts = max_conn_per_thread / 16;
	if (conf.num_accepts < 8)
		conf.num_accepts = 8;
#endif /* USE_LIBUV */
	unsigned thread_idx;
	for (thread_idx = 0; thread_idx < conf.num_threads; ++thread_idx)
		conf.thread_ctxs[thread_idx].globalconf
			.max_request_entity_size = max_body_len;

	if (!is_hot_reload)
		goto After_applying_new_config;

	conf.lua_site_count += hot_reload_extra_sites;
	conf.lua_site_count -= removed_sites;
	replace_lua_handlers(L);
	hot_reload_remove_threads(threads);

	conf.hot_reload_in_progress = false;
	conf.cfg_in_progress = false;
	return 0;

threads_launch_fail:
	for (idx = 0; idx < thr_launch_idx; ++idx)
		tell_thread_to_terminate(&conf.thread_ctxs[idx]);

	for (idx = 0; idx < thr_launch_idx; ++idx)
		pthread_join(conf.thread_ctxs[idx].tid, NULL);

threads_init_fail:
	for (idx = 0; idx < thr_init_idx; ++idx)
		deinit_worker_thread(idx);
userdata_init_fail:
fibers_fail:
	for (idx = 0; idx < fiber_idx; ++idx) {
		conf.thread_ctxs[idx].tx_fiber_should_exit = true;
		__sync_synchronize();
		fiber_cancel(conf.tx_fiber_ptrs[idx]);
	}
	for (idx = 0; idx < fiber_idx; ++idx) {
		const thread_ctx_t *const thread_ctx = &conf.thread_ctxs[idx];
		while (!thread_ctx->tx_fiber_finished)
			fiber_sleep(0.001);
		assert(thread_ctx->tx_fiber_finished);
	}

xtm_to_tx_fail:
	for (idx = 0; idx < xtm_to_tx_idx; ++idx)
		xtm_delete(conf.thread_ctxs[idx].queue_to_tx);

	free(conf.tx_fiber_ptrs);

fibers_fail_alloc:
listeners_fail:
	close_listener_cfgs_sockets();
	free(conf.listener_cfgs);
listeners_alloc_fail:
	SSL_CTX_free(ssl_ctx);
ssl_fail:
invalid_openssl_security_level:
min_proto_version_invalid:
listen_invalid:
no_handlers:
failed_to_add_threads:
invalid_handler:
invalid_sites:
	if (is_hot_reload) {
		if (prev_idx_of_root_site != conf.idx_of_root_site) {
			assert(conf.idx_of_root_site >= conf.lua_site_count);
			/* Move root site back. */
			const lua_site_t tmp =
				conf.lua_sites[conf.idx_of_root_site];
			conf.lua_sites[conf.idx_of_root_site] =
				conf.lua_sites[prev_idx_of_root_site];
			conf.lua_sites[prev_idx_of_root_site] = tmp;
			conf.idx_of_root_site = prev_idx_of_root_site;
		}
		for (idx = 0; idx < conf.lua_site_count +
		    hot_reload_extra_sites; ++idx) {
			lua_site_t *const lua_site = &conf.lua_sites[idx];
			if (lua_site->generation == generation)
				luaL_unref(L, LUA_REGISTRYINDEX,
					lua_site->new_lua_handler_ref);
			else {
				if (lua_site->generation == generation -
				    ADD_NEW_SITE_GENERATION_SHIFT) {
					if (lua_site->lua_handler_ref !=
					    LUA_REFNIL)
						luaL_unref(L, LUA_REGISTRYINDEX,
						    lua_site->lua_handler_ref);
					free(lua_site->path);
				}
				lua_site->generation = generation;
			}
		}
		if (hot_reload_extra_sites) {
			lua_site_t *const new_lua_sites =
				(lua_site_t *)realloc(conf.lua_sites,
				sizeof(lua_site_t) * conf.lua_site_count);
			if (new_lua_sites != NULL)
				conf.lua_sites = new_lua_sites;
			/* We can complain about failed realloc(smaller),
			 * but what is the point? */
		}
	} else for (idx = 0; idx < lua_site_count; ++idx) {
		lua_site_t *const lua_site = &lua_sites[idx];
		if (lua_site->lua_handler_ref != LUA_REFNIL)
			luaL_unref(L, LUA_REGISTRYINDEX,
				lua_site->lua_handler_ref);
		free(lua_site->path);
	}
	free(lua_sites);
invalid_sites_table:
c_desc_empty:
register_host_failed:
	/* N.b.: h2o currently can't "unregister" host(s). */
	if (!is_hot_reload) {
		for (thread_idx = 0; thread_idx < config_init_idx;
		    ++thread_idx)
			h2o_config_dispose(&conf.thread_ctxs[thread_idx]
				.globalconf);
		free(conf.thread_ctxs);
	}
thread_ctxs_alloc_failed:
error_hot_reload_shuttle_size:
error_parameter_not_a_number:
error_c_sites_func_wrong_return:
error_c_sites_func_failed:
error_c_sites_func_not_a_function:
error_hot_reload_c:
error_no_parameters:
	if (prev_idx_of_root_site != conf.idx_of_root_site) {
		assert(!is_hot_reload);
		assert(prev_idx_of_root_site < 0);
		conf.idx_of_root_site = -1;
	}
	assert(lerr != NULL);
	conf.hot_reload_in_progress = false;
error_something:
	conf.cfg_in_progress = false;
	return luaL_error(L, lerr);
}

unsigned get_shuttle_size(void)
{
	assert(conf.shuttle_size >= MIN_shuttle_size);
	assert(conf.shuttle_size <= MAX_shuttle_size);
	return conf.shuttle_size;
}

/* Launched in TX thread. */
static int
force_decrease_threads(lua_State *L)
{
	if (!conf.configured) {
		return luaL_error(L,
			"Not configured, nothing to terminate");
	}

	reap_terminating_threads_ungracefully();
	return 0;
}

static const struct luaL_Reg mylib[] = {
	{"cfg", cfg},
	{"shutdown", on_shutdown_for_user},
	{"force_decrease_threads", force_decrease_threads},
	{NULL, NULL}
};

int luaopen_httpng(lua_State *L)
{
	luaL_newlib(L, mylib);
	return 1;
}
