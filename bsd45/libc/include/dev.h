/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * dev.h - S401 unified device model: user-space device service protocol.
 * dev.h - S401 统一设备模型：用户态设备服务协议。
 *
 * 中文：统一设备模型把"设备命名 + 分派"从内核硬编码 strcmp + FD_* + switch
 * 散射逻辑，收敛为一个用户态服务 devfs_server。它维护统一的设备对象注册表
 * （struct devobj：名字 + open/read/write/ioctl/close 操作表），客户端经 IPC
 * 端口按"名字→句柄→统一 ops 分派"访问设备。服务崩溃只让设备不可用，内核不
 * TRAP，在飞请求返回 -EIO，服务可被重启。默认不使用服务时系统与原内核一致。
 *
 * EN: The unified device model moves device naming and dispatch out of the
 * kernel's hard-coded strcmp + FD_* + switch scatter into one user-space
 * service, devfs_server.  It keeps a uniform device-object registry
 * (struct devobj: name + open/read/write/ioctl/close ops table); clients reach
 * devices by "name -> handle -> uniform ops dispatch" over an IPC port.  A
 * service crash only makes devices unavailable, never traps the kernel: in-
 * flight requests get -EIO and the service can be restarted.  Without the
 * service the system behaves exactly as before.
 */
#ifndef _DEV_H_
#define _DEV_H_

/*
 * 中文：端口语义贴合内核 kipc.h 的 PORT_BDEV=2；回复端口 3。fs_server 占
 * 端口 0/1，两者不冲突。
 * EN: Request port matches kernel PORT_BDEV (2); reply port is 3.  fs_server
 * uses ports 0/1, so there is no collision.
 */
#define DEV_PORT		2	/* 统一设备服务请求端口 / device service request port */
#define DEV_REPLY_PORT		3	/* 统一设备服务回复端口 / device service reply port */

/* 操作码 / operation codes (stored in the message 'type' field) */
#define DEV_OPEN		1	/* 按名字打开设备 / open device by name */
#define DEV_READ		2	/* 读设备 / read from device */
#define DEV_WRITE		3	/* 写设备 / write to device */
#define DEV_IOCTL		4	/* 设备控制 / device control */
#define DEV_CLOSE		5	/* 关闭句柄 / close handle */
#define DEV_LIST		6	/* 枚举注册表 / enumerate registry */
#define DEV_SHUTDOWN		7	/* 受控退出（故障注入）/ controlled exit */

#define DEV_DATA_MAX		56	/* inline_data 数据区上限 / payload capacity */

/*
 * 中文：消息布局必须与内核 struct ipc_msg 逐字节一致：
 *   from, type, len, pa, reply_port (5×u32) + char inline_data[64] = 84 字节。
 * op 存 type；handle/arg 编码在 inline_data 前 8 字节；数据随后。
 * EN: Layout MUST byte-match kernel struct ipc_msg:
 *   from, type, len, pa, reply_port (5×u32) + char inline_data[64] = 84 bytes.
 * op goes in 'type'; handle/arg are encoded in the first 8 inline bytes;
 * payload follows.
 */
struct dev_msg {
	unsigned	from;
	unsigned	type;		/* DEV_* 操作码 / operation */
	unsigned	len;		/* inline_data 有效字节 / valid inline bytes */
	unsigned	pa;		/* 共享页 PA / shared page PA */
	unsigned	reply_port;
	char		inline_data[64];
};

/* inline_data 前 8 字节的参数块 / argument block in first 8 inline bytes */
struct dev_args {
	unsigned	handle;		/* 设备句柄或服务返回码 / handle or return code */
	unsigned	arg;		/* offset / len / cmd / flags 等 / generic arg */
};

/*
 * 客户端 API（libexec/devtest.c 内嵌实现，返回 >=0 成功，<0 为 -errno）。
 * Client API (implemented inline in devtest.c; >=0 success, <0 is -errno).
 */
int	dev_open(const char *name);
int	dev_read(int handle, void *buf, unsigned len);
int	dev_write(int handle, const void *buf, unsigned len);
int	dev_ioctl(int handle, unsigned cmd, unsigned arg);
int	dev_close(int handle);
int	dev_list(char *buf, unsigned len);

#endif /* _DEV_H_ */
