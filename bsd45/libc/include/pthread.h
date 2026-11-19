/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * pthread.h - user-space threads backed by BSD45 kernel kthreads (S26).
 */
#ifndef _PTHREAD_H_
#define _PTHREAD_H_

typedef int pthread_t;
typedef int pthread_attr_t;

/* S400: 用户态同步原语。单核协作式调度下 mutex 用 volatile 状态 + yield；
 * condvar 用 generation 计数防丢唤醒。
 * S400: user sync primitives.  Under single-core cooperative scheduling the
 * mutex is a volatile flag + yield; condvar uses a generation counter. */
typedef struct {
	volatile int	locked;
} pthread_mutex_t;
typedef struct {
	volatile unsigned	gen;
	volatile unsigned	waiters;
} pthread_cond_t;
typedef int pthread_mutexattr_t;
typedef int pthread_condattr_t;

#define PTHREAD_MUTEX_INITIALIZER	{ 0 }
#define PTHREAD_COND_INITIALIZER	{ 0, 0 }

#ifdef __cplusplus
extern "C" {
#endif

int	pthread_create(pthread_t *tid, const pthread_attr_t *attr,
	    void *(*start)(void *), void *arg);
int	pthread_join(pthread_t tid, void **retval);
int	pthread_detach(pthread_t tid);
pthread_t pthread_self(void);
void	pthread_exit(void *retval);
int	pthread_yield(void);
/* S400 mutex / condition variable */
int	pthread_mutex_init(pthread_mutex_t *m, const void *attr);
int	pthread_mutex_lock(pthread_mutex_t *m);
int	pthread_mutex_trylock(pthread_mutex_t *m);
int	pthread_mutex_unlock(pthread_mutex_t *m);
int	pthread_mutex_destroy(pthread_mutex_t *m);
int	pthread_cond_init(pthread_cond_t *c, const void *attr);
int	pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int	pthread_cond_signal(pthread_cond_t *c);
int	pthread_cond_broadcast(pthread_cond_t *c);
int	pthread_cond_destroy(pthread_cond_t *c);
/* scheduling: 0 = lowest, 19 = highest */
int	sched_setprio(int prio);
int	sched_getprio(void);

#ifdef __cplusplus
}
#endif

#endif
