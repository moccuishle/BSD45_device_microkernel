/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * net.h - S403 user-space network stack service protocol.
 * net.h - S403 用户态网络协议栈服务协议。
 *
 * 中文：MINIX 式网络服务化——TCP/IP 协议栈跑在用户态 netsrv 进程里，内核只
 * 提供 IPC 端口（未来加物理网卡的 packet 端口）。用户程序经本协议的 socket-like
 * API 通过 IPC 访问网络。当前实现一个完整可验证的**用户态 UDP/IP + 环回虚拟网卡**
 * 骨架：协议分派表、socket 表、端口绑定、数据报收发、故障隔离。物理网卡驱动上移
 * （需 IOPL/IRQ 地基）列为后续阶段，但协议栈本身已完全在用户态。
 * EN: MINIX-style network service — the TCP/IP stack runs in the user-space
 * netsrv process; the kernel only provides IPC ports (plus a future physical
 * NIC packet port).  User programs use this socket-like API over IPC.  This
 * delivers a fully verifiable **user-space UDP/IP + loopback virtual NIC**
 * skeleton: protocol dispatch table, socket table, port binding, datagram
 * send/recv, fault isolation.  Physical NIC driver upmove (needs IOPL/IRQ)
 * is a later phase, but the stack itself is entirely in user space.
 */
#ifndef _NET_H_
#define _NET_H_

/* 端口：请求 4 / 回复 5，避开 fs_server(0/1) 与 devfs_server(2/3) */
#define NET_PORT		4
#define NET_REPLY_PORT		5

/* 操作码 / operation codes (message 'type') */
#define NET_SOCKET		1	/* 创建 socket / create socket */
#define NET_BIND		2	/* 绑定本地端口 / bind local port */
#define NET_CONNECT		3	/* 设置对端 / set peer */
#define NET_SEND		4	/* 发送数据报 / send datagram */
#define NET_RECV		5	/* 接收数据报 / receive datagram */
#define NET_CLOSE		6	/* 关闭 socket / close socket */
#define NET_STAT		7	/* 查询统计 / query stats */
#define NET_SHUTDOWN		8	/* 受控退出 / controlled exit */

#define NET_DATA_MAX		56	/* inline 数据上限 / inline payload cap */
#define NET_AF_INET		2	/* AF_INET / 地址族 */
#define NET_SOCK_DGRAM		2	/* UDP / datagram socket */

/* errno（与内核 errno.h 一致）/ errno values */
#define ENET_EINVAL		22
#define ENET_EBADF		9
#define ENET_EAGAIN		35
#define ENET_EIO		5
#define ENET_EADDRINUSE		48

/*
 * 中文：消息布局必须与内核 struct ipc_msg 逐字节一致（84 字节）。
 * EN: Layout MUST byte-match kernel struct ipc_msg (84 bytes).
 */
struct net_msg {
	unsigned	from;
	unsigned	type;
	unsigned	len;
	unsigned	pa;
	unsigned	reply_port;
	char		inline_data[64];
};

/* inline_data 参数块：socket 句柄、参数、地址等 */
/* argument block in inline_data: handle, args, address */
struct net_args {
	unsigned	handle;		/* socket 句柄或返回码 / handle or return code */
	unsigned	arg;		/* 端口/长度/标志 / port/len/flags */
	unsigned	peer_port;	/* 对端端口 / peer port */
	unsigned	pad;
};

/* NET_STAT 回复：统计结构（放 inline_data）/ stats reply in inline_data */
struct net_stats {
	unsigned	nsockets;	/* 当前 socket 数 / live sockets */
	unsigned	npriv;		/* 已发送包数 / packets sent */
	unsigned	nprec;		/* 已接收包数 / packets received */
	unsigned	nloop;		/* 环回投递数 / loopback deliveries */
};

/* 客户端 API（nettest 内嵌实现，返回 >=0 成功，<0 为 -errno） */
int	net_socket(int domain, int type);
int	net_bind(int h, unsigned port);
int	net_connect(int h, unsigned port);
int	net_send(int h, const void *buf, unsigned len, unsigned peer);
int	net_recv(int h, void *buf, unsigned len);
int	net_close(int h);
int	net_stat(struct net_stats *s);

#endif /* _NET_H_ */
