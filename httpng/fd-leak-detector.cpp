#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <set>

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
typedef std::set<int> fds_t;
static std::set<int> fds;

/* This is debugging code, do not bother with error checking,
 * exceptions etc. */

extern "C" {

static void check_already_opened_locked(int fd, const char *name)
{
	fds_t::iterator it = fds.find(fd);
	if (it != fds.end()) {
		pthread_mutex_unlock(&mutex);
		fprintf(stderr, "%s(): %d already opened\n", name, fd);
		_exit(1);
	}
}

static int common(const char *name, int fd)
{
	if (fd < 0)
		return fd;
	pthread_mutex_lock(&mutex);
	check_already_opened_locked(fd, name);
	fds.insert(fd);
	pthread_mutex_unlock(&mutex);
	fprintf(stderr, "%s(): returning %d\n", name, fd);
	return fd;
}

#define IMPL_INTERNAL(func) return common(func, fd);

#define IMPL(func, params) do { \
	const int fd = func params; \
	IMPL_INTERNAL(#func) \
} while (0);

int spying_socket(int domain, int type, int protocol)
{
	IMPL(socket, (domain, type, protocol))
}

int spying_pipe2_named(int pipedes[2], int flags, const char *name)
{
	const int result = pipe2(pipedes, flags);
	if (result < 0)
		return result;
	pthread_mutex_lock(&mutex);

	check_already_opened_locked(pipedes[0], name);
	check_already_opened_locked(pipedes[1], name);
	fds.insert(pipedes[0]);
	fds.insert(pipedes[1]);

	pthread_mutex_unlock(&mutex);
	fprintf(stderr, "%s(): returning %d, %d\n", name, pipedes[0], pipedes[1]);
	return result;
}

int spying_pipe2(int pipedes[2], int flags)
{
	return spying_pipe2_named(pipedes, flags, "pipe2");
}

int spying_pipe(int pipefd[2])
{
	return spying_pipe2_named(pipefd, 0, "pipe");
}

int spying_eventfd(unsigned int initval, int flags)
{
	IMPL(eventfd, (initval, flags))
}

int spying_dup(int oldfd)
{
	IMPL(dup, (oldfd))
}

int spying_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
	IMPL(accept, (sockfd, addr, addrlen))
}

int spying_accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags)
{
	IMPL(accept4, (sockfd, addr, addrlen, flags))
}

int spying_close(int fd)
{
	fprintf(stderr, "close(): %d\n", fd);
	pthread_mutex_lock(&mutex);

	fds_t::iterator it = fds.find(fd);
	if (it == fds.end()) {
		pthread_mutex_unlock(&mutex);
		fprintf(stderr, "close(): %d was not opened (or has already been closed)\n", fd);
		_exit(1);
	}
	fds.erase(it);

	pthread_mutex_unlock(&mutex);
	return close(fd);
}

void complain_loudly_about_leaked_fds(void)
{
	unsigned counter = 0;
	pthread_mutex_lock(&mutex);

	for (fds_t::iterator it = fds.begin(); it != fds.end(); it++) {
		fprintf(stderr, "%s(): %d leaked\n", __func__, *it);
		++counter;
	}
	pthread_mutex_unlock(&mutex);
	if (counter != 0) {
		fprintf(stderr, "%s(): %u file descriptor leaks detected\n", __func__, counter);
		_exit(1);
	}
	fprintf(stderr, "%s(): ZERO file descriptor leaks detected\n", __func__);
}

} /* extern "C" */
