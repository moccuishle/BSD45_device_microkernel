/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * shmbuf.h - S402 shared-page bulk data transport (KP-7 closed loop).
 * shmbuf.h - S402 共享页大数据传输（KP-7 闭环）。
 *
 * 中文：把 KP-7 原语（page_alloc/map_shared/unmap_shared/page_free）封装为
 * "分配→映射→填充→提交(pa)→对方映射读取→ACK 释放"的闭环。大数据不进 84 字节
 * 的 IPC 消息内联区，只把物理页号 pa 放进消息的 pa 字段；接收方映射 pa 读数据，
 * 读完发 ACK，由分配方 page_free。无需跨进程引用计数；服务崩溃时内核在
 * free_address_space 统一回收映射页。
 * EN: Wrap KP-7 primitives into a closed loop: alloc -> map -> fill ->
 * commit(pa) -> peer maps/reads -> ACK -> free.  Bulk data never enters the
 * 84-byte inline area; only pa travels in the message's pa field.  The peer
 * maps pa, reads, sends ACK, and the allocator frees — no cross-process
 * refcount; on service exit the kernel reclaims mapped pages.
 *
 * 中文：实现为 static inline，工具单文件编译即可用，无需新增 libc 对象。
 * EN: Implemented as static inline so single-file tools can use it without a
 * new libc object.
 */
#ifndef _SHMBUF_H_
#define _SHMBUF_H_

#include <sys/types.h>

/* syscall 号 / syscall numbers */
#define SYS_SHM_MAP_SHARED	456
#define SYS_SHM_UNMAP_SHARED	457
#define SYS_SHM_PAGE_ALLOC	458
#define SYS_SHM_PAGE_FREE	459

/* 共享页专用映射 VA（各进程独立地址空间，避免冲突）/ dedicated mapping VA */
#define SHM_VA			0x0B000000u

/* 共享页缓冲区句柄 / shared-page buffer handle */
struct shmbuf {
	unsigned	pa;		/* 物理页号 / physical page number */
	int		mapped;		/* 当前是否已映射 / mapped at SHM_VA? */
};

/* int $0x80 系统调用封装 / int $0x80 syscall wrapper */
static inline int
shm_syscall4(int n, int a, int b, int c, int d)
{
	int r;

	__asm__ volatile("int $0x80"
	    : "=a"(r)
	    : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d)
	    : "memory");
	return (r);
}

#define shm_page_alloc()	shm_syscall4(SYS_SHM_PAGE_ALLOC,0,0,0,0)
#define shm_page_free(pa)	shm_syscall4(SYS_SHM_PAGE_FREE,(int)(pa),0,0,0)
#define shm_do_map(pa,va)	shm_syscall4(SYS_SHM_MAP_SHARED,(int)getpid(),\
					    (int)(va),(int)(pa),3)
#define shm_do_unmap(va)	shm_syscall4(SYS_SHM_UNMAP_SHARED,(int)getpid(),\
					    (int)(va),0,0)

/* 分配一页并映射到 SHM_VA，返回可写指针（0 失败）。
 * Allocate a page, map at SHM_VA, return writable pointer (0 on failure). */
static inline void *
shm_alloc(struct shmbuf *b)
{
	b->pa = (unsigned)shm_page_alloc();
	b->mapped = 0;
	if (b->pa == 0)
		return (0);
	if (shm_do_map(b->pa, SHM_VA) < 0) {
		shm_page_free(b->pa);
		b->pa = 0;
		return (0);
	}
	b->mapped = 1;
	return ((void *)SHM_VA);
}

/* 提交：解除映射，数据留在物理页；pa 由调用方放进 IPC 消息。
 * Commit: unmap; data stays in the page; caller sends b->pa in an IPC message. */
static inline void
shm_commit(struct shmbuf *b)
{
	if (b->mapped) {
		shm_do_unmap(SHM_VA);
		b->mapped = 0;
	}
}

/* 映射别人发来的 pa，返回可读指针（0 失败）。
 * Map a peer-sent pa, return readable pointer (0 on failure). */
static inline void *
shm_map(unsigned pa)
{
	if (pa == 0)
		return (0);
	if (shm_do_map(pa, SHM_VA) < 0)
		return (0);
	return ((void *)SHM_VA);
}

/* 解除映射（读完后调用；pa 仍由分配方 free）。
 * Unmap after reading; pa is still freed by its allocator. */
static inline void
shm_unmap(void)
{
	shm_do_unmap(SHM_VA);
}

/* 释放物理页（ACK 闭环最后一环，由分配方调用）。
 * Free the physical page (final ACK step, by the allocator). */
static inline void
shm_free(struct shmbuf *b)
{
	if (b->mapped) {
		shm_do_unmap(SHM_VA);
		b->mapped = 0;
	}
	if (b->pa) {
		shm_page_free(b->pa);
		b->pa = 0;
	}
}

#endif /* _SHMBUF_H_ */
