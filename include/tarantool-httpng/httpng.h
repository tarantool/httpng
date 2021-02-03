#ifndef H2O_MODULE_H_INCLUDED
#define H2O_MODULE_H_INCLUDED

#ifdef USE_LIBUV
#define H2O_USE_LIBUV 1
#else
#define H2O_USE_EPOLL 1 /* FIXME */
#endif /* USE_LIBUV */
#include <h2o.h>

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

struct listener_ctx;

typedef struct thread_ctx {
	h2o_context_t ctx;
	struct listener_ctx *listener_ctxs;
	struct xtm_queue *queue_to_tx;
	struct xtm_queue *queue_from_tx;
	h2o_socket_t *sock_from_tx;
#ifdef USE_LIBUV
	uv_loop_t loop;
#endif /* USE_LIBUV */
	unsigned num_connections;
	unsigned idx;
	pthread_t tid;
} thread_ctx_t;

struct anchor;
typedef struct {
	h2o_req_t *never_access_this_req_from_tx_thread;
	struct anchor *anchor;
	thread_ctx_t *thread_ctx;

	char disposed; /* never_access_this_req_from_tx_thread can only be used if disposed is false */
	char stopped; /* For use by handlers, initialized to false for new shuttles */
	char unused[sizeof(void *) - 2 * sizeof(char)];

	char payload[];
} shuttle_t;

typedef struct anchor {
	shuttle_t *shuttle;
	char should_free_shuttle;
} anchor_t;

/* Written directly into h2o_create_handler()->on_req */
typedef int (*req_handler_t)(h2o_handler_t *, h2o_req_t *);

typedef int (*init_userdata_in_tx_t)(void *); /* returns 0 on success */

typedef struct {
	const char *path;
	req_handler_t handler;
	init_userdata_in_tx_t init_userdata_in_tx;
	void *init_userdata_in_tx_param;
} path_desc_t;

typedef struct {
	unsigned num_threads;
	unsigned max_conn_per_thread;
	unsigned shuttle_size;
	path_desc_t path_descs[];
} site_desc_t;

extern __thread thread_ctx_t *curr_thread_ctx;

static inline thread_ctx_t *get_curr_thread_ctx(void)
{
	return curr_thread_ctx;
}

extern shuttle_t *prepare_shuttle(h2o_req_t *);
extern void free_shuttle(shuttle_t *, thread_ctx_t *);

#if defined(__cplusplus)
}
#endif /* defined(__cplusplus) */

#endif /* H2O_MODULE_H_INCLUDED */

