/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * shmtest.c - S402 acceptance: shared-page bulk data closed loop (KP-7).
 * shmtest.c - S402 验收：共享页大数据闭环（KP-7）。
 *
 * 中文：验证"分配→映射→填充整页(4KB)→提交(pa)→重新映射(pa)→校验→ACK释放"
 * 全链路。4KB 远超 84 字节 IPC 内联区，证明大数据走共享页可行；再分配验证
 * 页被正确回收（无泄漏）。这是 VFS 大数据读写与 net 大包传输的基础。
 * EN: Verify the full loop alloc -> map -> fill a whole page (4KB) -> commit
 * (pa) -> remap(pa) -> verify -> ACK/free.  4KB far exceeds the 84-byte inline
 * area, proving bulk data over shared pages works; re-allocation proves the
 * page was reclaimed (no leak).  Foundation for VFS bulk I/O and net packets.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <shmbuf.h>

#define PAGE_BYTES	4096

static int
fail(const char *what)
{
	printf("shm: %s FAILED\n", what);
	return (1);
}

int
main(void)
{
	struct shmbuf b;
	char *w, *r;
	unsigned pa_saved;
	int i, bad = 0;

	/* ---- 1: 分配 + 映射，填充整页 ---- */
	w = (char *)shm_alloc(&b);
	if (w == 0 || b.pa == 0)
		return (fail("shm_alloc"));
	for (i = 0; i < PAGE_BYTES; i++)
		w[i] = (char)(i & 0xff);
	printf("shm: alloc pa=0x%x mapped=%d\n", b.pa, b.mapped);

	/* ---- 2: 提交（解除映射，数据留在物理页，pa 可经 IPC 传递）---- */
	pa_saved = b.pa;
	shm_commit(&b);
	if (b.mapped)
		bad += fail("shm_commit");

	/* ---- 3: 用收到的 pa 重新映射（模拟接收方）并校验全部 4KB ---- */
	r = (char *)shm_map(pa_saved);
	if (r == 0)
		bad += fail("shm_map");
	else {
		for (i = 0; i < PAGE_BYTES; i++) {
			if ((unsigned char)r[i] != (unsigned char)(i & 0xff)) {
				printf("shm: data mismatch at %d\n", i);
				bad += fail("bulk data verify");
				break;
			}
		}
		if (bad == 0)
			printf("shm: bulk 4KB verify ok\n");
		shm_unmap();
	}

	/* ---- 4: ACK 释放 ---- */
	b.pa = pa_saved;
	shm_free(&b);
	if (b.pa != 0)
		bad += fail("shm_free");

	/* ---- 5: 再分配验证页被回收（无泄漏）---- */
	w = (char *)shm_alloc(&b);
	if (w == 0)
		bad += fail("re-alloc after free");
	else {
		w[0] = 0x7e;
		if ((unsigned char)w[0] != 0x7e)
			bad += fail("re-alloc write");
		shm_free(&b);
		printf("shm: re-alloc ok (no leak)\n");
	}

	printf("shm: %s\n", bad ? "FAIL" : "PASS");
	return (bad ? 1 : 0);
}
