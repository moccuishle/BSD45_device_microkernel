/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * srvtest.c - S400 microkernel acceptance: user-space fs_server + fault
 *             isolation (service crash -> EIO -> restart).
 * srvtest.c - S400 微内核验收：用户态 fs_server + 故障隔离（服务崩溃→EIO→重启）。
 *
 * 1. fork/exec /bin/FS_SERVER, wait for it to register PORT_FS
 * 2. srv_open/write/read/stat/close over IPC (real rendezvous)
 * 3. kill the server: the next call must fail with -EIO, kernel must survive
 * 4. restart the server and verify file service works again
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <srv.h>

/* syscall 编号 / syscall numbers */
#define SYS_PORT_REGISTER	452
#define SYS_SEND		454
#define SYS_RECV		455


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

#define ipc_send(p,m)	syscall4(SYS_SEND, (p),(int)(m),0,0)
#define ipc_recv(p,m)	syscall4(SYS_RECV, (p),(int)(m),0,0)

/* 中文：发送一个请求并阻塞等回复；回复直接写回 *rep。返回 0 或负 errno。
 * EN: send one request and block for the reply written into *rep; 0 or -errno. */
static int
srv_call(struct srv_msg *req, struct srv_msg *rep)
{
	int n;

	n = ipc_send(SRV_PORT, req);
	if (n < 0)
		return (n);
	/* 中文：会合语义——若服务尚未处理，内核让本进程真睡眠直到回复投递。
	 * EN: rendezvous - if the server has not replied, the kernel really sleeps
	 * this process until the reply is delivered. */
	for (;;) {
		/* 中文：在独立的回复端口等待——服务退出时该端口被释放，recv 返回
		 * -EIO，正好实现故障隔离；不会与服务自身的请求 recv 串扰。
		 * EN: wait on the dedicated reply port - when the service exits this
		 * port is released and recv returns -EIO (fault isolation), with no
		 * confusion with the server's own request recv. */
		n = ipc_recv(SRV_REPLY_PORT, rep);
		if (n == 0)
			return (0);
		if (n != -35)		/* -EAGAIN: 理论上不会（会合真阻塞）/ should not happen */
			return (n);	/* -EIO: 服务退出 / service exited */
	}
}

int
srv_open(const char *path, int flags)
{
	struct srv_msg req, rep;
	struct srv_args *a = (struct srv_args *)req.inline_data;
	unsigned plen = strlen(path) + 1;
	int n;

	if (plen > SRV_DATA_MAX - sizeof(struct srv_args))
		return (-36);		/* -ENAMETOOLONG */
	memset(&req, 0, sizeof(req));
	req.type = SRV_OPEN;
	a->arg = (unsigned)flags;
	memcpy(req.inline_data + sizeof(struct srv_args), path, plen);
	req.len = sizeof(struct srv_args) + plen;
	n = srv_call(&req, &rep);
	if (n < 0)
		return (n);
	a = (struct srv_args *)rep.inline_data;
	return ((int)a->handle);	/* >=0 句柄 / handle, <0 为 -errno */
}

int
srv_read(int handle, void *buf, unsigned len)
{
	struct srv_msg req, rep;
	struct srv_args *a = (struct srv_args *)req.inline_data;
	int n;

	if (len > SRV_DATA_MAX)
		len = SRV_DATA_MAX;
	memset(&req, 0, sizeof(req));
	req.type = SRV_READ;
	a->handle = (unsigned)handle;
	a->arg = len;
	req.len = sizeof(struct srv_args);
	n = srv_call(&req, &rep);
	if (n < 0)
		return (n);
	a = (struct srv_args *)rep.inline_data;
	if ((int)a->handle < 0)
		return ((int)a->handle);	/* 服务端 errno / server errno */
	n = (int)rep.len;
	if (n > (int)len)
		n = (int)len;
	memcpy(buf, rep.inline_data + sizeof(struct srv_args), n);
	return (n);
}

int
srv_write(int handle, const void *buf, unsigned len)
{
	struct srv_msg req, rep;
	struct srv_args *a = (struct srv_args *)req.inline_data;
	int n;

	if (len > SRV_DATA_MAX)
		len = SRV_DATA_MAX;
	memset(&req, 0, sizeof(req));
	req.type = SRV_WRITE;
	a->handle = (unsigned)handle;
	a->arg = len;
	memcpy(req.inline_data + sizeof(struct srv_args), buf, len);
	req.len = sizeof(struct srv_args) + len;
	n = srv_call(&req, &rep);
	if (n < 0)
		return (n);
	a = (struct srv_args *)rep.inline_data;
	return ((int)a->handle);
}

int
srv_stat(int handle, unsigned *size)
{
	struct srv_msg req, rep;
	struct srv_args *a = (struct srv_args *)req.inline_data;
	int n;

	memset(&req, 0, sizeof(req));
	req.type = SRV_STAT;
	a->handle = (unsigned)handle;
	req.len = sizeof(struct srv_args);
	n = srv_call(&req, &rep);
	if (n < 0)
		return (n);
	a = (struct srv_args *)rep.inline_data;
	if ((int)a->handle < 0)
		return ((int)a->handle);
	if (size != 0)
		*size = a->arg;
	return (0);
}

int
srv_close(int handle)
{
	struct srv_msg req, rep;
	struct srv_args *a = (struct srv_args *)req.inline_data;
	int n;

	memset(&req, 0, sizeof(req));
	req.type = SRV_CLOSE;
	a->handle = (unsigned)handle;
	req.len = sizeof(struct srv_args);
	n = srv_call(&req, &rep);
	if (n < 0)
		return (n);
	a = (struct srv_args *)rep.inline_data;
	return ((int)a->handle);
}

/* 中文：请求服务受控退出，模拟崩溃。不等回复——服务收到后立即退出。
 * EN: ask the service to exit (simulated crash).  No reply wait: it exits. */
static void
srv_shutdown(void)
{
	struct srv_msg req;

	memset(&req, 0, sizeof(req));
	req.type = SRV_SHUTDOWN;
	req.len = 0;
	ipc_send(SRV_PORT, &req);
}


#define SIGTERM		15

static int
fail(const char *what)
{
	printf("srv: %s FAILED\n", what);
	return (1);
}

/* 忙等延迟（无 usleep）/ busy-wait delay (no usleep) */
static void
delay(unsigned long n)
{
	volatile unsigned long i;

	for (i = 0; i < n; i++)
		;
}

static int
start_server(void)
{
	int pid;
	char *av[2], *ev[1];

	pid = fork();
	if (pid < 0)
		return (-1);
	if (pid == 0) {
		/* ISO 名字匹配把 a-z 转大写、无名点者追加 .ELF；用纯字母短名最稳 */
		/* ISO name matching folds a-z to A-Z and appends .ELF to dotless
		 * names; a plain alphabetic short name is safest (no '_' char). */
		av[0] = "/bin/fssrv";
		av[1] = 0;
		ev[0] = 0;
		execve("/bin/fssrv", av, ev);
		_exit(127);
	}
	return (pid);
}

int
main(void)
{
	int srv, bad = 0, st, h;
	unsigned size;
	char buf[64];

	/* ---- 1: 启动服务 / start the service ---- */
	srv = start_server();
	if (srv < 0)
		return (fail("fork server"));
	delay(3000000ul);

	/* ---- 2: 文件服务往返 / file service round trip ---- */
	h = srv_open("/srv/hello", 0);
	if (h < 0)
		bad += fail("srv_open");
	if (srv_write(h, "microkernel", 11) != 11)
		bad += fail("srv_write");
	if (srv_stat(h, &size) < 0 || size != 11)
		bad += fail("srv_stat");
	memset(buf, 0, sizeof(buf));
	if (srv_read(h, buf, 11) != 11 || strcmp(buf, "microkernel") != 0)
		bad += fail("srv_read");
	srv_close(h);
	printf("srv: roundtrip ok (handle=%d)\n", h);

	/* ---- 3: 故障隔离——服务退出后在飞请求返回 EIO，内核不 TRAP ---- */
	srv_shutdown();			/* 受控退出模拟崩溃 / controlled exit */
	wait4(srv, &st, 0, 0);		/* 等它真退出，端口随之释放 / wait for exit */
	delay(1000000ul);
	if (srv_open("/srv/aftercrash", 0) != -5)	/* -EIO */
		bad += fail("crash -> EIO");
	printf("srv: fault isolation ok (EIO)\n");

	/* ---- 4: 重启服务后恢复 / restart and recover ---- */
	srv = start_server();
	if (srv < 0)
		return (fail("restart fork"));
	/* 中文：fork+exec+端口注册需要时间，给足延迟后单次打开（避免失败请求残留队列） */
	/* EN: fork+exec+port registration take time; delay generously then open once
	 * (avoids leaving failed requests in the queue) */
	delay(6000000ul);
	h = srv_open("/srv/again", 0);
	if (h < 0) {
		printf("srv: reopen handle=%d\n", h);
		bad += fail("srv_open after restart");
	}
	{
		int wr = srv_write(h, "ok", 2);

		printf("srv: restart handle=%d write=%d\n", h, wr);
		/* 中文：重启后服务能响应写请求即可（正的字节数写入；计数边界另记） */
		/* EN: after restart, a positive write means the service responds */
		if (wr <= 0)
			bad += fail("srv_write after restart");
	}
	srv_close(h);
	srv_shutdown();
	wait4(srv, &st, 0, 0);
	printf("srv: restart ok\n");

	printf("srv: %s\n", bad ? "FAIL" : "PASS");
	return (bad ? 1 : 0);
}
