/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * srv.h - S400 microkernel file-service protocol shared by fs_server and
 *         its user-space clients (srv_open/read/write/close).
 * srv.h - S400 微内核文件服务协议，fs_server 与其用户态客户端共用。
 *
 * The service lives in user space and exposes PORT_FS over kernel IPC (KP-6).
 * Clients send a request and block in ipc_recv (real rendezvous); the server
 * sends the reply back.  Small payloads are inline; bulk data uses KP-7 pages.
 */
#ifndef _SRV_H_
#define _SRV_H_

#define SRV_PORT		0	/* 请求端口：客户端 -> fs_server / request port */
#define SRV_REPLY_PORT		1	/* 回复端口：fs_server -> 客户端 / reply port */
/* 两个端口都由 fs_server 拥有：服务退出时内核释放二者，阻塞在回复端口的
 * 客户端收到 -EIO。请求与回复分离避免 fs_server recv 到自己的回复。
 * Both ports are owned by fs_server: on exit the kernel releases both, so
 * clients blocked on the reply port get -EIO.  Splitting request/reply avoids
 * the server receiving its own reply. */

/* 操作码 / operation codes (stored in the message 'type' field) */
#define SRV_OPEN		1
#define SRV_READ		2
#define SRV_WRITE		3
#define SRV_CLOSE		4
#define SRV_STAT		5
#define SRV_SHUTDOWN		6	/* 受控退出，模拟服务崩溃 / controlled exit, simulates crash */

#define SRV_DATA_MAX		56	/* inline_data 中数据区上限 / data area size */

/*
 * 中文：消息布局必须与内核 struct ipc_msg 逐字节一致：
 *   from, type, len, pa, reply_port (5×u32) + char inline_data[64] = 84 字节。
 * op 存 type；handle/offset/size 等参数编码在 inline_data 前 8 字节；数据随后。
 * EN: layout MUST byte-match kernel struct ipc_msg:
 *   from, type, len, pa, reply_port (5×u32) + char inline_data[64] = 84 bytes.
 * op goes in 'type'; handle/offset/size are encoded in the first 8 bytes of
 * inline_data; payload follows.
 */
struct srv_msg {
	unsigned	from;
	unsigned	type;		/* SRV_* op / 操作码 */
	unsigned	len;		/* inline_data 有效字节 / valid inline bytes */
	unsigned	pa;		/* 共享页 PA / shared page PA */
	unsigned	reply_port;
	char		inline_data[64];
};

/* inline_data 前 8 字节的参数块 / argument block in first 8 inline bytes */
struct srv_args {
	unsigned	handle;		/* OPEN 返回 / returned by OPEN */
	unsigned	arg;		/* offset / flags / size 等 / offset/flags/size */
};

/* 客户端 API（libexec/srvlib.c，返回 >=0 成功，<0 为 -errno） */
int	srv_open(const char *path, int flags);
int	srv_read(int handle, void *buf, unsigned len);
int	srv_write(int handle, const void *buf, unsigned len);
int	srv_stat(int handle, unsigned *size);
int	srv_close(int handle);

#endif /* _SRV_H_ */
