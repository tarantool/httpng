#include "../httpng/process_helper.h"
#include <assert.h>
#include <errno.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;

static struct sockaddr_un addr = {
	.sun_family = AF_UNIX,
	.sun_path = REAPER_SOCKET_NAME,
};
static bool server_launched = false;

static void
send_response(int fd, pid_t pid)
{
retry:
	;
	const int sent = send(fd, &pid, sizeof(pid), 0);
	if (sent == sizeof(pid))
		return;
	if (sent < 0 && errno == EINTR)
		goto retry;
	/* FIXME: Handle partial sends maybe? */
	assert(false);
}

static pid_t
execute(process_helper_req_t *req)
{
	const unsigned len = req->un.len;
	if (len <= 0 || len > sizeof(req->str))
		return -1;

	char *(args[sizeof(req->str) / 2]); /* Worst case. */
	unsigned count = 0;
	char *pos = req->str;
	char *prev_pos = pos;
	const char *const end = &req->str[len];
	do {
		if (*pos == ' ') {
			*pos = 0;
			args[count++] = prev_pos;
			prev_pos = pos + 1;
		}
	} while (++pos < end);
	if (*(pos - 1) != 0)
		return -1;
	args[count] = NULL;

	pid_t pid;
	const int res = posix_spawnp(&pid, args[0], NULL,
		/* attrp */ NULL, args, environ);
	if (res != 0) {
		perror("execute(): posix_spawnp() failed");
		return -1;
	}
	return pid;
}

static int
my_wait(process_helper_req_t *req)
{
	int code;
	const pid_t pid = waitpid(req->un.pid, &code, 0);

	if (pid != req->un.pid || WIFSIGNALED(code) || !WIFEXITED(code))
		return WAIT_RESULT_ERROR;

	return WEXITSTATUS(code);
}

static int
my_trywait(process_helper_req_t *req)
{
	int code;
	const pid_t pid = waitpid(req->un.pid, &code, WNOHANG);
	if (pid == 0)
		return TRYWAIT_RESULT_AGAIN;

	if (pid != req->un.pid || WIFSIGNALED(code) || !WIFEXITED(code))
		return TRYWAIT_RESULT_ERROR;

	return WEXITSTATUS(code);
}

static void
serve(void)
{
	const int srv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (srv_fd < 0)
		return;
	if (bind(srv_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		goto fail;
	if (listen(srv_fd, SOMAXCONN) < 0)
		goto fail;

retry_accept:
	;
	const int fd = accept(srv_fd, NULL, NULL);
	if (fd < 0) {
		if (errno == EINTR)
			goto retry_accept;
		goto fail;
	}

retry_recv:
	;
	ssize_t already_received = 0;
	process_helper_req_t req;
	const ssize_t received = recv(fd, (char *)&req + already_received,
		sizeof(req) - already_received, 0);
	if (received < 0) {
		if (errno == EINTR)
			goto retry_recv;
		close(fd);
		goto fail;
	}
	already_received += received;
	if (already_received < sizeof(req))
		goto retry_recv;

	switch (req.type) {
	case TYPE_EXEC:
		send_response(fd, execute(&req));
		break;
	case TYPE_TRYWAIT:
		send_response(fd, my_trywait(&req));
		break;
	case TYPE_WAIT:
		send_response(fd, my_wait(&req));
		break;
	default:
		send_response(fd, -1);
	}
	close(fd);
	goto retry_accept;
fail:
	close(srv_fd);
}

static int
launch_reaper_server(void)
{
	if (server_launched)
		return -1;
	unlink(addr.sun_path);
	/* FIXME: Drop root (to which UID)? Refuse to work under root? */
	server_launched = true;
	const int res = fork();
	if (res > 0) {
		sleep(1); /* Stopgap. */
		return 0;
	}
	if (res < 0)
		return -2;
	serve();
	assert(false);
	return 0;
}

int
main(int argc, char *argv[])
{
	process_helper_req_t req;
#define long_str req.str
	size_t total_len = 0;
	if (argc < 2)
		return 2;
	int i;
	for (i = 1; i < argc; ++i) {
		const size_t len = strlen(argv[i]);
		if (total_len + len + 1>= sizeof(long_str))
			return 1;
		memcpy(&long_str[total_len], argv[i], len);
		total_len += len;
		long_str[total_len] = ' ';
		++total_len;
	}
	if (total_len + 1 >= sizeof(long_str))
		return 1;
	long_str[total_len] = 0;
	req.un.len = ++total_len;

retry_socket:
	;
	const int reaper_client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (reaper_client_fd < 0) {
		if (launch_reaper_server() != 0)
			goto fallback_to_simple;
		goto retry_socket;
	}
retry_connect:
	if (connect(reaper_client_fd, (struct sockaddr *)&addr,
	    sizeof(addr)) < 0) {
		if (launch_reaper_server() != 0) {
			close(reaper_client_fd);
			goto fallback_to_simple;
		}
		goto retry_connect;
	}
	req.type = TYPE_EXEC;
	ssize_t bytes_already_sent = 0;
retry_send:
	;
	const ssize_t bytes_sent = send(reaper_client_fd,
		(char *)&req + bytes_already_sent,
		sizeof(req) - bytes_already_sent, 0);
	if (bytes_sent < 0) {
		if (errno == EINTR)
			goto retry_send;
		close(reaper_client_fd);
		goto fallback_to_simple;
	}
	bytes_already_sent += bytes_sent;
	if (bytes_already_sent < sizeof(req))
		goto retry_send;

	pid_t pid;
	/* FIXME: Handle EINTR at least? */
	if (sizeof(pid) == recv(reaper_client_fd, &pid, sizeof(pid), 0))
		goto write_pid;
	close(reaper_client_fd);

fallback_to_simple:
	;
	char *const *args = &argv[1];
	posix_spawn_file_actions_t file_actions;
	posix_spawn_file_actions_init(&file_actions);

	const int res = posix_spawnp(&pid, argv[1], &file_actions,
		/* attrp */ NULL, args, environ);
	if (res != 0) {
		perror("main(): posix_spawnp() failed");
		return 3;
	}
write_pid:
	;
	FILE *const pidfile = fopen("tmp_pid.txt", "w");
	if (pidfile != NULL)
		fprintf(pidfile, "%u", (unsigned)pid);
	return 0;
}
