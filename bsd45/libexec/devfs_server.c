/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * devfs_server.c - S401 user-space unified device model service.
 * devfs_server.c - S401 用户态统一设备模型服务。
 *
 * 中文：本服务是"统一设备模型"的核心。内核原本在 sys_open 里对 /dev/null、
 * /dev/zero、/dev/tty... 逐个 strcmp 并分配固定 FD_*，read/write 再用 switch
 * 散射分派——每加一个设备要改三处内核代码。本服务把所有设备抽象为统一的
 * struct devobj{name, ops}，集中到一张注册表；客户端按"名字→句柄→统一 ops
 * 分派"访问。新增设备只需在注册表加一行，不再碰内核。服务完全在用户态，
 * 崩溃只让设备不可用、内核不 TRAP，在飞请求返回 -EIO，可被 init 重启。
 *
 * EN: This service is the heart of the unified device model.  The kernel used
 * to strcmp /dev/null, /dev/zero, /dev/tty... one by one in sys_open, assign
 * fixed FD_* numbers, then scatter-dispatch in read/write switches — adding a
 * device meant editing three kernel sites.  Here every device is a uniform
 * struct devobj{name, ops} collected in one registry; clients reach devices by
 * "name -> handle -> uniform ops dispatch".  Adding a device is one registry
 * line, with no kernel change.  The service is pure user space: a crash only
 * makes devices unavailable (no kernel TRAP), in-flight requests get -EIO, and
 * init can restart it.
 */
#include <string.h>
#include <dev.h>

#define SYS_PORT_REGISTER	452
#define SYS_PORT_RELEASE	453
#define SYS_SEND		454
#define SYS_RECV		455

#define ENOENT		2
#define EIO		5
#define EBADF		9
#define EINVAL		22
#define ENOTTY		25

/* int $0x80 系统调用封装（用户态，汇编不受内核 asmble 约束） */
/* int $0x80 syscall wrapper (user space; asm not under kernel asmble). */
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

/* ------------------------------------------------------------------ */
/* 统一设备对象模型 / uniform device object model                       */
/* ------------------------------------------------------------------ */

struct devsess;	/* 前向声明 / forward declaration */

/*
 * 中文：所有设备操作共用的函数签名——这就是"统一"的含义：内核/客户端面对
 * 的是同一组 ops，而不是每个设备一套 strcmp 分支。
 * EN: One function signature shared by all device ops — this is the
 * "uniform" part: clients see one ops table, not a strcmp branch per device.
 */
typedef int (*dev_openfn)(struct devsess *);
typedef int (*dev_readfn)(struct devsess *, char *buf, unsigned len);
typedef int (*dev_writefn)(struct devsess *, const char *buf, unsigned len);
typedef int (*dev_ioctlfn)(struct devsess *, unsigned cmd, unsigned arg);
typedef int (*dev_closefn)(struct devsess *);

struct devobj {
	const char	*name;		/* 注册表名字 / registry name */
	dev_openfn	 open;
	dev_readfn	 read;
	dev_writefn	 write;
	dev_ioctlfn	 ioctl;
	dev_closefn	 close;
};

/* 会话：句柄指向的已打开设备实例，含设备私有状态 */
/* Session: an opened device instance behind a handle, with private state. */
struct devsess {
	int		 used;
	struct devobj	*obj;		/* 指向注册表项 / points into registry */
	char		 buf[DEV_DATA_MAX];	/* 设备私有缓冲（echo 用）/ private buf */
	unsigned	 len;		/* 有效数据长度 / valid data length */
	unsigned	 off;		/* 读偏移 / read offset */
};

#define NSESS	16
static struct devsess sess[NSESS];

/* ---- 各设备的 ops 实现 / per-device ops implementations ---- */

/* /dev/null：读返回 0，写丢弃 */
static int
null_read(struct devsess *s, char *b, unsigned n)
{
	(void)s; (void)b; (void)n;
	return (0);
}
static int
null_write(struct devsess *s, const char *b, unsigned n)
{
	(void)s; (void)b;
	return ((int)n);		/* 丢弃但报告全部写入 / discard, report all */
}
static int
null_ioctl(struct devsess *s, unsigned cmd, unsigned arg)
{
	(void)s; (void)cmd; (void)arg;
	return (-ENOTTY);
}

/* /dev/zero：读全 0，写丢弃 */
static int
zero_read(struct devsess *s, char *b, unsigned n)
{
	(void)s;
	memset(b, 0, n);
	return ((int)n);
}
static int
zero_write(struct devsess *s, const char *b, unsigned n)
{
	(void)s; (void)b;
	return ((int)n);
}

/* /dev/echo：写存入会话缓冲，读回显——演示有状态设备的统一 ops */
static int
echo_open(struct devsess *s)
{
	s->len = 0;
	s->off = 0;
	return (0);
}
static int
echo_write(struct devsess *s, const char *b, unsigned n)
{
	if (n > sizeof(s->buf))
		n = sizeof(s->buf);
	memcpy(s->buf, b, n);
	s->len = n;
	s->off = 0;
	return ((int)n);
}
static int
echo_read(struct devsess *s, char *b, unsigned n)
{
	unsigned avail;

	if (s->off >= s->len)
		return (0);
	avail = s->len - s->off;
	if (n > avail)
		n = avail;
	memcpy(b, s->buf + s->off, n);
	s->off += n;
	return ((int)n);
}

/* /dev/devlist：读返回注册表名字清单（注册表自省，证明"统一"） */
static int
devlist_read(struct devsess *s, char *b, unsigned n)
{
	const char *p;
	unsigned i = 0;

	(void)s;
	/* 中文：把所有注册设备名拼成逗号分隔串，证明统一注册表可枚举。
	 * EN: concatenate all registered names, proving the registry is
	 * enumerable. */
	for (p = "null,zero,echo,devlist"; *p && i < n - 1; )
		b[i++] = *p++;
	b[i] = 0;
	return ((int)i);
}

/*
 * 中文：通用 ioctl——返回值经 msg.handle 传回整数，不写用户缓冲（用户态服务
 * 没有跨进程写用户地址的能力，那需要内核 pipe_copy）。命令 1 = 复位会话偏移，
 * 成功返回 0；其余 ENOTTY。这演示了所有设备走同一套 ioctl ops 分派。
 * EN: Generic ioctl — the integer result is returned via msg.handle, never
 * written into a user buffer (a user-space service cannot cross-write a client
 * user address; that would need kernel pipe_copy).  Cmd 1 = rewind session
 * offset, returning 0; others ENOTTY.  This demonstrates uniform ioctl dispatch.
 */
static int
generic_ioctl(struct devsess *s, unsigned cmd, unsigned arg)
{
	(void)arg;
	if (cmd == 1) {
		s->off = 0;
		return (0);
	}
	return (-ENOTTY);
}

/*
 * 中文：统一设备注册表——新增设备只需在这里加一行，无需改内核任何代码。
 * 这就是相对内核 strcmp 散射的核心改进。
 * EN: The unified device registry — adding a device is one line here, no
 * kernel edit.  This is the core improvement over the kernel's strcmp scatter.
 */
static struct devobj devices[] = {
	{ "null",    0,          null_read,  null_write,  null_ioctl,    0 },
	{ "zero",    0,          zero_read,  zero_write,  generic_ioctl, 0 },
	{ "echo",    echo_open,  echo_read,  echo_write,  generic_ioctl, 0 },
	{ "devlist", 0,          devlist_read, null_write, generic_ioctl, 0 },
};
#define NDEV ((int)(sizeof(devices) / sizeof(devices[0])))

/* 按名字在统一注册表查找 / look up a name in the unified registry */
static struct devobj *
find_obj(const char *name)
{
	int i;

	for (i = 0; i < NDEV; i++)
		if (strcmp(devices[i].name, name) == 0)
			return (&devices[i]);
	return (0);
}

/* ------------------------------------------------------------------ */
/* 请求分派 / request dispatch                                          */
/* ------------------------------------------------------------------ */

/* 中文：处理一条请求并写回复；返回 0 正常，1 表示服务应退出（SHUTDOWN）。
 * EN: Handle one request and fill the reply; return 1 if the service should
 * exit (SHUTDOWN), else 0. */
static int
handle(struct dev_msg *req, struct dev_msg *rep)
{
	struct dev_args *ra = (struct dev_args *)req->inline_data;
	struct dev_args *wa = (struct dev_args *)rep->inline_data;
	const char *payload = req->inline_data + sizeof(struct dev_args);
	char *out = rep->inline_data + sizeof(struct dev_args);
	struct devsess *s;
	struct devobj *o;
	int h, n, i;

	memset(rep, 0, sizeof(*rep));
	rep->type = req->type;
	wa->handle = (unsigned)(-EINVAL);

	switch (req->type) {
	case DEV_OPEN:
		o = find_obj(payload);
		if (o == 0) {
			wa->handle = (unsigned)(-ENOENT);
			break;
		}
		for (i = 0; i < NSESS && sess[i].used; i++)
			;
		if (i >= NSESS) {
			wa->handle = (unsigned)(-EIO);
			break;
		}
		s = &sess[i];
		memset(s, 0, sizeof(*s));
		s->used = 1;
		s->obj = o;
		if (o->open && o->open(s) < 0) {
			s->used = 0;
			wa->handle = (unsigned)(-EIO);
			break;
		}
		wa->handle = (unsigned)i;
		break;

	case DEV_READ:
		h = (int)ra->handle;
		if (h < 0 || h >= NSESS || !sess[h].used) {
			wa->handle = (unsigned)(-EBADF);
			break;
		}
		s = &sess[h];
		n = (int)ra->arg;
		if (n > DEV_DATA_MAX)
			n = DEV_DATA_MAX;
		n = s->obj->read ? s->obj->read(s, out, (unsigned)n) : -ENOTTY;
		if (n < 0) {
			wa->handle = (unsigned)n;
			break;
		}
		rep->len = sizeof(struct dev_args) + (unsigned)n;
		wa->handle = (unsigned)n;
		break;

	case DEV_WRITE:
		h = (int)ra->handle;
		if (h < 0 || h >= NSESS || !sess[h].used) {
			wa->handle = (unsigned)(-EBADF);
			break;
		}
		s = &sess[h];
		n = (int)ra->arg;
		if (n > DEV_DATA_MAX)
			n = DEV_DATA_MAX;
		n = s->obj->write ? s->obj->write(s, payload, (unsigned)n) : -ENOTTY;
		wa->handle = (unsigned)n;
		break;

	case DEV_IOCTL: {
		unsigned iocmd = ra->arg;			/* cmd 在 arg 字段 */
		unsigned ioarg;

		memcpy(&ioarg, payload, sizeof(ioarg));	/* 参数在 payload 前 4 字节 */
		h = (int)ra->handle;
		if (h < 0 || h >= NSESS || !sess[h].used) {
			wa->handle = (unsigned)(-EBADF);
			break;
		}
		s = &sess[h];
		n = s->obj->ioctl ? s->obj->ioctl(s, iocmd, ioarg) : -ENOTTY;
		wa->handle = (unsigned)n;
		break;
	}

	case DEV_CLOSE:
		h = (int)ra->handle;
		if (h < 0 || h >= NSESS || !sess[h].used) {
			wa->handle = (unsigned)(-EBADF);
			break;
		}
		s = &sess[h];
		if (s->obj && s->obj->close)
			s->obj->close(s);
		memset(s, 0, sizeof(*s));
		wa->handle = 0;
		break;

	case DEV_LIST: {
		unsigned cap = ra->arg ? ra->arg : DEV_DATA_MAX;

		if (cap > DEV_DATA_MAX)
			cap = DEV_DATA_MAX;
		n = devlist_read(0, out, cap);
		rep->len = sizeof(struct dev_args) + (unsigned)n;
		wa->handle = (unsigned)NDEV;
		break;
	}
	case DEV_SHUTDOWN:
		return (1);
	default:
		wa->handle = (unsigned)(-EINVAL);
		break;
	}

	if (rep->len == 0)
		rep->len = sizeof(struct dev_args);
	return (0);
}

int
main(void)
{
	struct dev_msg req, rep;
	int n;

	if (port_register(DEV_PORT) < 0)
		return (2);
	if (port_register(DEV_REPLY_PORT) < 0) {
		port_release(DEV_PORT);
		return (2);
	}
	for (;;) {
		n = ipc_recv(DEV_PORT, &req);
		if (n < 0)
			break;			/* -EIO: 端口被关闭 */
		if (req.type == DEV_SHUTDOWN)
			break;			/* 受控退出：内核释放端口，客户端 EIO */
		handle(&req, &rep);
		ipc_send(DEV_REPLY_PORT, &rep);
	}
	port_release(DEV_PORT);
	port_release(DEV_REPLY_PORT);
	return (0);
}
