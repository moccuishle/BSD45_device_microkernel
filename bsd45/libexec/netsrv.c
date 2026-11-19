/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * netsrv.c - S403 user-space network stack service (UDP/IP + loopback NIC).
 * netsrv.c - S403 用户态网络协议栈服务（UDP/IP + 环回网卡）。
 *
 * 中文：TCP/IP 协议栈运行在用户态。内部有三层：
 *   ① 协议分派表 proto_dispatch[]——按协议类型分派（UDP 已实现，TCP 预留），
 *      这是"协议栈在用户态"的实体，新增协议只需加表项。
 *   ② 用户态 socket 表 socks[]——每个 socket 有本地/对端端口与接收数据报队列。
 *   ③ 环回虚拟网卡 loopback——发往本机端口的数据报直接投递到目标 socket 队列；
 *      发往外部地址的包计入 npriv（无物理网卡时丢弃，驱动上移是后续阶段）。
 * 服务崩溃→内核释放端口→阻塞客户端 EIO→可重启，与 fs/dev 服务同构。
 *
 * EN: The TCP/IP stack runs in user space.  Three layers:
 *   1) protocol dispatch table proto_dispatch[] (UDP implemented, TCP reserved)
 *      — the entity of "stack in user space"; add a protocol = add a table entry.
 *   2) user-space socket table socks[], each with local/peer ports and an RX
 *      datagram queue.
 *   3) loopback virtual NIC — datagrams to local ports are delivered straight
 *      into the target socket's queue; external packets are counted (npriv) and
 *      dropped (no physical NIC; driver upmove is a later phase).
 * Service crash -> kernel releases ports -> blocked clients get EIO -> restart.
 */
#include <string.h>
#include <net.h>

#define SYS_PORT_REGISTER	452
#define SYS_PORT_RELEASE	453
#define SYS_SEND		454
#define SYS_RECV		455

#define NENTSOCKS	8
#define NPKT		16

static int
syscall4(int n, int a, int b, int c, int d)
{
	int r;
	__asm__ volatile("int $0x80" : "=a"(r)
	    : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d) : "memory");
	return (r);
}

#define port_register(p)	syscall4(SYS_PORT_REGISTER,(p),0,0,0)
#define port_release(p)		syscall4(SYS_PORT_RELEASE,(p),0,0,0)
#define ipc_send(p,m)		syscall4(SYS_SEND,(p),(int)(m),0,0)
#define ipc_recv(p,m)		syscall4(SYS_RECV,(p),(int)(m),0,0)

/* 一个数据报 / one datagram */
struct pkt {
	int		used;
	unsigned	src_port;
	char		data[NET_DATA_MAX];
	unsigned	len;
};

/* 用户态 socket / user-space socket */
struct usock {
	int		used;
	int		domain;
	int		type;
	unsigned	local_port;
	unsigned	peer_port;
	struct pkt	rx[NPKT];	/* 接收队列（有界）/ bounded RX queue */
};
static struct usock socks[NENTSOCKS];

/* 协议栈统计 / stack statistics */
static unsigned n_sent, n_recv, n_loop;

/* ------------------------------------------------------------------ */
/* 协议分派层（用户态 TCP/IP 栈的"协议开关表"）                          */
/* ------------------------------------------------------------------ */

/*
 * 中文：每个协议提供 send 处理函数。UDP 已实现；TCP 槽位预留（.realize=0）。
 * 这对应内核 cdevsw 的用户态等价物——协议知识在服务里，不在内核。
 * EN: Each protocol provides a send handler.  UDP implemented; TCP reserved
 * (realize=0).  This is the user-space equivalent of a kernel protocol switch
 * table — protocol knowledge lives in the service, not the kernel.
 */
struct protoent {
	int		type;		/* NET_SOCK_* / 协议类型 */
	int		realize;	/* 是否已实现 / implemented? */
	const char	*name;
	int		(*send)(struct usock *s, unsigned dst_port,
			    const char *data, unsigned len);
};

/*
 * 中文：环回虚拟网卡——若 dst_port 有绑定的本机 socket，把包入其 rx 队列并
 * 计数；否则视为发往外部（无物理网卡，丢弃但计入 n_sent）。
 * EN: Loopback virtual NIC — if dst_port has a bound local socket, enqueue the
 * packet into its rx queue and count it; otherwise treat as external (no
 * physical NIC, drop but still count in n_sent).
 */
static int
loopback_send(struct usock *src, unsigned dst_port, const char *data, unsigned len)
{
	struct usock *d;
	int i, k;

	for (i = 0; i < NENTSOCKS; i++) {
		d = &socks[i];
		if (!d->used || d->local_port != dst_port || d == src)
			continue;
		for (k = 0; k < NPKT && d->rx[k].used; k++)
			;
		if (k >= NPKT)
			break;			/* 目标队列满 / target queue full */
		d->rx[k].used = 1;
		d->rx[k].src_port = src ? src->local_port : 0;
		if (len > NET_DATA_MAX)
			len = NET_DATA_MAX;
		memcpy(d->rx[k].data, data, len);
		d->rx[k].len = len;
		n_loop++;
		return ((int)len);
	}
	/* 外部目标：无物理网卡，计入已发送但丢弃 */
	/* external target: no physical NIC, count as sent but drop */
	return ((int)len);
}

/* UDP 发送处理：交给环回网卡（未来物理网卡驱动上移后走驱动） */
/* UDP send handler: hand to loopback NIC (future: physical NIC driver) */
static int
udp_send(struct usock *s, unsigned dst_port, const char *data, unsigned len)
{
	return (loopback_send(s, dst_port, data, len));
}

/* 协议分派表 / protocol dispatch table */
static struct protoent proto_dispatch[] = {
	{ NET_SOCK_DGRAM, 1, "udp", udp_send },
	{ 1 /* NET_SOCK_STREAM / TCP */, 0, "tcp", 0 /* 预留 / reserved */ },
};
#define NPROTO ((int)(sizeof(proto_dispatch) / sizeof(proto_dispatch[0])))

static struct protoent *
find_proto(int type)
{
	int i;

	for (i = 0; i < NPROTO; i++)
		if (proto_dispatch[i].type == type && proto_dispatch[i].realize)
			return (&proto_dispatch[i]);
	return (0);
}

/* ------------------------------------------------------------------ */
/* 请求处理 / request handling                                          */
/* ------------------------------------------------------------------ */

static void
handle(struct net_msg *req, struct net_msg *rep)
{
	struct net_args *ra = (struct net_args *)req->inline_data;
	struct net_args *wa = (struct net_args *)rep->inline_data;
	const char *payload = req->inline_data + sizeof(struct net_args);
	char *out = rep->inline_data + sizeof(struct net_args);
	struct protoent *pr;
	struct usock *s;
	struct net_stats *st;
	int h, i, k, n;

	memset(rep, 0, sizeof(*rep));
	rep->type = req->type;
	wa->handle = (unsigned)(-ENET_EINVAL);

	switch (req->type) {
	case NET_SOCKET:
		pr = find_proto((int)ra->arg);
		if (pr == 0) {		/* 不支持的协议 / unsupported protocol */
			wa->handle = (unsigned)(-ENET_EINVAL);
			break;
		}
		for (i = 0; i < NENTSOCKS && socks[i].used; i++)
			;
		if (i >= NENTSOCKS) {
			wa->handle = (unsigned)(-ENET_EIO);
			break;
		}
		s = &socks[i];
		memset(s, 0, sizeof(*s));
		s->used = 1;
		s->domain = (int)ra->handle;	/* AF_INET */
		s->type = (int)ra->arg;
		wa->handle = (unsigned)i;
		break;

	case NET_BIND:
		h = (int)ra->handle;
		if (h < 0 || h >= NENTSOCKS || !socks[h].used) {
			wa->handle = (unsigned)(-ENET_EBADF);
			break;
		}
		/* 端口唯一性检查 / unique-port check */
		for (i = 0; i < NENTSOCKS; i++)
			if (socks[i].used && i != h && socks[i].local_port == ra->arg) {
				wa->handle = (unsigned)(-ENET_EADDRINUSE);
				break;
			}
		if (i < NENTSOCKS) {
			wa->handle = (unsigned)(-ENET_EADDRINUSE);
			break;
		}
		socks[h].local_port = ra->arg;
		wa->handle = 0;
		break;

	case NET_CONNECT:
		h = (int)ra->handle;
		if (h < 0 || h >= NENTSOCKS || !socks[h].used) {
			wa->handle = (unsigned)(-ENET_EBADF);
			break;
		}
		socks[h].peer_port = ra->arg;
		wa->handle = 0;
		break;

	case NET_SEND:
		h = (int)ra->handle;
		if (h < 0 || h >= NENTSOCKS || !socks[h].used) {
			wa->handle = (unsigned)(-ENET_EBADF);
			break;
		}
		s = &socks[h];
		pr = find_proto(s->type);
		if (pr == 0) {
			wa->handle = (unsigned)(-ENET_EINVAL);
			break;
		}
		n = (int)ra->arg;
		if (n > NET_DATA_MAX)
			n = NET_DATA_MAX;
		n = pr->send(s, ra->peer_port, payload, (unsigned)n);
		n_sent++;
		wa->handle = (unsigned)n;
		break;

	case NET_RECV:
		h = (int)ra->handle;
		if (h < 0 || h >= NENTSOCKS || !socks[h].used) {
			wa->handle = (unsigned)(-ENET_EBADF);
			break;
		}
		s = &socks[h];
		for (k = 0; k < NPKT && !s->rx[k].used; k++)
			;
		if (k >= NPKT) {
			wa->handle = (unsigned)(-ENET_EAGAIN);
			break;
		}
		n = (int)s->rx[k].len;
		memcpy(out, s->rx[k].data, (unsigned)n);
		wa->peer_port = s->rx[k].src_port;
		memset(&s->rx[k], 0, sizeof(s->rx[k]));
		n_recv++;
		rep->len = sizeof(struct net_args) + (unsigned)n;
		wa->handle = (unsigned)n;
		break;

	case NET_CLOSE:
		h = (int)ra->handle;
		if (h < 0 || h >= NENTSOCKS || !socks[h].used) {
			wa->handle = (unsigned)(-ENET_EBADF);
			break;
		}
		memset(&socks[h], 0, sizeof(socks[h]));
		wa->handle = 0;
		break;

	case NET_STAT:
		st = (struct net_stats *)out;
		st->nsockets = 0;
		for (i = 0; i < NENTSOCKS; i++)
			if (socks[i].used)
				st->nsockets++;
		st->npriv = n_sent;
		st->nprec = n_recv;
		st->nloop = n_loop;
		rep->len = sizeof(struct net_args) + sizeof(struct net_stats);
		wa->handle = 0;
		break;

	default:
		wa->handle = (unsigned)(-ENET_EINVAL);
		break;
	}

	if (rep->len == 0)
		rep->len = sizeof(struct net_args);
}

int
main(void)
{
	struct net_msg req, rep;
	int n;

	if (port_register(NET_PORT) < 0)
		return (2);
	if (port_register(NET_REPLY_PORT) < 0) {
		port_release(NET_PORT);
		return (2);
	}
	for (;;) {
		n = ipc_recv(NET_PORT, &req);
		if (n < 0)
			break;
		if (req.type == NET_SHUTDOWN)
			break;
		handle(&req, &rep);
		ipc_send(NET_REPLY_PORT, &rep);
	}
	port_release(NET_PORT);
	port_release(NET_REPLY_PORT);
	return (0);
}
