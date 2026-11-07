# r8125 的 XDP 与 AF_XDP

配套文档：[AF_XDP_ZC_DESIGN.md](AF_XDP_ZC_DESIGN.md)（设计与依据）

本驱动在 Realtek 官方 r8125-9.018.00 的基础上增加了原生 XDP 和 AF_XDP 零拷贝。新逻辑集中在 `src/r8125_xdp.c` 和 `src/r8125_xdp.h`；`r8125_n.c` 里只有单行调用点，关掉开关时全部是空函数，行为和官方驱动一致。

---

## 1. 功能

| 能力 | 说明 |
|---|---|
| XDP_DROP / XDP_PASS | 页复用路径上运行，PASS 保留程序移动过的帧头尾和 metadata |
| XDP_TX | 从收到帧的 CPU 对应的 TX 环发回，和协议栈共用 TX 环与队列锁 |
| XDP_REDIRECT | 到 devmap、cpumap、xskmap（拷贝模式）以及其他网卡 |
| `ndo_xdp_xmit` | 本口可以作为其他网卡 redirect 的目标（`xdp-forward` 用的就是它） |
| AF_XDP 零拷贝 RX / TX | `XDP_ZEROCOPY` 绑定，RX 缓冲区直接来自 UMEM，TX 直接从 UMEM 发出 |
| AF_XDP need_wakeup | 支持；不开 need_wakeup 时，驱动在缓冲区不足时持续轮询 |
| AF_XDP busy poll | 队列到 NAPI 的映射通过 `netif_queue_set_napi()` 登记（6.8+），6.13 之前同时写入 RX 队列信息 |

`ip -d link` 或 `ynl` 能看到的 `xdp-features`：`basic`、`redirect`、`ndo-xmit`、`xsk-zerocopy`。

## 2. 编译

`src/Makefile` 里 `ENABLE_XDP_SUPPORT = y` 是默认值。满足下面全部条件时才真正编进去，否则驱动按官方原样编译：

- 内核 ≥ 6.6
- `ENABLE_PAGE_REUSE = y`（默认）
- `ENABLE_RX_PACKET_FRAGMENT = n`、`ENABLE_LIB_SUPPORT = n`（默认）

要彻底关闭：`make ENABLE_XDP_SUPPORT=n`。

已对 x86 6.12、arm64 6.6 / 6.18 / 7.3 编译，开和关两种配置、以及 RSS / PTP / 多 TX 队列打开的组合都没有新增警告。

## 3. 准入规则

| 规则 | 原因 |
|---|---|
| 挂程序时，满长帧加 256 字节 headroom 和 `skb_shared_info` 要放得进一页（4K 页时 MTU 约 3490 以下） | 每帧一个缓冲区，第一版不做多缓冲 |
| 挂着程序时把 MTU 改大，超过上一条就拒绝 | 同上 |
| 零拷贝队列号必须小于 RX 和 TX 环数（默认配置下只有队列 0） | 和 igc / igb / fec 一致 |
| XSK pool 的帧大小（chunk 减去 headroom）要放得下满长帧；绑定后改 MTU 也会检查 | 描述符长度用的是全局 `rx_buf_sz` |
| RX 缓冲区没有对齐要求，unaligned pool 可以用 | 第 0 步实测 |

## 4. 使用

```sh
# 原生 XDP
xdp-loader load -m native enp1s0 prog.o
xdp-bench drop|pass|tx enp1s0 -m native
xdp-bench redirect-cpu enp1s0 -c 1 -m native
xdp-bench redirect-map enp1s0 enp2s0 -m native

# 软路由：两个口之间的 XDP 转发
xdp-forward load enp1s0 enp2s0
```

AF_XDP 零拷贝照常用 `XDP_ZEROCOPY` 绑定。先挂程序还是先建 socket 都可以：pool 先登记下来，程序和 pool 都在时才切到零拷贝。

## 5. 测试工具（`tools/xdp/`）

| 文件 | 作用 |
|---|---|
| `r8125_test.bpf.c` | `xdp_drop` / `xdp_tx` / `xdp_redirect` / `xdp_xsk` 四个程序，只处理发往 UDP 9、319、320 的 IPv4 帧，其余一律 PASS，所以 ARP、SSH 等不受影响 |
| `xsktest.c` | AF_XDP 测试：`rx`（逐帧校验 IP 和 UDP 校验和）、`l2fwd`（交换 MAC 后发回）、`tx`（按指定长度和端口发 UDP），可选拷贝模式、关闭 need_wakeup、先建 socket 后挂程序 |

```sh
clang -O2 -g -target bpf -c tools/xdp/r8125_test.bpf.c -o r8125_test.bpf.o
gcc -O2 -Wall -o xsktest tools/xdp/xsktest.c -lxdp -lbpf

xdp-loader load -m native -n xdp_tx enp1s0 r8125_test.bpf.o
./xsktest -i enp1s0 -m l2fwd -t 30            # 零拷贝，need_wakeup
./xsktest -i enp1s0 -m l2fwd -t 30 -c -w      # 拷贝模式，不用 need_wakeup
./xsktest -i enp1s0 -m tx -D <对端 MAC> -s <本端 IP> -d <对端 IP> -l 42 -p 319 -N 50000
```

**注意：** 测短帧填充时必须网线直连。有些交换机会按 IPv4 总长度截掉帧尾再补零，抓到的长度和填充都不可信（见设计文档 §10.1）。

## 6. 实测（Cubie A7Z，RTL8125B / CFG_5，arm64，PCIe DMA 非一致，vendor 6.6.98，单中断向量）

对端：网线直连 Windows 上的 Intel I225-V，`pktmon` 抓包。

| 场景 | 结果 |
|---|---|
| 挂/卸程序 3 次，同时 TCP 流入 | 连接不断，7.3 GiB 逐字节正确 |
| XDP_DROP 90 万帧 | 空闲页不变 |
| cpumap redirect TCP 流 | 约 115 kpps，8.3 GiB 正确 |
| XDP_TX、`bpf_redirect` 回本口（`ndo_xdp_xmit`） | 各 3 轮 × 10 万帧，全部回到对端，空闲页不变 |
| XDP_TX 短帧 | 42 字节帧线上 60 字节补零；发往 319 / 320 的短帧补到 81 字节（8125A/B 勘误） |
| 零拷贝 RX | 20 万帧，校验和全部正确 |
| 零拷贝 l2fwd | 20 万帧全部回到对端；先建 socket / 先挂程序 × need_wakeup 开 / 关，拷贝模式，全部 10 万 / 10 万 |
| 零拷贝 TX | 60、42、1514 字节以及发往 319 的 42 字节帧，长度、填充、校验和全部正确 |
| 零拷贝环上的 PASS | 同时接收 UDP 到 socket 和 TCP 到协议栈：10 万帧正确，TCP 11.0 GiB 正确，接近 2.5G 线速 |
| 零拷贝运行中：重新协商链路、MTU 1700、MTU 1800（被拒绝）、改环大小 | 转发 191.8 万帧，无错误，退出后空闲页不变 |
| socket 绑着时卸载程序、换一个程序、再卸载 | 无异常 |
| V3 描述符（`ENABLE_PTP_SUPPORT=y` 时启用，32 字节，写回时地址字段被覆盖） | TCP 流量中触发 5 次 reset 和 3 次 ESD 恢复（只在测试版本里加的触发器），17.0 GiB 正确；空闲时各 5 次和 3 次，空闲页不变；XDP_TX、`ndo_xdp_xmit`、零拷贝 RX/TX 全部通过；零拷贝 l2fwd 期间并发 3 次 reset 和 2 次 ESD，191 万帧无错 |
| 发送速率（60 / 1514 字节） | 零拷贝 41 万 / 20.3 万 pps，拷贝 34 万 / 20.3 万 pps；1514 字节就是 2.5G 线速 |

**调试内核**（Armbian 镜像重新打包，内核开 DMA_API_DEBUG、PROVE_LOCKING、DEBUG_ATOMIC_SLEEP、KFENCE）下又跑了一遍：TCP 流量中挂/卸程序、重新协商、改 MTU；XDP_TX / redirect / drop；零拷贝 RX、各种 l2fwd 组合、拷贝模式、各长度 TX；零拷贝运行中重新协商、改 MTU（1800 被拒绝）、改环大小。全部结束后，DMA API 错误计数没有增加，`debug_locks` 仍为 1，内核日志里没有 WARNING / BUG / lockdep / KFENCE 报告。调试内核让 CPU 变慢，XDP_TX 时会有 `rx_missed`，所有收到的帧都正确发回。缺陷 E（DMA 映射错误检查被兼容宏编译掉）就是在这个内核上发现的。

**KASAN 内核**（同一套镜像再开 KASAN generic，由 Armbian 构建流程打包；先前手工编译的 KASAN 内核启动不了，原因在手工构建，不在 KASAN）下把上面四轮又跑了一遍：TCP 流量中挂/卸程序、重新协商、改 MTU，6.6 GiB 正确；XDP_TX / redirect / drop；零拷贝和拷贝模式的全部组合与各长度 TX；零拷贝运行中重新协商、改 MTU、改环大小。KASAN、KFENCE、DMA API 和 WARNING 计数始终等于加载驱动之前的基线（基线里的一个 KASAN 报告和两个 WARNING 来自厂商内核启动时的其他驱动）。

**两小时压测（KASAN 内核）。** 零拷贝 l2fwd、拷贝模式 l2fwd、原生 XDP_TX 轮流各跑 9 分钟，段与段之间各做一次重新协商和一次 MTU 往返，对端持续灌 UDP：零拷贝收发 1.568 亿帧，拷贝模式 5104 万帧，坏帧都是 0、发送全部完成；XDP_TX 发出 6154 万帧。结束时 KASAN、KFENCE、DMA API、WARNING 计数与开始时完全相同，空闲页相差 788 页（约 3 MB，在测试之间的正常波动范围内）。其中两段没有流量，是对端 Windows 在第一次断链后暂停发送所致，之后的同类段落都正常。

**已知的硬件 / 官方行为：close / open 之后的 64 字节描述符。** 接口 close 再 open（`ip link set down/up`、`ethtool -G`）之后，偶尔会有一个描述符报告收到 64 字节（去掉 FCS 是 60），没有任何错误位，但缓冲区并没有被这次写入：内容是缓冲区原来的数据。在 l2fwd 测试里，这一帧的 MAC 地址恰好是上一轮已经交换过的，由此可以确认。8 次 close / open 大约出现 2~5 个；重新协商（`ethtool -r`）在 1600 万帧里一次也没有出现。用只计数的 XDP 程序以 generic 模式挂在**未修改的官方驱动**上，同样每 8 次出现 2~5 个，所以这不是本驱动引入的。协议栈会按 IP 长度丢掉这种帧（`IpInTruncatedPkts`）；XDP 程序和 AF_XDP 应用应当像对待任何外来数据一样校验长度。

A7Z 上 60 字节帧的发送上限约 39 万~41 万 pps，与零拷贝还是拷贝、定时器中断开或关都无关，此时 CPU 有 96% 空闲，瓶颈在这块板子的 PCIe Gen2 x1 链路对小帧的处理。x86 上的小帧上限需要另外测。

## 7. 尚未覆盖

- x86 平台上机（只做了编译）
- 开 RSS 后的多队列（V4 描述符、多队列零拷贝）：A7Z 只有一个中断向量，驱动会自动关闭 RSS。V3 描述符已经通过开启 PTP 覆盖
- 8125A、8125D 等其他芯片的短帧填充行为（设计文档 §11 R9）
- 更长时间（天级）的长跑
- 小帧 RX 的上限：对端发包速率不够，测不出来
