# r8125：带 XDP 与 AF_XDP 零拷贝的 Realtek 2.5G 驱动

基于 Realtek 官方 Linux 驱动 **r8125-9.018.00**，在尽量少改动官方代码的前提下：

- 增加了原生 XDP、`ndo_xdp_xmit` 和 AF_XDP 零拷贝收发；
- 修复了官方驱动的五个缺陷。

关掉 `ENABLE_XDP_SUPPORT` 时，除了缺陷修复，驱动行为和官方完全一致。

官方原版说明见 [`README`](README)，逐项改动见 [`CHANGELOG.md`](CHANGELOG.md)。

---

## 版本

| | |
|---|---|
| 当前版本 | **9.018.00-xdp.1**（git 标签 `v9.018.00-xdp.1`） |
| 基于 | Realtek r8125 **9.018.00**，原样保存在 `vendor` 分支（标签 `vendor-9.018.00`） |
| 模块版本字符串 | 仍是官方的 `9.018.00-NAPI-DASH`（`ethtool -i` 显示），没有改动 |
| 内核 | 驱动本身沿用官方的兼容范围；XDP / AF_XDP 需要 **Linux 6.6 及以上**，低于 6.6 时自动不编入 |
| 状态 | 功能完整。在 RTL8125B + arm64 上做过完整测试，包括 DMA_API_DEBUG、lockdep、KFENCE、KASAN 和两小时压测；其他芯片和平台的情况见下文“支持状态” |

---

## 相对官方 9.018.00 的改进

### 修复的官方缺陷

| | 问题 | 后果 | 怎么发现的 |
|---|---|---|---|
| A | 页复用路径漏了 `skb_reserve(rx_offset)` | `NET_IP_ALIGN = 2` 的架构（riscv、arm32 等）上收到的帧整体错位 2 字节 | 读代码；用实验参数在 arm64 上验证 |
| B | `reset_task` 只把描述符原样交还硬件 | 开 RSS（V3/V4 描述符）时，硬件可能 DMA 到由 RSS 信息构成的地址 | 读代码 |
| C | link down / ESD 重建 RX 环时没有停 NAPI | 与正在运行的 poll 并发释放页 | 读代码 |
| D | 页复用模式下丢包会打破 `dirty_rx` 不变量 | 每次丢包后泄漏一页及其 DMA 映射，实测每万次丢包约 5000 页 | 实现 XDP_DROP 时发现，上机复现 |
| E | 兼容宏 `#define dma_mapping_error(a,b) 0` 在 2.6.27 以后的内核上也生效 | 全部 DMA 映射错误检查被编译掉，映射失败时把 `DMA_MAPPING_ERROR` 当地址交给硬件 | DMA_API_DEBUG 内核 |

修复 C 时还顺带修正了官方的一处问题：`change_mtu` 重建失败后 NAPI 处于关闭状态，之后执行 close 会卡死。现在不会了。

### 新功能

| 功能 | 说明 |
|---|---|
| 原生 XDP | DROP / PASS / TX / REDIRECT（devmap、cpumap、xskmap、其他网卡）；PASS 保留程序移动过的帧头尾和 metadata |
| `ndo_xdp_xmit` | 本口可以作为其他网卡 redirect 的目标，`xdp-forward` 可以直接用来做软路由转发 |
| AF_XDP 零拷贝 | RX 和 TX 都支持；支持 need_wakeup；挂程序和建 socket 的先后顺序都可以；不开 need_wakeup 时，缓冲区不足会持续轮询 |
| busy poll | 通过 `netif_queue_set_napi()`（6.8+）登记队列到 NAPI 的映射，`ynl` / netlink 能查到；6.13 之前同时写入 RX 队列信息 |
| 8125A/B 的 PTP 勘误 | XDP 和 XSK 发出的短 UDP PTP 帧会按官方规则补齐，否则会被硬件悄悄丢弃 |

### 实现上的改进

- **TX 统一释放**：官方三处重复的 TX 释放代码合成一个函数 `rtl8125_tx_release()`，按描述符的类型分派（skb / XDP 帧 / XSK 帧）。
- **重建流程统一**：所有重建环的操作都走统一的函数。改 MTU、挂/卸程序、绑 XSK pool 用 `rtl8125_rings_rebuild()`；link down 和 ESD 用 `rtl8125_reset_rings()`。
- **RX 同步优化**：页复用路径只把实际收到的字节同步给 CPU（原来同步整个缓冲区），并提前预取帧头。这对 DMA 非一致的平台有利。
- **代码隔离**：XDP 相关代码全部放在 `src/r8125_xdp.c` 和 `src/r8125_xdp.h`。`r8125_n.c` 里只加了单行调用点，关掉开关时这些调用都是空的内联函数。

---

## 编译与安装

```sh
./autorun.sh                       # 官方脚本：编译、安装、加载
# 或者只编译
make -C /lib/modules/$(uname -r)/build M=$PWD/src modules
sudo insmod src/r8125.ko
```

满足以下条件时，`ENABLE_XDP_SUPPORT` 会自动编入；否则驱动按官方原样编译，不会编译失败：

- 内核 ≥ 6.6；
- `ENABLE_PAGE_REUSE = y`（默认）；
- 没有打开 `ENABLE_RX_PACKET_FRAGMENT` / `ENABLE_LIB_SUPPORT`（默认都不打开）。

如果要显式关闭，用 `make ... ENABLE_XDP_SUPPORT=n`。

确认 XDP 已编入：

```sh
nm -u src/r8125.ko | grep -c xdp_do_redirect   # 1 表示已编入
ip -d link show enp1s0                         # 挂上程序后能看到 prog/xdp
```

---

## 使用

```sh
# 挂一个原生 XDP 程序
xdp-loader load -m native enp1s0 prog.o
xdp-loader unload enp1s0 --all

# 快速试验
xdp-bench drop enp1s0 -m native
xdp-bench tx enp1s0 -m native
xdp-bench redirect-cpu enp1s0 -c 1 -m native

# 软路由：两个口之间直接转发
xdp-forward load enp1s0 enp2s0
```

AF_XDP 零拷贝照常以 `XDP_ZEROCOPY` 绑定队列 0，挂程序和建 socket 的先后顺序都可以。仓库里带了测试工具：

```sh
clang -O2 -g -target bpf -c tools/xdp/r8125_test.bpf.c -o r8125_test.bpf.o
gcc -O2 -Wall -o xsktest tools/xdp/xsktest.c -lxdp -lbpf
./xsktest -i enp1s0 -m l2fwd -t 30            # 零拷贝转发，逐帧校验
./xsktest -i enp1s0 -m l2fwd -c -w            # 拷贝模式，不用 need_wakeup
```

**准入规则：**
- 挂 XDP 程序时，满长帧加上 256 字节 headroom 必须放得进一页。4K 页时 MTU 上限约 3490。
- 零拷贝只用队列 0；XSK pool 的帧大小必须放得下满长帧。
- 挂着程序或绑着 pool 的时候，把 MTU 改大到超出上限会被拒绝。

细节见 [`docs/XDP.md`](docs/XDP.md)。

---

## 支持状态

| 芯片 / 平台 | 状态 |
|---|---|
| RTL8125B（CFG_5），arm64，PCIe DMA 非一致，单中断向量 | **完整实测**：V1 和 V3 描述符；reset 和 ESD；XDP 全部判决；零拷贝收发及其生命周期；调试内核和 KASAN；两小时压测 |
| RTL8125A（CFG 2~3） | 代码路径与 8125B 相同（同样适用 PTP 勘误），未上机。需要确认硬件会自动补零短帧 |
| RTL8125D 等不在 CFG 2~7 的芯片 | 不需要勘误填充，未上机。需要确认硬件会自动补零短帧 |
| x86 | 编译通过（6.12），未上机 |
| 多中断向量（ISR v2~v7）、开 RSS 的多队列 | 代码按官方中断布局实现，未上机。A7Z 只有一个中断向量，驱动会自动关闭 RSS |

在新芯片或新平台上验证的方法：在网线直连的对端抓包，用 `xsktest -m tx -l 42` 看线上是不是 60 字节、填充是不是全零；再跑一遍 `docs/XDP.md` 里的测试，大约半小时。

---

## 已知问题

- **close / open 之后的 60 字节帧（硬件 / 官方行为）。** 接口 close 再 open 之后，偶尔会有一个描述符报告收到 60 字节、没有任何错误位，缓冲区里却是旧数据。未修改的官方驱动同样会出现。
  - 协议栈会按 IP 长度把这种帧丢掉；
  - XDP 程序和 AF_XDP 应用应当校验长度和内容。
- **小帧发送速率受平台限制。** A7Z 上 60 字节帧最多约 41 万 pps，与零拷贝还是拷贝、中断方式都无关，瓶颈在这块板子的 PCIe 链路。1514 字节帧能跑满 2.5G 线速。
- **暂不支持多缓冲（jumbo）XDP。** 挂程序时 MTU 受上面的页大小限制。

---

## 测试概况

在 Radxa Cubie A7Z（RTL8125B）上测试，对端是一台用网线直连的 Windows 主机（Intel I225-V）。

- **编译**：x86 6.12；arm64 6.6、6.18、7.3。XDP 开和关两种配置、RSS / PTP / 多 TX 队列组合，都没有新增警告；`W=1` 和 sparse 下也干净。
- **功能**：XDP 各判决；零拷贝和拷贝模式的全部组合；运行中重新协商链路、改 MTU、改环大小；V3 描述符下的 reset 和 ESD 恢复。
- **调试内核**：DMA_API_DEBUG、lockdep、KFENCE、DEBUG_ATOMIC_SLEEP 下，r8125 没有引起任何报告。
- **KASAN**：KASAN generic 内核下全套测试通过；两小时混合压测处理约 2.7 亿帧，没有任何报告，内存没有增长。

完整数据见 [`docs/XDP.md`](docs/XDP.md)。

---

## 分支

| 分支 | 内容 |
|---|---|
| `main` | 缺陷修复、重构、XDP / AF_XDP、文档和测试工具。每个提交都能单独编译 |
| `vendor` | Realtek 9.018.00 原样导入，只用于将来 rebase 新版本 |
| `exp/phase0` | 开发前的硬件实验（RX 对齐、短帧填充），不进补丁序列 |

Realtek 发布新版本时：更新 `vendor` 分支，再把 `main` rebase 上去。缺陷 A~E 的修复提交可以单独整理后反馈给 Realtek。

## 文档

| 文件 | 内容 |
|---|---|
| [`CHANGELOG.md`](CHANGELOG.md) | 版本和逐项改动 |
| [`docs/XDP.md`](docs/XDP.md) | 用法、准入规则、测试工具、全部实测数据、尚未覆盖的项目 |
| [`docs/AF_XDP_ZC_DESIGN.md`](docs/AF_XDP_ZC_DESIGN.md) | 设计推导、缺陷分析、内核兼容、调用点清单、补丁序列 |
| [`docs/TEST_R5S.md`](docs/TEST_R5S.md) | NanoPi R5S 测试计划 |
| [`CLAUDE.md`](CLAUDE.md) | 给修改代码的人：结构、不变量、构建检查、测试方法、提交规范 |

## 许可证

GPL-2.0-only，与 Realtek 官方驱动相同。
