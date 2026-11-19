/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * nettest.c - S403 acceptance: user-space network stack (UDP/IP + loopback)
 *             delivered as a fault-isolated microkernel service.
 * nettest.c - S403 验收：用户态网络协议栈（UDP/IP + 环回）作为故障隔离的
 *            微内核服务。
 *
 * 1. fork/exec /bin/netsrv
 * 2. 创建两个 UDP socket，分别 bind 端口，connect 对端
 * 3. A 发送 → 环回网卡投递 → B 接收（协议栈在用户态完成）
 * 4. NET_STAT 验证统计（发包/收包/环回计数）
 * 5. NET_SHUTDOWN → 后续请求 -EIO（故障隔离，内核无 TRAP）
 * 6. 重启服务后恢复
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <net.h>

#define SYS_SEND	454
#define SYS_RECV	455

static int
syscall4(int n, int a, int b, int c, int d)
{
	int r;
	__asm__ volatile("int $0x80" : "=a"(r)
	    : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d) : "memory");
	return (r);
}

#define ipc_send(p,m)	syscall4(SYS_SEND,(p),(int)(m),0,0)
#define ipc_recv(p,m)	syscall4(SYS_RECV,(p),(int)(m),0,0)

static int
net_call(struct net_msg *req, struct net_msg *rep)
{
	int n;

	n = ipc_send(NET_PORT, req);
	if (n < 0)
		return (n);
	for (;;) {
		n = ipc_recv(NET_REPLY_PORT, rep);
		if (n == 0)
			return (0);
		if (n != -35)
			return (n);		/* -EIO 服务退出 */
	}
}

/* ---- 客户端 net_* API ---- */
int
net_socket(int domain, int type)
{
	struct net_msg req, rep;
	struct net_args *a = (struct net_args *)req.inline_data;
	int n;

	memset(&req, 0, sizeof(req));
	req.type = NET_SOCKET;
	a->handle = (unsigned)domain;
	a->arg = (unsigned)type;
	req.len = sizeof(struct net_args);
	n = net_call(&req, &rep);
	if (n < 0)
		return (n);
	a = (struct net_args *)rep.inline_data;
	return ((int)a->handle);
}

int
net_bind(int h, unsigned port)
{
	struct net_msg req, rep;
	struct net_args *a = (struct net_args *)req.inline_data;
	int n;

	memset(&req, 0, sizeof(req));
	req.type = NET_BIND;
	a->handle = (unsigned)h;
	a->arg = port;
	req.len = sizeof(struct net_args);
	n = net_call(&req, &rep);
	if (n < 0)
		return (n);
	a = (struct net_args *)rep.inline_data;
	return ((int)a->handle);
}

int
net_connect(int h, unsigned port)
{
	struct net_msg req, rep;
	struct net_args *a = (struct net_args *)req.inline_data;
	int n;

	memset(&req, 0, sizeof(req));
	req.type = NET_CONNECT;
	a->handle = (unsigned)h;
	a->arg = port;
	req.len = sizeof(struct net_args);
	n = net_call(&req, &rep);
	if (n < 0)
		return (n);
	a = (struct net_args *)rep.inline_data;
	return ((int)a->handle);
}

int
net_send(int h, const void *buf, unsigned len, unsigned peer)
{
	struct net_msg req, rep;
	struct net_args *a = (struct net_args *)req.inline_data;
	int n;

	if (len > NET_DATA_MAX)
		len = NET_DATA_MAX;
	memset(&req, 0, sizeof(req));
	req.type = NET_SEND;
	a->handle = (unsigned)h;
	a->arg = len;
	a->peer_port = peer;
	memcpy(req.inline_data + sizeof(struct net_args), buf, len);
	req.len = sizeof(struct net_args) + len;
	n = net_call(&req, &rep);
	if (n < 0)
		return (n);
	a = (struct net_args *)rep.inline_data;
	return ((int)a->handle);
}

int
net_recv(int h, void *buf, unsigned len)
{
	struct net_msg req, rep;
	struct net_args *a = (struct net_args *)req.inline_data;
	int n;

	if (len > NET_DATA_MAX)
		len = NET_DATA_MAX;
	memset(&req, 0, sizeof(req));
	req.type = NET_RECV;
	a->handle = (unsigned)h;
	a->arg = len;
	req.len = sizeof(struct net_args);
	n = net_call(&req, &rep);
	if (n < 0)
		return (n);
	a = (struct net_args *)rep.inline_data;
	if ((int)a->handle < 0)
		return ((int)a->handle);
	n = (int)rep.len - (int)sizeof(struct net_args);
	if (n < 0)
		n = 0;
	if ((unsigned)n > len)
		n = (int)len;
	memcpy(buf, rep.inline_data + sizeof(struct net_args), (unsigned)n);
	return (n);
}

int
net_close(int h)
{
	struct net_msg req, rep;
	struct net_args *a = (struct net_args *)req.inline_data;
	int n;

	memset(&req, 0, sizeof(req));
	req.type = NET_CLOSE;
	a->handle = (unsigned)h;
	req.len = sizeof(struct net_args);
	n = net_call(&req, &rep);
	if (n < 0)
		return (n);
	a = (struct net_args *)rep.inline_data;
	return ((int)a->handle);
}

int
net_stat(struct net_stats *s)
{
	struct net_msg req, rep;
	struct net_args *a = (struct net_args *)rep.inline_data;
	int n;

	memset(&req, 0, sizeof(req));
	req.type = NET_STAT;
	req.len = sizeof(struct net_args);
	n = net_call(&req, &rep);
	if (n < 0)
		return (n);
	if ((int)a->handle < 0)
		return ((int)a->handle);
	memcpy(s, rep.inline_data + sizeof(struct net_args), sizeof(*s));
	return (0);
}

static void
net_shutdown(void)
{
	struct net_msg req;

	memset(&req, 0, sizeof(req));
	req.type = NET_SHUTDOWN;
	req.len = sizeof(struct net_args);
	ipc_send(NET_PORT, &req);
}

/* ---- 测试框架 / harness ---- */
static int
fail(const char *what)
{
	printf("net: %s FAILED\n", what);
	return (1);
}

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
		av[0] = "/bin/netsrv";
		av[1] = 0;
		ev[0] = 0;
		execve("/bin/netsrv", av, ev);
		_exit(127);
	}
	return (pid);
}

int
main(void)
{
	int srv, bad = 0, st, a, b;
	struct net_stats stats;
	char buf[NET_DATA_MAX + 1];

	srv = start_server();
	if (srv < 0)
		return (fail("fork server"));
	delay(3000000ul);

	/* ---- 1: 创建 + 绑定 + 连接两个 UDP socket ---- */
	a = net_socket(NET_AF_INET, NET_SOCK_DGRAM);
	b = net_socket(NET_AF_INET, NET_SOCK_DGRAM);
	if (a < 0 || b < 0)
		bad += fail("net_socket");
	if (net_bind(a, 1000) < 0 || net_bind(b, 2000) < 0)
		bad += fail("net_bind");
	if (net_connect(a, 2000) < 0)
		bad += fail("net_connect");

	/* ---- 2: 环回 UDP 收发（协议栈在用户态）---- */
	if (net_send(a, "udp-packet", 10, 2000) != 10)
		bad += fail("net_send");
	memset(buf, 0, sizeof(buf));
	if (net_recv(b, buf, 10) != 10 || strcmp(buf, "udp-packet") != 0) {
		printf("net: recv buf=[%s]\n", buf);
		bad += fail("net_recv loopback");
	}
	if (bad == 0)
		printf("net: udp loopback ok\n");

	/* ---- 3: 统计 ---- */
	if (net_stat(&stats) < 0)
		bad += fail("net_stat");
	else if (stats.nloop < 1 || stats.nprec < 1 || stats.npriv < 1)
		bad += fail("net_stat counters");
	else
		printf("net: stats sent=%d recv=%d loop=%d socks=%d\n",
		    stats.npriv, stats.nprec, stats.nloop, stats.nsockets);

	net_close(a);
	net_close(b);

	/* ---- 4: 故障隔离——服务退出后请求返回 EIO ---- */
	net_shutdown();
	wait4(srv, &st, 0, 0);
	delay(1000000ul);
	if (net_socket(NET_AF_INET, NET_SOCK_DGRAM) != -5)	/* -EIO */
		bad += fail("service crash -> EIO");
	else
		printf("net: fault isolation ok (EIO)\n");

	/* ---- 5: 重启恢复 ---- */
	srv = start_server();
	if (srv < 0)
		return (fail("restart fork"));
	delay(6000000ul);
	a = net_socket(NET_AF_INET, NET_SOCK_DGRAM);
	if (a < 0 || net_bind(a, 3000) < 0)
		bad += fail("restart socket/bind");
	else
		printf("net: restart ok\n");
	net_close(a);
	net_shutdown();
	wait4(srv, &st, 0, 0);

	printf("net: %s\n", bad ? "FAIL" : "PASS");
	return (bad ? 1 : 0);
}
