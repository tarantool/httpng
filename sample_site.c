#define H2O_USE_EPOLL 1 /* FIXME */

#include <module.h>
#include <msgpuck/msgpuck.h>
#include <lauxlib.h>
#include <stddef.h>
#include "../../xtm/xtm_api.h"

#include <tarantool-httpng/httpng.h>

static const char users_path[] = "/users";
static const char stats_path[] = "/stats";
static const char large_path[] = "/large";
static const char fiber_path[] = "/fiber";
static unsigned users_response_data_size;
static unsigned stats_response_data_size_minus_1;
static unsigned large_response_data_size;
static unsigned fiber_response_data_size;

static struct {
	uint32_t space_id;
	uint32_t index_id;
	uint32_t large_space_id;
	uint32_t large_index_id;
} our_userdata; /* Site/path-specific */

typedef struct {
	uint32_t id;
} our_request_t;

#pragma pack(push, 1)
typedef struct {
	unsigned len;
	char data[];
} simple_response_t;
#pragma pack(pop)

typedef struct {
	box_iterator_t *iterator;
	h2o_generator_t generator;
	unsigned len;
	bool need_more; /* Processing in TX thread must be called again */
	char data[];
} response_with_state_t;

typedef struct {
	box_tuple_t *tuple; /* unref can only be done from TX thread */
	const char *direct_ptr; /* Initialized only if (tuple != NULL) */
	h2o_generator_t generator;
	unsigned len;
	char data[];
} large_response_t;

typedef struct {
	struct fiber *fiber; /* Should only be accessed from TX thread */
	unsigned len;
	bool fiber_done; /* Should only be accessed from TX thread */
	char data[];
} fiber_response_t;

static void continue_processing_stats_req_in_tx(shuttle_t *);
static void cancel_processing_stats_req_in_tx(shuttle_t *);

static inline shuttle_t *get_shuttle_from_generator(h2o_generator_t *generator)
{
	response_with_state_t *const response = container_of(generator, response_with_state_t, generator);
	return (shuttle_t *)((char *)response - offsetof(shuttle_t, payload));
}

/* Called when dispatch must not fail */
static void stubborn_dispatch(struct xtm_queue *queue, void (*func)(shuttle_t *), shuttle_t *shuttle)
{
	while (xtm_fun_dispatch(queue, (void(*)(void *))func, shuttle, 0)) {
		/* Error; we must not fail so retry a little later */
		fiber_sleep(0);
	}
}

/* Launched in HTTP server thread */
static void postprocess_users_req(shuttle_t *shuttle)
{
	if (shuttle->disposed) {
		free_shuttle(shuttle);
		return;
	}
	h2o_req_t *req = shuttle->never_access_this_req_from_tx_thread;
	static h2o_generator_t generator = {NULL, NULL};
	req->res.status = 200;
	req->res.reason = "OK";
	h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL, H2O_STRLIT("text/plain; charset=utf-8"));
	h2o_start_response(req, &generator);

	h2o_iovec_t buf;
	simple_response_t *const response = (simple_response_t *)(&shuttle->payload);
	buf.base = response->data;
	buf.len = response->len;
	shuttle->anchor->user_free_shuttle = &free_shuttle;
	h2o_send(req, &buf, 1, H2O_SEND_STATE_FINAL);
}

/* Launched in HTTP server thread when H2O has sent everything and asks for more */
static void proceed_sending_stats(h2o_generator_t *self, h2o_req_t *req)
{
	shuttle_t *const shuttle = get_shuttle_from_generator(self);
	thread_ctx_t *const thread_ctx = get_curr_thread_ctx();
	stubborn_dispatch(thread_ctx->queue_to_tx, &continue_processing_stats_req_in_tx, shuttle);
}

/* Launched in HTTP server thread when connection has been closed */
static void stop_sending_stats(h2o_generator_t *self, h2o_req_t *req)
{
	shuttle_t *const shuttle = get_shuttle_from_generator(self);
	response_with_state_t *const response = (response_with_state_t *)(&shuttle->payload);
	if (response->need_more && !shuttle->stopped) {
		shuttle->stopped = true;
		stubborn_dispatch(get_curr_thread_ctx()->queue_to_tx, &cancel_processing_stats_req_in_tx, shuttle);
	}
}

/* Launched in HTTP server thread to postprocess first response */
static void postprocess_stats_req_first(shuttle_t *shuttle)
{
	response_with_state_t *const response = (response_with_state_t *)(&shuttle->payload);
	if (shuttle->disposed) {
		if (response->need_more) {
			if (!shuttle->stopped) {
				shuttle->stopped = true;
				stubborn_dispatch(get_curr_thread_ctx()->queue_to_tx, &cancel_processing_stats_req_in_tx, shuttle);
			}
		} else
			free_shuttle(shuttle);
		return;
	}
	h2o_req_t *const req = shuttle->never_access_this_req_from_tx_thread;
	req->res.status = 200;
	req->res.reason = "OK";
	h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL, H2O_STRLIT("text/plain; charset=utf-8"));
	h2o_start_response(req, &response->generator);

	h2o_iovec_t buf;
	buf.base = response->data;
	buf.len = response->len;
	if (response->need_more) {
		h2o_send(req, &buf, 1, H2O_SEND_STATE_IN_PROGRESS);
	} else {
		shuttle->anchor->user_free_shuttle = &free_shuttle;
		h2o_send(req, &buf, 1, H2O_SEND_STATE_FINAL);
	}
}

/* Launched in HTTP server thread to postprocess responses other than first */
static void postprocess_stats_req_others(shuttle_t *shuttle)
{
	response_with_state_t *const response = (response_with_state_t *)(&shuttle->payload);
	if (shuttle->disposed) {
		if (response->need_more) {
			if (!shuttle->stopped) {
				shuttle->stopped = true;
				stubborn_dispatch(get_curr_thread_ctx()->queue_to_tx, &cancel_processing_stats_req_in_tx, shuttle);
			}
		} else
			free_shuttle(shuttle);
		return;
	}
	h2o_req_t *const req = shuttle->never_access_this_req_from_tx_thread;

	h2o_iovec_t buf;
	buf.base = response->data;
	buf.len = response->len;
	if (response->need_more) {
		h2o_send(req, &buf, 1, H2O_SEND_STATE_IN_PROGRESS);
	} else {
		shuttle->anchor->user_free_shuttle = &free_shuttle;
		h2o_send(req, &buf, 1, H2O_SEND_STATE_FINAL);
	}
}

/* Launched in TX thread; note that queue_from_tx is not created yet */
static int init_userdata_in_tx(void *param)
{
	(void)param;

	static const char space_name[] = "tester";
	if ((our_userdata.space_id = box_space_id_by_name(space_name, sizeof(space_name) - 1)) == BOX_ID_NIL)
		return -1;
	static const char index_name[] = "primary";
	if ((our_userdata.index_id = box_index_id_by_name(our_userdata.space_id, index_name, sizeof(index_name) - 1)) == BOX_ID_NIL)
		return -1;

	static const char large_space_name[] = "large";
	if ((our_userdata.large_space_id = box_space_id_by_name(large_space_name, sizeof(large_space_name) - 1)) == BOX_ID_NIL)
		return -1;
	static const char large_index_name[] = "primary";
	if ((our_userdata.large_index_id = box_index_id_by_name(our_userdata.large_space_id, large_index_name, sizeof(large_index_name) - 1)) == BOX_ID_NIL)
		return -1;

	return 0;
}

/* Launched in TX thread */
static void process_users_req_in_tx(shuttle_t *shuttle)
{
	simple_response_t *const response = (simple_response_t *)&shuttle->payload;

	const our_request_t *const our_req = (our_request_t *)&shuttle->payload;
	const unsigned entry_index = our_req->id;
	char entry_index_msgpack[16];
	char *key_end = mp_encode_array(entry_index_msgpack, 1);
	key_end = mp_encode_uint(key_end, entry_index);
	assert(key_end < &entry_index_msgpack[0] + sizeof(entry_index_msgpack));
	box_tuple_t *tuple;
	if (box_index_get(our_userdata.space_id, our_userdata.index_id, entry_index_msgpack, key_end, &tuple)) {
		static const char error_str[] = "Query error";
		memcpy(&response->data, error_str, sizeof(error_str) - 1);
		response->len = sizeof(error_str) - 1;
	} else if (tuple == NULL) {
		static const char error_str[] = "Entry not found";
		memcpy(&response->data, error_str, sizeof(error_str) - 1);
		response->len = sizeof(error_str) - 1;
	} else {
		const char *name_msgpack = box_tuple_field(tuple, 1);
		if (name_msgpack == NULL) {
			static const char error_str[] = "Invalid entry format";
			memcpy(&response->data, error_str, sizeof(error_str) - 1);
			response->len = sizeof(error_str) - 1;
		} else {
			uint32_t len;
			const char *const name = mp_decode_str(&name_msgpack, &len);
			if (len > users_response_data_size) {
				/* Real implementation should probably report error
				 * or use complex response logic to allocate and pass large buffers
				 * to send via HTTP(S) */
				len = users_response_data_size;
			}
			memcpy(&response->data, name, len);
			response->len = len;
		}
	}

	stubborn_dispatch(shuttle->thread_ctx->queue_from_tx, &postprocess_users_req, shuttle);
}

/* Launched in TX thread */
static void process_stats_req_in_tx(shuttle_t *shuttle)
{
	response_with_state_t *const response = (response_with_state_t *)&shuttle->payload;
	response->generator = (h2o_generator_t){ proceed_sending_stats, stop_sending_stats };

	char entry_index_msgpack[16];
	char *key_end = mp_encode_array(entry_index_msgpack, 1);
	key_end = mp_encode_uint(key_end, 0);
	assert(key_end < &entry_index_msgpack[0] + sizeof(entry_index_msgpack));

#define RETURN_WITH_ERROR(err) \
	do { \
		response->need_more = false; \
		static const char error_str[] = err; \
		memcpy(&response->data, error_str, sizeof(error_str) - 1); \
		response->len = sizeof(error_str) - 1; \
		stubborn_dispatch(shuttle->thread_ctx->queue_from_tx, &postprocess_stats_req_first, shuttle); \
		return; \
	} while (0)

#define RETURN_WITH_ERROR_FREE_IT(err) \
	do { \
		box_iterator_free(response->iterator); \
		RETURN_WITH_ERROR(err); \
	} while (0)

	if ((response->iterator = box_index_iterator(our_userdata.space_id, our_userdata.index_id, ITER_ALL, entry_index_msgpack, key_end)) == NULL) {
		RETURN_WITH_ERROR("Can't init iterator");
	}

	box_tuple_t *tuple;
	if (box_iterator_next(response->iterator, &tuple))
		RETURN_WITH_ERROR_FREE_IT("Iteration error");

	if (tuple == NULL)
		RETURN_WITH_ERROR_FREE_IT("No entries");

	const char *name_msgpack = box_tuple_field(tuple, 1);
	if (name_msgpack == NULL)
		RETURN_WITH_ERROR_FREE_IT("Invalid entry format");

	/* More efficient implementation is possible: if entry max size is known we can write more than one entry into shuttle_t */
	response->need_more = true;
	uint32_t len;
	const char *const name = mp_decode_str(&name_msgpack, &len);
	if (len > stats_response_data_size_minus_1) {
		/* Real implementation should probably report error
		 * or use complex response logic to allocate and pass large buffers
		 * to send via HTTP(S) */
		len = stats_response_data_size_minus_1;
	}
	memcpy(&response->data, name, len);
	response->data[len] = '\n';
	response->len = len + 1;
	stubborn_dispatch(shuttle->thread_ctx->queue_from_tx, &postprocess_stats_req_first, shuttle);
}
#undef RETURN_WITH_ERROR_FREE_IT
#undef RETURN_WITH_ERROR

/* Launched in HTTP server thread */
static void cancel_processing_stats_req_in_http_thread(shuttle_t *shuttle)
{
	assert(shuttle->disposed);
	free_shuttle(shuttle);
}

/* Launched in TX thread */
static void cancel_processing_stats_req_in_tx(shuttle_t *shuttle)
{
	response_with_state_t *const response = (response_with_state_t *)&shuttle->payload;

	box_iterator_free(response->iterator);

	/* Can't call free_shuttle() from TX thread because it [potentially] uses per-thread pools */
	stubborn_dispatch(shuttle->thread_ctx->queue_from_tx, &cancel_processing_stats_req_in_http_thread, shuttle);
}

/* Launched in TX thread */
static void continue_processing_stats_req_in_tx(shuttle_t *shuttle)
{
	response_with_state_t *const response = (response_with_state_t *)&shuttle->payload;

#define RETURN_WITH_ERROR_FREE_IT(err) \
	do { \
		box_iterator_free(response->iterator); \
		response->need_more = false; \
		static const char error_str[] = err; \
		memcpy(&response->data, error_str, sizeof(error_str) - 1); \
		response->len = sizeof(error_str) - 1; \
		stubborn_dispatch(shuttle->thread_ctx->queue_from_tx, &postprocess_stats_req_others, shuttle); \
		return; \
	} while (0)

	box_tuple_t *tuple;
	if (box_iterator_next(response->iterator, &tuple))
		RETURN_WITH_ERROR_FREE_IT("Iteration error");

	if (tuple == NULL)
		RETURN_WITH_ERROR_FREE_IT("<End of list>"); /* Or we can simply set response->len=0 */

	const char *name_msgpack = box_tuple_field(tuple, 1);
	if (name_msgpack == NULL)
		RETURN_WITH_ERROR_FREE_IT("Invalid entry format");

	uint32_t len;
	const char *const name = mp_decode_str(&name_msgpack, &len);
	if (len > stats_response_data_size_minus_1) {
		/* Real implementation should probably report error
		 * or use complex response logic to allocate and pass large buffers
		 * to send via HTTP(S) */
		len = stats_response_data_size_minus_1;
	}
	memcpy(&response->data, name, len);
	response->data[len] = '\n';
	response->len = len + 1;
	stubborn_dispatch(shuttle->thread_ctx->queue_from_tx, &postprocess_stats_req_others, shuttle);
}
#undef RETURN_WITH_ERROR_FREE_IT

/* Launched in HTTP server thread */
static int users_req_handler(h2o_handler_t *self, h2o_req_t *req)
{
	(void)self;
	/* req->path_normalized has "."/".." processed and query ("?...") stripped */
	if (!(h2o_memis(req->method.base, req->method.len, H2O_STRLIT("GET")) &&
	    h2o_memis(req->path_normalized.base, req->path_normalized.len, H2O_STRLIT(users_path))))
		return -1;

	/* Example of what we receive:
	req->input.scheme->name.base="https"
	req->input.scheme->name.default_port=443 (not related to actual port listened)
	req->input.authority.base="localhost:7890"
	*/

	unsigned id = 1; /* Default value for simplicity */
	if (req->query_at != SIZE_MAX) {
		/* Expecting: "?id=3" */
		const size_t len = req->path.len - req->query_at;
		const char *const query = &req->path.base[req->query_at]; /* N. b.: NOT null terminated */

		/* FIXME: Efficient (without memcpy) and error-prone code should be placed here */
		char temp[32];
		if (len <= sizeof(temp) - 1) {
			memcpy(temp, query, len);
			temp[len] = 0;
			sscanf(temp, "?id=%u", &id);
		}
	}

	shuttle_t *const shuttle = prepare_shuttle(req);

	our_request_t *const our_req = (our_request_t *)&shuttle->payload;
	our_req->id = id;

	thread_ctx_t *const thread_ctx = get_curr_thread_ctx();
	if (xtm_fun_dispatch(thread_ctx->queue_to_tx, (void(*)(void *))&process_users_req_in_tx, shuttle, 0)) {
		/* Error */
		free_shuttle_with_anchor(shuttle);
		req->res.status = 500;
		req->res.reason = "Queue overflow";
		h2o_send_inline(req, H2O_STRLIT("Queue overflow\n"));
		return 0;
	}

	return 0;
}

/* Launched in HTTP server thread */
static int stats_req_handler(h2o_handler_t *self, h2o_req_t *req)
{
	(void)self;
	if (!(h2o_memis(req->method.base, req->method.len, H2O_STRLIT("GET")) &&
	    h2o_memis(req->path_normalized.base, req->path_normalized.len, H2O_STRLIT(stats_path)))) {
		return -1;
	}

	shuttle_t *const shuttle = prepare_shuttle(req);
	/* Can fill in shuttle->payload here */

	thread_ctx_t *const thread_ctx = get_curr_thread_ctx();
	if (xtm_fun_dispatch(thread_ctx->queue_to_tx, (void(*)(void *))&process_stats_req_in_tx, shuttle, 0)) {
		/* Error */
		free_shuttle_with_anchor(shuttle);
		req->res.status = 500;
		req->res.reason = "Queue overflow";
		h2o_send_inline(req, H2O_STRLIT("Queue overflow\n"));
		return 0;
	}

	return 0;
}

/* Launched in HTTP server thread */
static void cancel_processing_large_req_in_http_thread(shuttle_t *shuttle)
{
	assert(shuttle->disposed);
	free_shuttle(shuttle);
}

/* Launched in TX thread */
static void cancel_processing_large_req_in_tx(shuttle_t *shuttle)
{
	large_response_t *const response = (large_response_t *)&shuttle->payload;

	box_tuple_unref(response->tuple);

	/* Can't call free_shuttle() from TX thread because it [potentially] uses per-thread pools */
	stubborn_dispatch(shuttle->thread_ctx->queue_from_tx, &cancel_processing_large_req_in_http_thread, shuttle);
}

/* Launched in HTTP server thread */
static void free_shuttle_after_deref(shuttle_t *shuttle)
{
	assert(((large_response_t *)(&shuttle->payload))->tuple != NULL);
	shuttle->disposed = true;
	if (!shuttle->stopped) {
		shuttle->stopped = true;
		stubborn_dispatch(get_curr_thread_ctx()->queue_to_tx, &cancel_processing_large_req_in_tx, shuttle);
	}
}

/* Launched in HTTP server thread to postprocess response */
static void postprocess_large_req(shuttle_t *shuttle)
{
	large_response_t *const response = (large_response_t *)(&shuttle->payload);
	if (shuttle->disposed) {
		if (response->tuple != NULL) {
			if (!shuttle->stopped) {
				shuttle->stopped = true;
				stubborn_dispatch(get_curr_thread_ctx()->queue_to_tx, &cancel_processing_large_req_in_tx, shuttle);
			}
		} else
			free_shuttle(shuttle);
		return;
	}
	h2o_req_t *const req = shuttle->never_access_this_req_from_tx_thread;
	req->res.status = 200;
	req->res.reason = "OK";
	h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL, H2O_STRLIT("text/plain; charset=utf-8"));
	char content_length_str[32];
	const size_t content_length_str_len = snprintf(content_length_str, sizeof(content_length_str), "%llu", (unsigned long long)response->len);
	h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_LENGTH, NULL, content_length_str, content_length_str_len);
	h2o_start_response(req, &response->generator);

	h2o_iovec_t buf;
	buf.len = response->len;
	if (response->tuple == NULL) {
		buf.base = response->data;
		shuttle->anchor->user_free_shuttle = &free_shuttle;
	} else {
		buf.base = (void *)response->direct_ptr;
		shuttle->anchor->user_free_shuttle = &free_shuttle_after_deref;
	}
	h2o_send(req, &buf, 1, H2O_SEND_STATE_FINAL);
}

/* Launched in HTTP server thread when connection has been closed */
static void stop_sending_large(h2o_generator_t *self, h2o_req_t *req)
{
	shuttle_t *const shuttle = get_shuttle_from_generator(self);
	large_response_t *const response = (large_response_t *)(&shuttle->payload);
	if (response->tuple != NULL && !shuttle->stopped) {
		shuttle->stopped = true;
		stubborn_dispatch(get_curr_thread_ctx()->queue_to_tx, &cancel_processing_large_req_in_tx, shuttle);
	}
}

/* Launched in TX thread */
static void process_large_req_in_tx(shuttle_t *shuttle)
{
	large_response_t *const response = (large_response_t *)&shuttle->payload;
	response->generator = (h2o_generator_t){ NULL, stop_sending_large };

	char entry_index_msgpack[16];
	char *key_end = mp_encode_array(entry_index_msgpack, 1);
	key_end = mp_encode_uint(key_end, 2 /* FIXME: Hardcoded index */);
	assert(key_end < &entry_index_msgpack[0] + sizeof(entry_index_msgpack));

#define RETURN_WITH_ERROR(err) \
	do { \
		response->tuple = NULL; \
		static const char error_str[] = err; \
		memcpy(&response->data, error_str, sizeof(error_str) - 1); \
		response->len = sizeof(error_str) - 1; \
		stubborn_dispatch(shuttle->thread_ctx->queue_from_tx, &postprocess_large_req, shuttle); \
		return; \
	} while (0)

	box_tuple_t *tuple;
	if (box_index_get(our_userdata.large_space_id, our_userdata.large_index_id, entry_index_msgpack, key_end, &tuple))
		RETURN_WITH_ERROR("Query error");

	if (tuple == NULL)
		RETURN_WITH_ERROR("Entry not found");

	const char *name_msgpack = box_tuple_field(tuple, 1);
	if (name_msgpack == NULL)
		RETURN_WITH_ERROR("Invalid entry format");

	uint32_t len;
	const char *const name = mp_decode_str(&name_msgpack, &len);
	response->len = len;
	if (len <= large_response_data_size) {
		response->tuple = NULL;
		memcpy(response->data, name, len);
	} else {
		box_tuple_ref(tuple);
		response->tuple = tuple;
		response->direct_ptr = name;
	}
	stubborn_dispatch(shuttle->thread_ctx->queue_from_tx, &postprocess_large_req, shuttle);
}
#undef RETURN_WITH_ERROR

/* Launched in HTTP server thread */
static int large_req_handler(h2o_handler_t *self, h2o_req_t *req)
{
	(void)self;
	if (!(h2o_memis(req->method.base, req->method.len, H2O_STRLIT("GET")) &&
	    h2o_memis(req->path_normalized.base, req->path_normalized.len, H2O_STRLIT(large_path)))) {
		return -1;
	}

	shuttle_t *const shuttle = prepare_shuttle(req);
	/* Can fill in shuttle->payload here */

	thread_ctx_t *const thread_ctx = get_curr_thread_ctx();
	if (xtm_fun_dispatch(thread_ctx->queue_to_tx, (void(*)(void *))&process_large_req_in_tx, shuttle, 0)) {
		/* Error */
		free_shuttle_with_anchor(shuttle);
		req->res.status = 500;
		req->res.reason = "Queue overflow";
		h2o_send_inline(req, H2O_STRLIT("Queue overflow\n"));
		return 0;
	}

	return 0;
}

/* Launched in HTTP server thread */
static void cancel_processing_fiber_req_in_http_thread(shuttle_t *shuttle)
{
	assert(shuttle->disposed);
	free_shuttle(shuttle);
}

/* Launched in TX thread */
static void cancel_processing_fiber_req_in_tx(shuttle_t *shuttle)
{
	fiber_response_t *const response = (fiber_response_t *)&shuttle->payload;

	if (response->fiber != NULL && !response->fiber_done)
		fiber_cancel(response->fiber);

	/* Can't call free_shuttle() from TX thread because it [potentially] uses per-thread pools */
	stubborn_dispatch(shuttle->thread_ctx->queue_from_tx, &cancel_processing_fiber_req_in_http_thread, shuttle);
}

/* Launched in HTTP server thread to postprocess response */
static void postprocess_fiber_req(shuttle_t *shuttle)
{
	fiber_response_t *const response = (fiber_response_t *)(&shuttle->payload);
	if (shuttle->disposed) {
		if (!shuttle->stopped) {
			shuttle->stopped = true;
			stubborn_dispatch(get_curr_thread_ctx()->queue_to_tx, &cancel_processing_fiber_req_in_tx, shuttle);
		}
		return;
	}
	h2o_req_t *const req = shuttle->never_access_this_req_from_tx_thread;
	req->res.status = 200;
	req->res.reason = "OK";
	h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL, H2O_STRLIT("text/plain; charset=utf-8"));
	char content_length_str[32];
	const size_t content_length_str_len = snprintf(content_length_str, sizeof(content_length_str), "%llu", (unsigned long long)response->len);
	h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_LENGTH, NULL, content_length_str, content_length_str_len);
	static h2o_generator_t generator = {NULL, NULL};
	h2o_start_response(req, &generator);

	h2o_iovec_t buf;
	buf.base = response->data;
	buf.len = response->len;
	shuttle->anchor->user_free_shuttle = &free_shuttle; /* Optimization, replacing &free_shuttle_with_fiber because fiber is already done */
	h2o_send(req, &buf, 1, H2O_SEND_STATE_FINAL);
}

/* Launched in TX thread */
static int
example_fiber_func(va_list ap)
{
	static const double sleep_time = 10.;
	shuttle_t *const shuttle = va_arg(ap, shuttle_t *);

	fiber_sleep(sleep_time);

	fiber_response_t *const response = (fiber_response_t *)&shuttle->payload;
	response->len = snprintf(response->data, fiber_response_data_size, "Hello from fiber after sleeping for %.1f seconds", sleep_time);
	stubborn_dispatch(shuttle->thread_ctx->queue_from_tx, &postprocess_fiber_req, shuttle);

	response->fiber_done = true;
	return 0;
}

/* Launched in TX thread */
static void process_fiber_req_in_tx(shuttle_t *shuttle)
{
	fiber_response_t *const response = (fiber_response_t *)&shuttle->payload;

	/* This is just an example of fibers usage, no DB access used at all */

#define RETURN_WITH_ERROR(err) \
	do { \
		static const char error_str[] = err; \
		memcpy(&response->data, error_str, sizeof(error_str) - 1); \
		response->len = sizeof(error_str) - 1; \
		stubborn_dispatch(shuttle->thread_ctx->queue_from_tx, &postprocess_fiber_req, shuttle); \
		return; \
	} while (0)

	if ((response->fiber = fiber_new("HTTP test fiber", &example_fiber_func)) == NULL)
		RETURN_WITH_ERROR("Failed to create fiber");
	response->fiber_done = false;
	fiber_start(response->fiber, shuttle);
}
#undef RETURN_WITH_ERROR

/* Launched in HTTP server thread */
static void free_shuttle_with_fiber(shuttle_t *shuttle)
{
	shuttle->disposed = true;
	if (!shuttle->stopped) {
		shuttle->stopped = true;
		stubborn_dispatch(get_curr_thread_ctx()->queue_to_tx, &cancel_processing_fiber_req_in_tx, shuttle);
	}
}

/* Launched in HTTP server thread */
static int fiber_req_handler(h2o_handler_t *self, h2o_req_t *req)
{
	(void)self;
	if (!(h2o_memis(req->method.base, req->method.len, H2O_STRLIT("GET")) &&
	    h2o_memis(req->path_normalized.base, req->path_normalized.len, H2O_STRLIT(fiber_path)))) {
		return -1;
	}

	shuttle_t *const shuttle = prepare_shuttle(req);
	/* Can fill in shuttle->payload here */

	thread_ctx_t *const thread_ctx = get_curr_thread_ctx();
	if (xtm_fun_dispatch(thread_ctx->queue_to_tx, (void(*)(void *))&process_fiber_req_in_tx, shuttle, 0)) {
		/* Error */
		free_shuttle_with_anchor(shuttle);
		req->res.status = 500;
		req->res.reason = "Queue overflow";
		h2o_send_inline(req, H2O_STRLIT("Queue overflow\n"));
		return 0;
	}
	shuttle->anchor->user_free_shuttle = &free_shuttle_with_fiber;

	return 0;
}

static const site_desc_t our_site_desc = {
	.num_threads = 4,
	.max_conn_per_thread = 64,
	.shuttle_size = 256,
	/* All .init_userdata_in_tx() are called, we can use per-path initialization functions
	 * (this can be more convenient when integrating several modules)
	 * or put everything into one. */
	.path_descs = {
		{ .path = users_path, .handler = users_req_handler, .init_userdata_in_tx = init_userdata_in_tx, },
		{ .path = stats_path, .handler = stats_req_handler, },
		{ .path = large_path, .handler = large_req_handler, },
		{ .path = fiber_path, .handler = fiber_req_handler, },
		{ .path = NULL }, /* Terminator */
	},
};

static int get_site_desc(lua_State *L)
{
	/* We are passed one Lua parameter we can use to configure descs etc */
	lua_pop(L, 1);

	lua_pushinteger(L, (uintptr_t)&our_site_desc);
	return 1;
}

static void init_site(void)
{
	users_response_data_size = our_site_desc.shuttle_size - sizeof(shuttle_t) - offsetof(simple_response_t, data);
	stats_response_data_size_minus_1 = our_site_desc.shuttle_size - sizeof(shuttle_t) - offsetof(response_with_state_t, data) - 1;

	/* We can use lower value to trigger direct access to tuple */
	large_response_data_size = our_site_desc.shuttle_size - sizeof(shuttle_t) - offsetof(large_response_t, data);

	fiber_response_data_size = our_site_desc.shuttle_size - sizeof(shuttle_t) - offsetof(fiber_response_t, data);
}

static const struct luaL_Reg mylib[] = {
	{"get_site_desc", get_site_desc},
	{NULL, NULL}
};

int luaopen_sample_site(lua_State *L)
{
	init_site();
	luaL_newlib(L, mylib);
	return 1;
}
