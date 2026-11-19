/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * pthread.c - pthread_create/pthread_join on top of BSD45 user threads.
 *
 * The kernel ABI is:
 *	440 thread_create(entry, arg, stack) -> tid
 *	441 thread_exit(code)
 *	442 thread_join(tid) -> code
 *	443 thread_self() -> tid
 *	444 thread_yield()
 *
 * Each new thread gets a static user stack and a small start record; the
 * kernel enters at pthread_tramp with eax = &start, so the trampoline calls
 * start->fn(start->arg) and then thread_exit().
 */
#include <pthread.h>
#include <syscall_arch.h>

#define	NTHREADS	4
#define	sc(n, a, b, c)	__syscall3((n), (a), (b), (c))

static int	used[NTHREADS];
static pthread_t tids[NTHREADS];
static volatile int done[NTHREADS];
static void	*retv[NTHREADS];
static int	detached[NTHREADS];

struct pstart {
	void	*(*fn)(void *);
	void	*arg;
};

static struct pstart pss[NTHREADS];

extern void pthread_tramp(void);

static int
stack_alloc(void)
{
	int i;

	for (i = 0; i < NTHREADS; i++)
		if (!used[i]) {
			used[i] = 1;
			return (i);
		}
	return (-1);
}

int
pthread_create(pthread_t *tid, const pthread_attr_t *attr,
    void *(*start)(void *), void *arg)
{
	struct pstart *ps;
	int i, r;

	(void)attr;
	if (tid == 0 || start == 0)
		return (-1);
	i = stack_alloc();
	if (i < 0)
		return (-1);
	ps = &pss[i];
	ps->fn = start;
	ps->arg = arg;
	r = sc(440, (int)pthread_tramp, (int)ps, 0);
	if (r < 0) {
		used[i] = 0;
		return (-1);
	}
	tids[i] = r;
	*tid = r;
	return (0);
}

int
pthread_join(pthread_t tid, void **retval)
{
	int i;

	for (i = 0; i < NTHREADS; i++)
		if (tids[i] == tid)
			break;
	if (i == NTHREADS)
		return (-1);
	while (!done[i])
		pthread_yield();
	if (retval != 0)
		*retval = retv[i];
	used[i] = 0;
	tids[i] = 0;
	done[i] = 0;
	retv[i] = 0;
	return (0);
}

int
pthread_detach(pthread_t tid)
{
	int i;

	for (i = 0; i < NTHREADS; i++)
		if (tids[i] == tid)
			break;
	if (i == NTHREADS)
		return (-1);
	detached[i] = 1;
	(void)sc(445, tid, 0, 0);
	return (0);
}

pthread_t
pthread_self(void)
{

	return (sc(443, 0, 0, 0));
}

void
pthread_exit(void *retval)
{
	pthread_t self = pthread_self();
	int i;

	for (i = 0; i < NTHREADS; i++)
		if (tids[i] == self) {
			retv[i] = retval;
			done[i] = 1;
			if (detached[i])
				used[i] = 0;
			break;
		}
	sc(441, (int)retval, 0, 0);
	for (;;)
		;
}

int
pthread_yield(void)
{

	return (sc(444, 0, 0, 0));
}

int
sched_setprio(int prio)
{

	return (sc(446, prio, 0, 0));
}

int
sched_getprio(void)
{

	return (sc(447, 0, 0, 0));
}

/* ==================================================================== */
/* S400: 用户态同步原语 mutex / condition variable                       */
/* S400: user-space mutex / condition variable                           */
/* ==================================================================== */
/*
 * 中文：BSD45 是单核协作式调度，线程只在 syscall/时钟 tick 让出 CPU，不会真正
 * 并行。因此 mutex 用 volatile 状态 + sched_yield 自旋即可正确互斥，无需内核
 * futex。condvar 用 generation 计数防止 signal 丢失：wait 记录进入时的 gen，
 * 释放 mutex 后 yield 直到 gen 变化或 predicate 由调用方复查。
 * EN: BSD45 is a single-core cooperative scheduler: threads yield only at
 * syscalls/ticks and never run truly in parallel.  A volatile flag plus
 * sched_yield is therefore a correct mutex without a kernel futex.  The condvar
 * uses a generation counter to avoid lost wakeups: wait snapshots gen, drops
 * the mutex, yields until gen changes (caller rechecks its predicate).
 */

int
pthread_mutex_init(pthread_mutex_t *m, const void *attr)
{
	(void)attr;
	if (m == 0)
		return (-1);
	m->locked = 0;
	return (0);
}

int
pthread_mutex_lock(pthread_mutex_t *m)
{
	if (m == 0)
		return (-1);
	while (m->locked)
		pthread_yield();	/* 协作式让出 / cooperative yield */
	m->locked = 1;
	return (0);
}

int
pthread_mutex_trylock(pthread_mutex_t *m)
{
	if (m == 0)
		return (-1);
	if (m->locked)
		return (1);		/* 忙碌 / busy */
	m->locked = 1;
	return (0);
}

int
pthread_mutex_unlock(pthread_mutex_t *m)
{
	if (m == 0)
		return (-1);
	m->locked = 0;
	return (0);
}

int
pthread_mutex_destroy(pthread_mutex_t *m)
{
	if (m == 0 || m->locked)
		return (-1);
	return (0);
}

int
pthread_cond_init(pthread_cond_t *c, const void *attr)
{
	(void)attr;
	if (c == 0)
		return (-1);
	c->gen = 0;
	c->waiters = 0;
	return (0);
}

int
pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
	unsigned gen;

	if (c == 0 || m == 0)
		return (-1);
	gen = c->gen;
	c->waiters++;
	pthread_mutex_unlock(m);
	while (c->gen == gen)	/* 等 signal 递增 gen / wait for signal */
		pthread_yield();
	c->waiters--;
	return (pthread_mutex_lock(m));
}

int
pthread_cond_signal(pthread_cond_t *c)
{
	if (c == 0)
		return (-1);
	if (c->waiters > 0)
		c->gen++;		/* 只唤醒一个语义由调用方 predicate 保证 */
	return (0);
}

int
pthread_cond_broadcast(pthread_cond_t *c)
{
	if (c == 0)
		return (-1);
	c->gen++;
	return (0);
}

int
pthread_cond_destroy(pthread_cond_t *c)
{
	if (c == 0 || c->waiters > 0)
		return (-1);
	return (0);
}
