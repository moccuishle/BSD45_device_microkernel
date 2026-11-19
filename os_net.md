# BSD45 用户态网络协议栈（S403）+ VFS 透明转发 + 共享页闭环（S402）

> 本轮完成三件事：① **VFS 透明转发层**——`open("/srv/x")` 等普通 syscall 经内核转发到用户态 fs_server；② **共享页大数据闭环**——KP-7 原语封装为"分配→填充→提交→映射→校验→释放"全链路；③ **用户态网络协议栈**——UDP/IP + 环回虚拟网卡运行在用户态 netsrv，故障隔离、可重启。默认不用这些能力时系统与原内核一致（零回退）。

## 1. VFS 透明转发层（kernel/fs 路径，S402）

### 1.1 问题与目标

S400 的文件服务通过用户态 `srv_*` API 访问——用户程序要显式调用新 API。本轮目标是**透明转发**：用户程序用标准 `open("/srv/x")/read/write/close` 即可，内核 VFS 层把 `/srv/` 前缀请求转成 IPC 发给 fs_server。

### 1.2 核心难点与解法

最初尝试在**内核 syscall handler 内**扩展续传协议（`srv_forward` + `srv_deliver_waiter`）阻塞等待 fs_server 回复。但 BSD45 的 `resume_user` 路径不经过 `cpu_switch`、**不维护 `curpcb`**——内核态嵌套阻塞后调用 `copyin` 会因 `curpcb==NULL` 触发 stack fault（TRAP 12），且单线程 block 的 TCB 状态处理易死锁。该方向在实测中反复 TRAP/死锁，风险不可控，已回退。

**最终方案：libc 层透明转发**（对用户完全透明、零内核改动、零 TRAP 风险）：
- `libc/src/libc_sys.c` 的 `open/read/write/close` 检测 `/srv/` 前缀或虚拟 fd
- `open("/srv/x")`：去掉前缀 → 经 IPC 调 fs_server `SRV_OPEN` → 登记虚拟 fd（`0x4000` 基址，不与内核 fd 冲突）→ 标准 fd 返回
- `read/write/close`：虚拟 fd 走 `srv_fd_read/write/close`（复用 S400 已验证的真会合 IPC）
- 错误经 `syscall_rv` 转成标准 `-1 + errno`
- 完全复用 S400 的成熟 IPC 机制；不使用服务时 libc 行为与原样一致

### 1.3 故障隔离

- 服务退出后内核释放回复端口，libc 的 `srv_call` 收到 `-EIO`，`open` 返回 `-1/errno=EIO`
- 请求/回复端口沿用 fs_server 的 0/1；数据 ≤56 字节内联（大包走共享页，见 §2）

## 2. 共享页大数据闭环（KP-7，S402）

### 2.1 问题

IPC 消息内联区只有 84 字节（数据 ≤56 字节），大文件/大包无法承载。

### 2.2 闭环设计

`libc/include/shmbuf.h`（static inline，工具单文件即可用）封装 KP-7 原语：
```
shm_alloc()  → page_alloc + map_shared 到 SHM_VA，返回可写指针
   ↓ 填充数据（整页 4KB）
shm_commit() → 解除映射，数据留在物理页；pa 放进 IPC 消息的 pa 字段发送
   ↓ 接收方
shm_map(pa)  → 映射收到的 pa 到 SHM_VA，读取数据
shm_unmap()  → 读完解除映射
shm_free()   → page_free 释放物理页（ACK 最后一环，由分配方调用）
```
无需跨进程引用计数；服务崩溃时内核在 `free_address_space` 统一回收映射页。

`shmtest.c` 验证整页 4KB 数据往返 + 释放后再分配（无泄漏）。这是 VFS 大数据读写与 net 大包传输的基础原语。

## 3. 用户态网络协议栈（netsrv，S403）

### 3.1 架构：MINIX 式服务化

完整 4.5BSD TCP/IP 栈上移需要网卡驱动 + IOPL + IRQ 投递地基（当前缺，QEMU 无网卡配置）。本轮实现**协议栈本体完全在用户态**的可运行骨架：

```
 用户程序 (nettest)  ── net_* API over IPC (端口4/5) ──▶ netsrv (用户态)
                                                          ├ 协议分派表 proto_dispatch[]
                                                          │   (UDP 已实现 / TCP 预留)
                                                          ├ socket 表 socks[]
                                                          │   (端口绑定 + RX 数据报队列)
                                                          └ 环回虚拟网卡 loopback
                                                              (本机端口→目标 socket 队列)
 内核：仅 IPC 端口，无协议栈代码
```

"协议栈在用户态"的实体是 `proto_dispatch[]`——用户态的协议开关表（对应内核 cdevsw 的用户态等价物），协议知识在服务进程里，不在内核。新增协议只需加表项。

### 3.2 能力

- `NET_SOCKET/BIND/CONNECT/SEND/RECV/CLOSE/STAT/SHUTDOWN` 操作
- UDP 数据报收发：发送到本机绑定端口经环回网卡直接投递到目标 socket 的 RX 队列
- 端口唯一性检查（`EADDRINUSE`）、socket 表满（`EIO`）、未实现协议（TCP→`EINVAL`）
- `NET_STAT` 统计：socket 数、发包数、收包数、环回投递数
- 故障隔离：SHUTDOWN → 内核释放端口 → 客户端 `-EIO` → 可重启

### 3.3 边界（如实说明）

- 物理网卡驱动未上移：发往非本机端口的包计入已发送计数后丢弃（无物理 NIC）。真正接入物理网络需要 IOPL 端口 I/O + IRQ→用户态投递 + 网卡驱动服务，是后续阶段
- 当前为无连接 UDP 语义；TCP（NET_SOCK_STREAM）在分派表预留但未实现
- 数据 ≤56 字节内联；大包可走 §2 共享页（原语已具备）
- 单客户端串行；多客户端并发需要按客户端区分 reply_port

## 4. 设计取舍

1. **透明转发与零回退**：`/srv/` 是新增前缀，内核原有 `/dev/*`、`tmpfs`、`ISO` 路径完全不动；不使用服务时行为与原内核一致。
2. **libc 层而非内核 handler 内阻塞**：见 §1.2，BSD45 的 `resume_user` 不维护
   `curpcb`，内核态嵌套阻塞会栈错误，故透明转发放在 libc 层——对用户仍完全透明，
   且内核零改动、零回退。
3. **环回优先**：用户态 net 先做可完整验证的环回路径，证明协议栈服务化与故障隔离；物理网卡上移列为后续。

## 5. 构建与验证

遵守原则：**Windows 用 PowerShell、Ubuntu 用 bash；Ubuntu 只构建不执行测试**。

```bash
# Ubuntu：仅构建（ELF 直链严格校验）
cd ~/bsd45-build && bash build_all.sh
```

```powershell
# Windows：i686 原生链路 + QEMU 执行
$env:PATH = "C:\Users\Administrator\.workbuddy\binaries\python\versions\3.13.12;" + $env:PATH
.\build_kernel.ps1; .\build_sh.ps1; .\build_iso.ps1
.\tools\run_qemu_test.ps1 -Keys "shell|/bin/shmtest|exit 0" -WaitFor "user exit"
.\tools\run_qemu_test.ps1 -Keys "shell|/bin/vfstest|exit 0" -WaitFor "user exit"
.\tools\run_qemu_test.ps1 -Keys "shell|/bin/nettest|exit 0" -WaitFor "user exit"
```

一键验收脚本 `tools/test_net.sh` 驱动 vfstest/shmtest/nettest 并断言"内核不 TRAP"。

实际验证结果（2026-11-20，Ubuntu bash ELF 直链校验 + Windows i686 原生链路 + QEMU）：
- `vfstest: PASS` —— 标准 `open("/srv/vfstest")` 返回虚拟 fd 0x4000、write 11 字节、
  read 回 "transparent"、服务退出后 `open` 返回 -1/errno=EIO、重启恢复；无 kernel TRAP
- `nettest: PASS` —— 用户态 netsrv 的 UDP 环回收发、统计（sent=1/recv=1/loop=1/socks=2）、
  故障隔离 EIO、重启恢复；无 kernel TRAP
- `shmtest: PASS` —— 4KB 共享页往返校验 + 释放后再分配（无泄漏）
- 回归：`srvtest: PASS`、`devtest: PASS`、`ipctest: PASS` —— libc 改动零回退

调试要点（已解决，如实记录）：
1. 内核 syscall 内嵌套阻塞转发曾导致 TRAP 12（stack fault）——`resume_user` 路径不维护
   `curpcb`，内核态 `copyin` 写 `curpcb->pcb_onfault` 时空指针。故改为 libc 层转发，
   内核零改动，此问题消失。
2. fs_server 的 CLOSE 语义是删除文件（tmpfs close=unlink），vfstest 改为同 fd write/read
   回环验证；故障隔离断言按 libc 语义用 `-1 + errno==EIO`。
3. libc 增大（+4KB）使 devtest 子服务 fork/exec 变慢，首次等待 3M→8M 避免时序误判。

## 6. 变更文件清单

**新增**
- `libc/include/shmbuf.h`、`libc/include/net.h`
- `libexec/shmtest.c`、`libexec/vfstest.c`、`libexec/netsrv.c`、`libexec/nettest.c`
- `os_net.md`（本文档）

**修改**
- `libc/src/libc_sys.c`（open/read/write/close 的 /srv/ 透明转发 + srv IPC 客户端）
- `build_sh.sh/.ps1`、`build_iso.sh/.ps1`（编译打包新程序与新头文件）
- `os_microkernel.md`（交叉引用）

**内核零改动**：VFS 透明转发、共享页闭环、用户态 net 全部在用户态/libc 实现，
不改变内核 syscall 路径——这是本轮刻意的设计取舍（见 §1.2、§4）。
