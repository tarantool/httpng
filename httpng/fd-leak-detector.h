#define close(x) spying_close(x)
#define socket(a, b, c) spying_socket(a, b, c)
#define pipe2(a, b) spying_pipe2(a, b)
#define pipe(a) spying_pipe(a)
#define eventfd(a, b) spying_eventfd(a, b)
#define dup(a) spying_dup(a)
#define accept(a, b, c) spying_accept(a, b, c)
#define accept4(a, b, c, d) spying_accept4(a, b, c, d)

#include <arpa/inet.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

extern void complain_loudly_about_leaked_fds(void);

extern int spying_close(int fd);
extern int spying_socket(int domain, int type, int protocol);
extern int spying_pipe2(int pipedes[2], int flags)  __THROW __wur;
extern int spying_pipe(int pipefd[2]) __THROW __wur;
extern int spying_eventfd(unsigned int initval, int flags) __THROW;
extern int spying_dup(int oldfd) __THROW __wur;
extern int spying_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
extern int spying_accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags);

#ifdef __cplusplus
} /* extern "C" */
#endif /* __cplusplus */

