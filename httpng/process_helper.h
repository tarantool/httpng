#ifndef PROCESS_HELPER_H
#define PROCESS_HELPER_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define REAPER_SOCKET_NAME "tmp_reaper_socket"

typedef enum {
	TYPE_EXEC = 0,
	TYPE_TRYWAIT = 1,
	TYPE_WAIT = 2,
} type_t;

enum {
	/* Negative! */
	TRYWAIT_RESULT_ERROR = -1,
	WAIT_RESULT_ERROR = -1,
	TRYWAIT_RESULT_AGAIN = -2,
};

typedef struct {
	type_t type;
	union {
		unsigned len; /* TYPE_EXEC */
		pid_t pid; /* TYPE_TRYWAIT/TYPE_WAIT */
	} un;
	char str[4088];
} process_helper_req_t;

#ifdef __cplusplus
} /* extern "C" */
#endif /* __cplusplus */

#endif /* PROCESS_HELPER_H */
