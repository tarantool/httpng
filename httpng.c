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

static h2o_pathconf_t *register_handler(h2o_hostconf_t *hostconf, const char *path, int (*on_req)(h2o_handler_t *, h2o_req_t *))
{
	/* These functions never return NULL, dying instead */
	h2o_pathconf_t *pathconf = h2o_config_register_path(hostconf, path, 0);
	h2o_handler_t *handler = h2o_create_handler(pathconf, sizeof(*handler));
	handler->on_req = on_req;
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

/* Lua parameters: function_to_call, function_param */
int init(lua_State *L)
{
	memset(&conf.globalconf, 0, sizeof(conf.globalconf));

	if (lua_gettop(L) < 2)
		goto Error;

	if (lua_type(L, 1) != LUA_TFUNCTION)
		goto Error;

	if (lua_pcall(L, 1, 1, 0) != LUA_OK)
		goto Error;

	int is_integer;
	const site_desc_t *const site_desc = (site_desc_t *)lua_tointegerx(L, 1, &is_integer);
	if (!is_integer)
		goto Error;
	const path_desc_t *const path_descs = site_desc->path_descs;
	conf.tx_fiber_should_work = 1;
	/* FIXME: Add sanity checks, especially shuttle_size - it must >sizeof(shuttle_t) and aligned */
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
