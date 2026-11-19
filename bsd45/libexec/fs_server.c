/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * fs_server.c - S400 user-space filesystem service (microkernel variant).
 * fs_server.c - S400 用户态文件系统服务（微内核变体）。
 *
 * Registers PORT_FS, loops receiving IPC requests from user clients, and
 * serves an in-memory tmpfs: open/read/write/stat/close.  Runs entirely in
 * user space; a crash only makes /srv unavailable, never traps the kernel.
 * If killed, the kernel closes the port and wake blocked clients with -EIO;
 * init can restart this binary (see S400 init supervision).
 * 注册 PORT_FS，循环接收用户客户端的 IPC 请求，提供内存 tmpfs 服务。完全运行在
 * 用户态；崩溃只会让 /srv 不可用，不会 TRAP 内核。被 kill 后内核关闭端口并给阻塞
 * 客户端返回 -EIO，init 可重启本程序。
 */
#include <string.h>
#include <srv.h>

#define SYS_PORT_REGISTER	452
#define SYS_PORT_RELEASE	453
#define SYS_SEND		454
#define SYS_RECV		455

#define ENOENT		2
#define EBADF		9
#define ENOSPC		28
#define EEXIST		17

#define NFILES		16
#define FSIZE		512

static int
syscall4(int n, int a, int b, int c, int d)
{
	int r;

	__asm__ volatile("int $0x80"
	    : "=a"(r)
	    : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d)
	    : "memory");
	return (r);
}

#define port_register(p)	syscall4(SYS_PORT_REGISTER,(p),0,0,0)
#define port_release(p)		syscall4(SYS_PORT_RELEASE,(p),0,0,0)
#define ipc_send(p,m)		syscall4(SYS_SEND,(p),(int)(m),0,0)
#define ipc_recv(p,m)		syscall4(SYS_RECV,(p),(int)(m),0,0)

struct sfile {
	int		used;
	char		name[32];
	char		data[FSIZE];
	unsigned	size;
};
static struct sfile files[NFILES];

/* 中文：按名字找文件，找不到返回 -1 */
/* EN: find a file by name, -1 if absent */
static int
find_file(const char *name)
{
	int i;

	for (i = 0; i < NFILES; i++)
		if (files[i].used && strcmp(files[i].name, name) == 0)
			return (i);
	return (-1);
}

/* 中文：处理一条请求，把结果写进 rep，返回是否有回复（总是 1） */
/* EN: handle one request, fill rep; always yields a reply */
static void
handle(struct srv_msg *req, struct srv_msg *rep)
{
	struct srv_args *ra = (struct srv_args *)req->inline_data;
	struct srv_args *wa = (struct srv_args *)rep->inline_data;
	const char *name;
	int idx;

	memset(rep, 0, sizeof(*rep));
	rep->type = req->type;
	switch (req->type) {
	case SRV_SHUTDOWN:
		/* 中文：受控退出，模拟服务崩溃——内核会释放端口并给阻塞客户端 EIO。
		 * EN: controlled exit simulating a crash - kernel releases ports and
		 * gives blocked clients EIO. */
		wa->handle = 0;
		break;
	case SRV_OPEN:
		name = req->inline_data + sizeof(struct srv_args);
		idx = find_file(name);
		if (idx < 0) {
			for (idx = 0; idx < NFILES; idx++)
				if (!files[idx].used)
					break;
			if (idx >= NFILES) {
				wa->handle = (unsigned)-ENOSPC;
				break;
			}
			files[idx].used = 1;
			strncpy(files[idx].name, name, sizeof(files[idx].name) - 1);
			files[idx].name[sizeof(files[idx].name) - 1] = 0;
			files[idx].size = 0;
		}
		wa->handle = (unsigned)idx;	/* 句柄即槽位索引 */
		break;
	case SRV_WRITE:
		idx = (int)ra->handle;
		if (idx < 0 || idx >= NFILES || !files[idx].used) {
			wa->handle = (unsigned)-EBADF;
			break;
		}
		if (ra->arg > sizeof(files[idx].data) - 1)
			ra->arg = sizeof(files[idx].data) - 1;
		memcpy(files[idx].data, req->inline_data + sizeof(struct srv_args),
		    ra->arg);
		files[idx].data[ra->arg] = 0;
		files[idx].size = ra->arg;
		wa->handle = (int)ra->arg;
		break;
	case SRV_READ:
		idx = (int)ra->handle;
		if (idx < 0 || idx >= NFILES || !files[idx].used) {
			wa->handle = (unsigned)-EBADF;
			break;
		}
		if (ra->arg > files[idx].size)
			ra->arg = files[idx].size;
		memcpy(rep->inline_data + sizeof(struct srv_args),
		    files[idx].data, ra->arg);
		rep->len = sizeof(struct srv_args) + ra->arg;
		wa->handle = (int)ra->arg;
		break;
	case SRV_STAT:
		idx = (int)ra->handle;
		if (idx < 0 || idx >= NFILES || !files[idx].used) {
			wa->handle = (unsigned)-EBADF;
			break;
		}
		wa->handle = 0;
		wa->arg = files[idx].size;
		break;
	case SRV_CLOSE:
		idx = (int)ra->handle;
		if (idx >= 0 && idx < NFILES)
			files[idx].used = 0;
		wa->handle = 0;
		break;
	default:
		wa->handle = (unsigned)-1;
		break;
	}
	rep->len = rep->len ? rep->len : sizeof(struct srv_args);
}

int
main(void)
{
	struct srv_msg req, rep;
	int n;

	/* 中文：拥有请求端口与回复端口。回复走独立端口，避免服务端 recv 到自己的
	 * 回复；两端口同属本服务，退出时被内核一并释放，阻塞客户端得到 EIO。
	 * EN: Own both request and reply ports.  Replies go to a separate port so
	 * the server never receives its own reply; both are owned by this service,
	 * so on exit the kernel releases them and blocked clients get EIO. */
	if (port_register(SRV_PORT) < 0)
		return (2);
	if (port_register(SRV_REPLY_PORT) < 0) {
		port_release(SRV_PORT);
		return (2);
	}
	for (;;) {
		n = ipc_recv(SRV_PORT, &req);
		if (n < 0)
			break;		/* -EIO: 端口被关闭 */
		if (req.type == SRV_SHUTDOWN)
			break;		/* 受控退出 / controlled exit */
		handle(&req, &rep);
		ipc_send(SRV_REPLY_PORT, &rep);
	}
	port_release(SRV_PORT);
	port_release(SRV_REPLY_PORT);
	return (0);
}
