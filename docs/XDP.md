# r8125 的 XDP 与 AF_XDP

配套文档：[AF_XDP_ZC_DESIGN.md](AF_XDP_ZC_DESIGN.md)（设计与依据）、[../CHANGELOG.md](../CHANGELOG.md)（逐项改动）

适用版本：**9.018.00-xdp.1**（基于 Realtek r8125 9.018.00）

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
| `xsktest.c` | AF_XDP 测试：`rx`（逐帧校验 IP 和 UDP 校验和）、`l2fwd`（交换 MAC 后发回）、`tx`（按指定长度和端口发 UDP），可选拷贝模式、关闭 need_wakeup、先建 socket 后挂程序。建好 socket 后用 `XDP_OPTIONS` 读出实际协商到的模式；和要求的模式不一致（比如要零拷贝却退回了拷贝模式），或者有坏帧、有未完成的发送时，退出码为 1 |
| `test_xsk_pool_setup.py` | 主机回归测试，不需要网卡和 root 权限。从 `src/` 里抽出 `rtl8125_xsk_pool_setup()` 和 `rtl8125_rings_rebuild()`，把内核接口换成桩函数后编译运行，检查 pool 的归属、返回值和错误日志，共 31 项。`--revision <提交>` 可以拿旧版本对照。不能覆盖真实的 DMA、NAPI、内存分配失败和并发 |

```sh
clang -O2 -g -target bpf -c tools/xdp/r8125_test.bpf.c -o r8125_test.bpf.o
gcc -O2 -Wall -o xsktest tools/xdp/xsktest.c -lxdp -lbpf

xdp-loader load -m native -n xdp_tx enp1s0 r8125_test.bpf.o
./xsktest -i enp1s0 -m l2fwd -t 30            # 零拷贝，need_wakeup
./xsktest -i enp1s0 -m l2fwd -t 30 -c -w      # 拷贝模式，不用 need_wakeup
./xsktest -i enp1s0 -m tx -D <对端 MAC> -s <本端 IP> -d <对端 IP> -l 42 -p 319 -N 50000

python3 tools/xdp/test_xsk_pool_setup.py                                   # 主机回归
CFLAGS="-fsanitize=address,undefined -g" python3 tools/xdp/test_xsk_pool_setup.py
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

## 6b. 实测（NanoPi R5S，RTL8125B，arm64 RK3568，Armbian 6.18.62，32 个 MSI-X 向量 / ISR v2，RSS）

编译选项是 `ENABLE_RSS_SUPPORT=y ENABLE_MULTIPLE_TX_QUEUE=y`，得到 2 个 RX 队列（V3 描述符）和 2 个 TX 队列。对端是网线直连的 Windows I225-V，流量由 WSL 产生。UDP 测试给 lan1 配了 4 个地址，让不同的流按目的地址哈希到两个队列。

| 场景 | 结果 |
|---|---|
| 8 条 TCP 流，不挂程序 / 挂着程序 | 都是 2.36 Gbit/s，两个 RX 向量的中断数基本相同 |
| 8 条 TCP 流满速时，挂/卸程序 20 次、改 MTU 20 次 | 修复隐患 F 之前：第一次就停止收包，官方原版也一样。修复后：全部正常，dmesg 干净 |
| XDP_TX，4 个流共 10 万帧 | 收 88719、发 88720（多出的是 ARP）；两个 RX 队列和两个 TX 环都在用 |
| `bpf_redirect` 回本口（`ndo_xdp_xmit`） | 收 100001、发 100004 |
| 零拷贝 RX，绑在队列 1 / 队列 0 | 协商为零拷贝；按约 4 万 pps 发 10 万帧时，各收到属于自己队列的一半（50001 / 49999），坏帧为 0，`rx_missed` 不变 |
| 零拷贝 l2fwd，队列 1 / 队列 0 | 各 50000 收、50000 发、50000 完成；队列 1 的发送走 TX 环 1 |

突发发送（WSL 尽快发出 10 万帧）时，零拷贝队列和走协议栈的队列各丢约 1.5 万帧，记在 `rx_missed` 上（这个计数器是 16 位的，会回绕）。原因是用户态逐帧校验跟不上，填充环被取空；协议栈那一侧也处理不过来。同样的流量用 XDP_TX 回弹时一帧不丢，限速后零拷贝也一帧不丢，所以这不是驱动问题。

## 7. 尚未覆盖

- x86 平台上机（只做了编译）
- V4 描述符和 ISR v3 以上的中断布局（R5S 上开 RSS 用的是 V3 描述符、ISR v2）
- 超过 2 个 RX 队列的 RSS：驱动不支持 `ethtool -L`，R5S 是 4 核，默认只有 2 个队列
- 8125A、8125D 等其他芯片的短帧填充行为（设计文档 §11 R9）
- 更长时间（天级）的长跑
- 小帧 RX 的上限：对端发包速率不够，测不出来
