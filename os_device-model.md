# BSD45 统一设备模型微内核化（S401）

> 本轮把"设备管理"做成微内核：设备命名与分派从内核硬编码逻辑，收敛为
> **用户态 devfs_server 的统一设备对象注册表**，客户端经 IPC 端口按
> "名字 → 句柄 → 统一 ops 分派"访问。服务崩溃只让设备不可用、内核不 TRAP，
> 在飞请求返回 `-EIO`，服务可被重启。不使用服务时系统与原内核完全一致。

## 1. 为什么：从散射到统一

### 1.1 改造前的设备模型（内核态、不统一）

改造前，BSD45 的"设备管理"分散在内核三处，没有统一的设备抽象：

| 层 | 位置 | 形态 |
|---|---|---|
| 命名/打开 | `usermode.c:4276` `sys_open` | 对 `/dev/null /dev/zero /dev/tty /dev/mounts /dev/ps /dev/vmstat` **逐个 strcmp**，分配固定 `FD_NULL=6 .. FD_VMSTAT=10` |
| 读分派 | `usermode.c:4109+` `sys_read` | 按 `FD_*` 类型 switch，每种设备一段专用代码（null 返回 0、zero 填 0、ps/mounts/vmstat 渲染文本…） |
| 写分派 | `usermode.c:4078+` `sys_write` | 又一组 switch：null/zero 丢弃、console 走 `cnputc` |
| 控制 | `sys_ioctl` | 只特判 fd≤2 终端请求，其余 `ENOTTY` |

问题：**每加一个设备要改三处内核代码**，设备知识编译进内核，无法故障隔离，
驱动或服务出错直接 TRAP 整机。这是典型的宏内核设备模型。

### 1.2 改造后：统一设备模型（用户态服务）

```
 客户端 (devtest / 任意程序)
   │  dev_open("echo") / dev_read / dev_write / dev_ioctl / dev_list
   │  打包 struct dev_msg → ipc_send(DEV_PORT) → ipc_recv(DEV_REPLY_PORT) 真会合
   ▼
 ┌─────────────────────────────────────────────┐
 │ 用户态 devfs_server（端口 2 请求 / 3 回复）    │
 │                                             │
 │  统一注册表 struct devobj[]                   │
 │   { name, open, read, write, ioctl, close }   │
 │   ┌ "null"    → null_read / null_write ...  │
 │   ├ "zero"    → zero_read / zero_write ...  │
 │   ├ "echo"    → echo_open/read/write ...    │
 │   └ "devlist" → devlist_read ...            │
 │                                             │
 │  分派：find_obj(name) → 会话表句柄 → obj->ops │
 └─────────────────────────────────────────────┘
   ▼  服务退出 → 内核释放端口 → 阻塞客户端 -EIO
```

"统一"体现在三点：
1. **统一设备对象**：所有设备都是 `struct devobj{name, open, read, write, ioctl, close}`，
   面对同一组函数签名——不再每个设备一套 strcmp 分支。
2. **统一名字空间**：一张注册表，`dev_list` 可枚举；新增设备只加一行、不碰内核。
3. **统一分派**：`find_obj(name)` → 句柄（会话表索引）→ `obj->ops` 函数指针调用。

## 2. 实现内容

### 2.1 协议 `libc/include/dev.h`

- `struct dev_msg` 与内核 `struct ipc_msg` **逐字节对齐**（5×u32 + `char[64]` = 84 字节），
  避免跨内核/用户读写错位
- 请求端口 `DEV_PORT=2`（贴合内核 `PORT_BDEV`）、回复端口 `DEV_REPLY_PORT=3`；
  fs_server 占端口 0/1，两者不冲突
- `struct dev_args{handle, arg}` 编码在 `inline_data` 前 8 字节，数据区随后（≤56 字节）
- ops：`DEV_OPEN/READ/WRITE/IOCTL/CLOSE/LIST/SHUTDOWN`
- 客户端 API：`dev_open/read/write/ioctl/close/list`

### 2.2 用户态服务 `libexec/devfs_server.c`

- 统一设备对象注册表 + 会话表（句柄）：
  - `null`：写丢弃、读 0 字节
  - `zero`：读全 0、写丢弃
  - `echo`：有状态设备，写存缓冲、读回显（演示统一 ops 处理状态）
  - `devlist`：返回注册表名字清单（注册表自省，证明可枚举）
- `DEV_IOCTL` 命令 1 = 复位会话偏移（演示统一 ioctl op）
- 返回值经 `msg.handle` 传整数；用户态服务**不直接写客户端用户地址**
  （跨进程写需要内核 `pipe_copy`，本服务不需要）
- `DEV_SHUTDOWN` 受控退出：内核释放两个端口，阻塞在回复端口的客户端收到 `-EIO`

### 2.3 验收程序 `libexec/devtest.c`

内嵌 `dev_*` 客户端 API（send 请求 + 真会合 recv 回复），6 项验收：

1. fork/exec `/bin/devfs`，等服务注册端口
2. `DEV_LIST` 枚举注册表（名字空间）
3. `null/zero/echo` 按名字打开，读写走统一 ops 分派
4. `DEV_IOCTL` 复位偏移
5. `DEV_SHUTDOWN` 服务退出 → 后续请求返回 `-EIO`，内核无 TRAP（故障隔离）
6. 重启服务后恢复

## 3. 设计取舍与边界（如实说明）

1. **不是透明劫持内核 `sys_open/read`**。BSD45 的阻塞 syscall 通过"保存 regs
   + proc_sched"恢复用户态，handler 中途阻塞后后续代码被丢弃，无法在 sys_read
   内部阻塞再转发。采用 MINIX 式"服务经 IPC 端口暴露 + 用户态 `dev_*` API"，
   语义等价且**零回退**：内核原有 `/dev/*` strcmp 分派保持不动。
2. **真实硬件驱动未上移**。ATAPI/ATA 的 PIO 端口 I/O 与 IRQ 投递需要 IOPL /
   IRQ→用户态地基（当前缺），本轮上移的是**设备命名/分派模型**与纯软件伪设备
   （null/zero/echo），这是统一设备模型的骨架。硬件驱动逐个上移是后续阶段。
3. 数据 ≤56 字节内联；共享页（KP-7）大数据路径原语已具备，本轮未用。
4. 单客户端串行验收；多客户端并发需要 reply_port 按客户端区分。
5. `SIGTERM` 在该内核默认动作不终止进程，故障注入用 `DEV_SHUTDOWN` 受控退出
   模拟"服务崩溃"——确定性且同样验证"服务消失 → EIO → 重启"契约。
6. ISO `-iso-level 1` 的 8.3 短名限制：服务名 `DEVFS.ELF`、测试 `DEVTEST.ELF`，
   客户端用 `/bin/devfs` 访问（内核把 a-z 转大写、无点名追加 `.ELF`）。

## 4. 构建与验证

遵守原则：**Windows 用 PowerShell、Ubuntu 用 bash；Ubuntu 只构建不执行测试**。

```bash
# Ubuntu：仅构建（ELF 直链严格校验）
cd ~/bsd45-build && bash build_all.sh
```

```powershell
# Windows：i686 原生链路 + QEMU 执行
$env:PATH = "C:\Users\Administrator\.workbuddy\binaries\python\versions\3.13.12;" + $env:PATH
.\build_kernel.ps1; .\build_sh.ps1; .\build_iso.ps1
.\tools\run_qemu_test.ps1 -Keys "shell|/bin/devtest|exit 0" -WaitFor "user exit"
```

`devtest` 验收输出关键项：`dev: registry [...]` / `dev: uniform ops ok` /
`dev: fault isolation ok (EIO)` / `dev: restart ok` / `dev: PASS`，且无 kernel TRAP。

也可运行一键故障注入验收 `tools/test_device.sh`（检查上述关键项 + 负向断言"内核不 TRAP"）。

实际验证结果（2026-11-20，Ubuntu bash 构建 ELF 直链严格校验 + Windows i686 原生链路 + QEMU 执行）：
- `dev: registry [null,zero,echo,devlist] count=4` —— 统一注册表枚举成功（4 个设备）
- `dev: uniform ops ok` —— null 吞写读0、zero 读全0、echo 往返、ioctl rewind 从头重读，全部走统一 ops 分派
- `dev: fault isolation ok (EIO)` —— 服务受控退出后在飞请求返回 EIO，内核无 TRAP
- `dev: restart ok` —— 服务重启后按名字打开设备、写入被接受
- `dev: PASS`
- 回归：`srvtest`（S400）、`ipctest`、`all_tests` 与基线一致，零回退

说明：Windows GCC 16.2 对 `struct devsess` 上误写的函数指针成员（应为 `sess->obj->ops`）
未报错，Ubuntu ELF 直链严格校验暴露了 3 处此类笔误并已修正——再次证明双平台校验的价值。

## 5. 与微内核目标的关系

S400 落地了通用服务化骨架（IPC 会合 + 用户态 fs_server + 故障隔离）；S401 把
同一套机制应用到**设备管理**这一具体领域，证明统一设备模型可以在用户态实现，
设备知识不再编译进内核。剩余差距：`tty_server`、真实硬件驱动上移（需 IOPL/IRQ
地基）、init supervisor、共享页大数据闭环（详见 `os_microkernel.md`）。

## 6. 变更文件清单

**新增**
- `libc/include/dev.h`（统一设备模型协议）
- `libexec/devfs_server.c`（用户态统一设备服务）
- `libexec/devtest.c`（验收）
- `os_device-model.md`（本文档）

**修改**
- `build_sh.sh/.ps1`（编译 devfs_server/devtest、复制 dev.h）
- `build_iso.sh/.ps1`（打包 DEVFS.ELF / DEVTEST.ELF）
- `os_microkernel.md`（交叉引用）
