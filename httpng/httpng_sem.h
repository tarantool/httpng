#ifndef HTTPNG_SEM_H
#define HTTPNG_SEM_H

#ifdef __APPLE__
#include <dispatch/dispatch.h>
typedef dispatch_semaphore_t httpng_sem_t;
#else /* __APPLE__ */
#include <semaphore.h>
typedef sem_t httpng_sem_t;
#endif /* __APPLE__ */

static inline void
httpng_sem_init(httpng_sem_t *s, uint32_t value)
{
#ifdef __APPLE__
	*s = dispatch_semaphore_create(value);
#else /* __APPLE__ */
	sem_init(s, 0, value);
#endif /* __APPLE__ */
}

static inline void
httpng_sem_wait(httpng_sem_t *s)
{
#ifdef __APPLE__
	dispatch_semaphore_wait(*s, DISPATCH_TIME_FOREVER);
#else /* __APPLE__ */
	int r;
	do {
		r = sem_wait(s);
	} while (r == -1 && errno == EINTR);
#endif /* __APPLE__ */
}

static inline void
httpng_sem_post(httpng_sem_t *s)
{
#ifdef __APPLE__
	dispatch_semaphore_signal(*s);
#else /* __APPLE__ */
	sem_post(s);
#endif /* __APPLE__ */
}

static inline void
httpng_sem_destroy(httpng_sem_t *s)
{
#ifdef __APPLE__
	dispatch_release(*s);
#else /* __APPLE__ */
	sem_destroy(s);
#endif /* __APPLE__ */
}

#endif /* HTTPNG_SEM_H */
