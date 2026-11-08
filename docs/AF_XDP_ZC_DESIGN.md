# r8125 AF_XDP 零拷贝设计方案

| 项 | 内容 |
|---|---|
| 基线代码 | Realtek 官方 r8125-9.018.00（官网下载，未经修改） |
| 目标 | 原生 XDP（DROP / PASS / TX / REDIRECT）+ `ndo_xdp_xmit` + AF_XDP 零拷贝（RX + TX） |
| 内核范围 | **6.6 ~ 7.x**（已对 v6.6 / 6.8 / 6.9 / 6.10 / 6.12 / 6.13 / 6.14 / 6.18 / 7.3 头文件逐一核对） |
| 已定决策 | ① 支持到 6.6 LTS；② 官方驱动隐患 A / B / C 全部修复 |
| 主要场景 | 软路由（RTL8125 x86 小主机；NanoPi R5S / R5C、Radxa E52C 等 Rockchip arm64 板） |
| 文档状态 | **已实现，版本 9.018.00-xdp.1**（2026-11-07）：补丁序列全部提交，在 Cubie A7Z（RTL8125B）上验证，包括调试内核、KASAN 和两小时压测。用法与实测见 [XDP.md](XDP.md)，改动清单见 [../CHANGELOG.md](../CHANGELOG.md)。正文保留设计推导；实现时的改动在各节以“实现”标出，汇总见 §9 |

> **范围说明（2026-11 调研后补充）：** 对软路由来说，真正有用的终点是**补丁 8**（原生 XDP + `ndo_xdp_xmit`，配合 xdp-tools 的 `xdp-forward`）。补丁 9~10 的零拷贝只服务于 AF_XDP 用户态程序，包括 DPDK `net/af_xdp`，列为可选。DPDK 原生 `net/r8169` 在 Rockchip 平台上不可用（§2.4），因此 DPDK 在这些板子上唯一可行的路径就是"本方案 + `net/af_xdp`"。

---

## 目录

1. [结论](#1-结论)
2. [现状核实](#2-现状核实)
3. [官方驱动隐患与修复](#3-官方驱动隐患与修复)
4. [内核兼容策略（6.6+）](#4-内核兼容策略66)
5. [总体设计](#5-总体设计)
6. [数据结构](#6-数据结构)
7. [关键流程](#7-关键流程)
8. [约束与准入规则](#8-约束与准入规则)
9. [补丁序列](#9-补丁序列)
10. [第 0 步与测试计划](#10-第-0-步与测试计划)
11. [风险与待确认项](#11-风险与待确认项)
12. [参考资料](#12-参考资料)

---

## 1. 结论

- **可行。** 驱动当前完全没有 XDP 支持（没有 `ndo_bpf`、`xdp_rxq_info`、page_pool），所以分两层做：先做原生 XDP，再在它之上做 XSK 零拷贝。
- **对官方代码的改动很小。** 新逻辑全部放在 `r8125_xdp.c` / `r8125_xdp.h`，由 `ENABLE_XDP_SUPPORT` 开关控制。`r8125_n.c` 只增加约 17 处单行调用点（§5.3），外加 4 个隐患修复（实现时多发现了隐患 D）和 3 个只重构不改行为的补丁。
- **没有上游先例。** 上游 r8169 至今没有 XDP（v7.3 中搜 xdp/xsk 都是 0 处），2021 年的社区补丁也没被接收。也没找到任何 r8125 的 AF_XDP 实现。
- **性能预期。** 2.5G 小包线速约 3.72 Mpps（2.5e9 ÷ (84 × 8)）。零拷贝主要降低 CPU 占用和延迟抖动，不太可能让吞吐翻倍。做到第 4 个补丁（原生 XDP + copy 模式 AF_XDP）时先实测，再决定是否继续投入零拷贝。

---

## 2. 现状核实

> 行号对应 `src/r8125_n.c` / `src/r8125.h`（9.018.00 版本）。

### 2.1 硬件与驱动结构

| 项 | 结论 | 依据 |
|---|---|---|
| RX 队列数 | 硬件最多 **8** 个（`R8125_MAX_RX_QUEUES = 16` 只是数组大小）。默认关闭 RSS 时为 1 个 | `r8125_n.c:15674`、`15768` |
| TX 队列数 | 默认 1 个。只有同时满足 `ENABLE_MULTIPLE_TX_QUEUE`、ISR ≥ 2、ISR v2 时 ≥ 19 个向量，才会用到 2 个 | `r8125_n.c:15737-15746` |
| 单队列停启 | **不支持**。只找到全局的 `ChipCmd.CmdRxEnb`，官方复位一律整机复位 | `r8125.h:1607`、`r8125_n.c:19172` |
| ISR 版本与 NAPI | v1 单向量（`rtl8125_poll`）；v2/v5/v7 的 RX 在 0~15、TX 单独（v2 用 16/18、v5 用 16/17、v7 用 27/28）；v3/v4 的 RX 与 TX 合用向量（`poll_msix_ring`） | `r8125_n.c:17966-18008`、`20900-20935` |
| 8125CP（CFG_12） | 要求至少 49 个向量，但上限是 32，所以**永远退回 ISR v1** | `r8125.h:700-702` |
| 芯片对应 ISR | CFG 4/5/7 → v2；8/9 → v4；10/11/13~17 → v5；12 → v7；其他 → v1 | `r8125_n.c:15704-15728` |
| RX 描述符格式 | V1（16B）、V3（32B，RSS 或 PTP 时启用）、V4（16B，RSS 时启用）。**V3/V4 写回时 `addr` 会被 RSS 信息覆盖**（两者是 union），交还硬件前必须重写 `addr` | `r8125.h:2039-2115`、`r8125_n.c:21418` |
| 描述符写地址 | `rtl8125_map_to_asic(tp, ring, desc, dma, len, idx)` 和缓冲区来源无关，会同步维护影子数组 `RxDescPhyAddr[]` | `r8125_n.c:19293` |
| RX 长度字段 | V1 为 14 位，V3/V4 为 15 位（看 `RsvdMask`） | `r8125.h:2025-2027` |
| TX 描述符 | 32 字节；opts1 包含 Own / RingEnd / FirstFrag / LastFrag / len | `r8125.h:2029`、`1852-1855` |
| TX 完成 | TxNoClose 模式（默认）读硬件的 closed pointer 计数器，天然适合批量 `xsk_tx_completed(n)` | `r8125_n.c:20759` |
| 短包填充 | 官方对**所有芯片**设 `UseSwPaddingShortPkt = TRUE`，但实测硬件会自己补零到 60 字节（§2.5）。8125A/B（CFG 2~7）的 PTP/UDP 勘误实测存在，必须保留。上游 r8169 实现了同样的逻辑 | `r8125_n.c:15469`、`20322-20392`；r8169 `rtl_quirk_packet_padto`；§10.1 |
| 填充缓冲常量 | `SHORT_PACKET_PADDING_BUF_SIZE = 256` 已定义但未使用，可以直接用 | `r8125.h:750` |
| `ring_info` | 本身有 `__pad[sizeof(void*) - 4]`，加类型字段不会让结构变大 | `r8125.h:2134-2140` |
| lib ring | Makefile 引用了 `r8125_lib.c`，但源码包中**没有这个文件**，必须和 XDP 互斥 | `ls src/` |
| 回环 | 不支持 `NETIF_F_LOOPBACK`，`xskxceiver` 需要两个口或环回线 | grep 结果为 0 |
| 已处理的最高内核版本 | 源码中最高判断到 `KERNEL_VERSION(6,13,0)` | grep |

### 2.2 RX 缓冲地址对齐：**任意字节对齐都可以（已实测）**

- 旧版 r8169 中 8168 家族写明 `.align = 8`（v2.6.30），v4.4 为 16 字节对齐。8168KB 就在 r8125 的芯片表里。设计初稿因此按 8 字节对齐保守设计。
- 官方 r8125 在 `NET_IP_ALIGN = 2` 的架构上（riscv / loongarch / mips / arm32）确实会把"页地址 + 2"写进描述符，但这条路径本身有隐患 A，不能当作证据。
- **实测（§10.1）：** RTL8125B 上把缓冲区起点依次放在页内偏移 0、1、2、4、8、16、32、64、258 字节处，全部通过逐字节比对，协议栈和网卡错误计数都没有变化。偏移 1 也能通过，说明 DMA 引擎直接使用描述符里的字节地址，没有向下对齐。
- **结论：** RX 缓冲地址没有对齐要求。§8 不设对齐规则，unaligned pool 可以接受，headroom 取任何值都可以。

### 2.3 参考驱动的关键事实

| 驱动 | 可借鉴的点 |
|---|---|
| **igb**（`igb_xsk.c`，单文件约 560 行） | 结构最像我们：XSK TX 在 TX 清理的 NAPI 中、持 `__netif_tx_lock` 调用 `igb_xmit_zc`，并和协议栈共用 TX 环；批量 `xsk_buff_alloc_batch` 补 buffer |
| **fec** | **同样没有单队列硬件停启**，pool 设置走"先建好新环再替换 + `fec_restart` 整机复位"；用 `napi_if_scheduled_mark_missed` 唤醒 |
| **igc** | XDP_TX / XSK 和协议栈共用 TX 环；pool 准入要求 `frame_size >= ETH_FRAME_LEN + 2*VLAN_HLEN`；没挂程序时只记录 pool，等挂程序时才生效 |
| xsk 核心 | **先** `xsk_reg_pool_at_qid` **再**调用 `ndo_bpf(SETUP_XSK_POOL)`；关闭时**先** `ndo_bpf(NULL)` **再**清除注册，且关闭**不允许失败**（失败会 WARN） |
| xsk 核心 | `xsk_buff_free` 把 buffer 放回 pool 的 `free_list`，`xsk_buff_alloc_batch` **优先**从 free_list 取（6.6 和 7.3 都一样）。所以只要 NAPI 已停，重建环时把 buffer 全部归还再取回，不依赖 fill 环 |
| **DPDK `net/r8169`**（Realtek 官方，BSD-3） | 硬件知识可以对照参考，代码不能搬（许可证不兼容）。它独立印证了 8125A/B 的 PTP/UDP 勘误（DPDK 的 `CFG_METHOD_48~53` 对应本驱动的 CFG 2~7）；没有单队列启停；RTL8125 上只开放 1 收 1 发 |

### 2.4 平台差异：DMA 一致性

依据：主线 v7.3 设备树。

| 平台 | PCIe DMA 一致性 | PCIe 的 IOMMU | 对本方案的影响 |
|---|---|---|---|
| x86 | 一致 | 有 | 无 |
| RK3568（R5S / R5C） | **不一致**（没有 `dma-coherent`） | 没有 | 所有 `dma_sync_*` 都会真正执行缓存维护，漏掉一处就会出错，**很适合当正确性测试机** |
| RK3582 / RK3588（E52C） | **不一致** | 有（`mmu600_pcie`） | 同上 |
| SC8280XP（Dragon Q8B，外接 RTL8125 时） | 一致 | 有 | 无 |

- 本方案全部走内核 DMA API，在非一致平台上是正确的：
  - 官方页复用路径显式调用 `dma_sync_single_range_for_cpu/device`；
  - XSK pool 在非一致平台上会自动打开 `dma_need_sync`；
  - `XDP_TX` 的 `dma_map_single(TO_DEVICE)` 会把缓存刷回内存。
- 反过来，DPDK 原生 PMD 不做缓存维护，所以在 Rockchip 上不可用。Realtek 2022 年在 ARM SoC 上移植 RTL8125 PMD 时出现的 RDU 卡死，就是这个原因。

### 2.5 短包填充：硬件自己补零，PTP 勘误真实存在（已实测）

- 内核厂商驱动对所有芯片都设置了 `UseSwPaddingShortPkt = TRUE`，上游 r8169 也对 8125 强制填充到 60 字节。
- DPDK `net/r8169`（同样出自 Realtek）对非 UDP 的短包在任何芯片上都不做软件填充，只在 PTP 勘误路径里顺带补到 60 字节（`r8169_rxtx.c:1549`、`:1614`）。
- **实测（§10.1，RTL8125B / CFG_5）：**
  1. 关掉全部软件填充后，42 和 59 字节的 UDP 帧在线上都是 60 字节，填充全是零，IP / UDP 校验和正确。**硬件会自己补零。**
  2. 关掉勘误填充后，发往 UDP 319 / 320 的短帧**一帧都到不了对端**。驱动的发送计数照常增加，对端网卡也没有任何接收错误计数，帧根本没有出现在线上。
  3. 只保留通用填充（补到 60 字节）也不行，结果和第 2 条一样。勘误看的是 L4 长度：UDP 头加负载必须达到 `MIN_PATCH_LEN = 47` 字节（IPv4 下整帧 81 字节），只补到 60 字节不够。
- **结论：** 普通短帧不需要软件填充；8125A/B 的 PTP 勘误填充必须保留，而且必须按官方的 `rtl8125_get_patch_pad_len` 计算长度。§7.4 据此简化。
- **注意：** 只测了 CFG_5 一种芯片。其他 `mcfg` 是否同样补零，由 §10.2 的短包用例逐一确认（§11 R9）。

---

## 3. 官方驱动隐患与修复

> 都是读代码推出来的，没有上机复现。按已定决策**全部修复**，每个隐患单独一个补丁，放在补丁序列最前面，方便同时反馈给 Realtek。

### 隐患 A：`NET_IP_ALIGN = 2` 的架构上 RX 数据错位 2 字节

- **位置：** `rtl8125_rx_interrupt` 页复用分支（约 `r8125_n.c:21331-21337`）。
- **原因：** `build_skb(rxb->data + page_offset - rx_offset, ...)` 让 skb head 指向 buffer 开头，而 DMA 写入位置是 `buffer + rx_offset`。内核 ≥ 3.19 且开了 NAPI 时 `R8125_USE_NAPI_ALLOC_SKB = 1`，于是 `if (!R8125_USE_NAPI_ALLOC_SKB) skb_reserve(skb, R8125_RX_ALIGN)` 被跳过，`skb->data` 比包头早 `rx_offset` 字节。这个条件本来是给 `napi_alloc_skb` 路径用的（它内部已经 reserve 了 NET_IP_ALIGN），被误用到了 `build_skb` 路径上。
- **影响：** 只影响 `NET_IP_ALIGN = 2` 的架构。x86 / arm64 上 `rx_offset = 0`，不受影响。
- **已上机验证：** 第 0 步在 arm64 上用模块参数把 `rx_offset` 设成非零值，模拟了这些架构的情况。带着这个修复，1~258 的所有非零偏移都收发正确（§10.1）。
- **修复：** 页复用分支里无条件 `skb_reserve(skb, ring->rx_offset)`。非页复用（skb）分支保持原样。

### 隐患 B：开 RSS（V3/V4 描述符）时，`reset_task` 可能让硬件 DMA 到垃圾地址

- **位置：** `rtl8125_reset_task`（约 `r8125_n.c:19927-19942`）。
- **原因：** 复位时把索引清零（`rtl8125_init_ring_indexes`），然后对**全部**描述符只做 `mark_to_asic`。这有两个问题：
  1. V3/V4 下，已写回但还没处理的描述符，`addr` 位置是 RSS 信息，没有用 `RxDescPhyAddr[]` 恢复；
  2. 页复用模式下，补 buffer 失败的槽位（`rxb->page == NULL`）也被交还硬件，而它的 `RxDescPhyAddr` 已经失效。索引清零又让 cur/dirty 的语义和实际 buffer 占用对不上。
- **修复：** 这里 NAPI 已经由 `_rtl8125_wait_for_quiescence` 停掉。把"重置索引 + 只重新标记描述符"换成正规的 `rtl8125_rx_clear()` + `rtl8125_init_ring()`（其中已包含索引重置和 TX 描述符初始化）。`init_ring` 失败时打印错误并重新 `schedule_reset_work`。

### 隐患 C：link down / ESD 路径重建 RX 环时没有停 NAPI

- **位置：** `rtl8125_link_down_patch`（约 `r8125_n.c:5262-5279`），以及 ESD 检查路径（约 `r8125_n.c:17420-17428`）。
- **原因：** 在 workqueue 里（持 rtnl）执行 `hw_reset → tx_clear → rx_clear → init_ring`。`hw_reset` 只屏蔽中断，没有 `napi_disable`，另一个 CPU 上正在跑的 poll 可能和 `rx_clear` 释放页并发。
- **已核实前提：** 所有调用者进入时 NAPI 都处于开启状态：open（`enable_napi` 在 18592，调用在 18606）、change_mtu（19210 → 19216）、reset_task（19951 → 19955）、linkchg/ESD 任务运行期间。所以可以安全地"先停后开"，不会嵌套 disable 导致卡死。
- **修复：** 提取一个统一的函数，替换这两处：

```c
/* 调用前提：持 rtnl，NAPI 处于开启状态 */
static void rtl8125_reset_rings(struct net_device *dev)
{
        struct rtl8125_private *tp = netdev_priv(dev);

        _rtl8125_wait_for_quiescence(dev);  /* napi_disable + 屏蔽中断 + synchronize_irq */
        rtl8125_hw_reset(dev);
        rtl8125_tx_clear(tp);
        rtl8125_rx_clear(tp);
        if (rtl8125_init_ring(dev) < 0)
                netdev_err(dev, "Failed to refill the RX rings\n");
        rtl8125_enable_napi(tp);
}
```

- **补充修复（实现时发现）：** 官方 `change_mtu` 在 `init_ring` 失败时直接返回，NAPI 保持关闭，但设备仍在运行。之后再来一次 link down 就会对已关闭的 NAPI 再次 `napi_disable` 而永久阻塞；官方代码里 close 也会卡在同一处。补丁 3 让 `change_mtu` 无论重建成功与否都先重新打开 NAPI。

- **对 XDP 的意义：** 这是 ZC 正确性的基础。修复后，link 抖动时 ZC 环可以放心地把 buffer 全部还给 pool 再取回（§2.3 最后一行），不需要专门的"保留 buffer"逻辑。

### 隐患 D：页复用模式下，丢弃的帧会让下一个正常帧覆盖一个仍在使用的缓冲区（实现时发现，已上机复现）

- **位置：** `rtl8125_rx_interrupt` 的 `release_descriptor` 路径，配合 `rtl8125_put_rx_buffer`。
- **原因：** 页复用的环有一个不变量：`dirty_rx` 到 `cur_rx` 之间的槽位都是空的。`put_rx_buffer` 依赖它，直接把另一半页交给 `dirty_rx` 处的槽位，不检查那里有没有 buffer。丢包路径（描述符错误、超长帧、skb 分配失败）却把描述符原地交还硬件，`cur_rx` 前进而 `dirty_rx` 不动。下一个正常帧于是覆盖了那个槽位上仍然映射着、交给了硬件的页：页和 DMA 映射泄漏，描述符在硬件持有时被改写。
- **复现：** 让每隔一个描述符走一次丢包路径，修复前每 1 万次丢包空闲页减少约 5000 页，修复后不变。
- **修复：** 丢包改走新的 `rtl8125_recycle_rx_buffer()`，把同一个 buffer 原样交给 `dirty_rx` 处的槽位，和正常路径一样维持不变量。XDP_DROP 也走它。

### 隐患 E：兼容宏把全部 DMA 映射错误检查编译掉了（调试内核发现）

- **位置：** `r8125.h` 的 `#ifndef dma_mapping_error / #define dma_mapping_error(a,b) 0`。
- **原因：** 2.6.27 起 `dma_mapping_error()` 是函数而不是宏，`#ifndef` 恒为真，于是驱动里每一处映射检查都变成常量 0：RX 页分配、协议栈发送、XDP 发送。映射失败（IOMMU 或 swiotlb 耗尽）时，`DMA_MAPPING_ERROR` 会被当作地址交给硬件。
- **发现：** 开 `CONFIG_DMA_API_DEBUG` 的内核上，模块只引用了 `debug_dma_map_single` 而没有 `debug_dma_mapping_error`，每次 unmap 都报 "failed to check map error"。
- **修复：** 兼容宏只在 2.6.27 之前生效。修复后全套测试不再产生任何 DMA API 错误；未修改的官方驱动仍然会在每次 unmap 时报告。

---

## 4. 内核兼容策略（6.6+）

### 4.1 API 版本矩阵（逐个版本核对头文件）

| API | 6.6 | 6.8 | 6.10 | 6.14 | 7.3 | 处理方式 |
|---|---|---|---|---|---|---|
| `xdp_features` / `NETDEV_XDP_ACT_XSK_ZEROCOPY` / `xdp_features_set_redirect_target` | ✅ | ✅ | ✅ | ✅ | ✅ | 直接用（6.3 起） |
| `xsk_buff_alloc_batch(pool, xdp, max)` | ✅ | ✅ | ✅ | ✅ | ✅ | 直接用 |
| `xsk_tx_peek_release_desc_batch(pool, max)` + `pool->tx_descs` | ✅ | ✅ | ✅ | ✅ | ✅ | 直接用（签名不变） |
| `xsk_buff_set_size` / `xsk_buff_raw_get_dma` / `xsk_buff_raw_dma_sync_for_device` / `xsk_pool_dma_map` / `xsk_pool_get_rx_frame_size` | ✅ | ✅ | ✅ | ✅ | ✅ | 直接用 |
| `xdp_rxq_info_reg(rxq, dev, qid, napi_id)` / `_reg_mem_model` | ✅ | ✅ | ✅ | ✅ | ✅ | 直接用 |
| `xdp_init_buff` / `xdp_prepare_buff` / `xdp_convert_buff_to_frame` / `xdp_do_flush()` | ✅ | ✅ | ✅ | ✅ | ✅ | 直接用 |
| `xsk_buff_dma_sync_for_cpu` | `(xdp, pool)` | `(xdp, pool)` | **`(xdp)`** | `(xdp)` | `(xdp)` | **兼容宏 ①**（6.9 是双参数，6.10 改为单参数） |
| `xdp_build_skb_from_zc()` | ❌ | ❌ | ❌ | ✅ | ✅ | **不用**，自己实现拷贝版（见 4.3） |
| `xsk_buff_add_frag` | void | void | void | void | bool | 不用（第一版不做多缓冲） |
| `netif_queue_set_napi` | ❌ | ✅ | ✅ | ✅ | ✅ | 不用（可选优化，后续再说） |
| `xsk_tx_metadata_request` | ❌ | ✅ | ✅ | ✅ | ✅ | 不用（第一版不做 TX 元数据） |

### 4.2 兼容头：只有一个宏

```c
/* r8125_xdp.h */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,6,0)
#error "ENABLE_XDP_SUPPORT requires Linux >= 6.6"
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6,10,0)
#define rtl8125_xsk_sync_for_cpu(xdp, pool)  xsk_buff_dma_sync_for_cpu(xdp, pool)
#else
#define rtl8125_xsk_sync_for_cpu(xdp, pool)  xsk_buff_dma_sync_for_cpu(xdp)
#endif
```

Makefile 默认 `ENABLE_XDP_SUPPORT = n`，老内核用户完全不受影响。

### 4.3 为什么不用 `xdp_build_skb_from_zc`

- 6.6 上没有这个函数；而且它依赖 `CONFIG_PAGE_POOL`（没开时直接返回 NULL）。
- 它成功时会顺带 `xsk_buff_free` 并调用 `eth_type_trans`，和官方 RX 路径"统一在后面调用 `eth_type_trans`"的写法冲突。
- 所以全版本统一用自己的拷贝实现（igc 的 `igc_construct_skb_zc` 写法）：`napi_alloc_skb` → `memcpy(data_meta..data_end)` → `skb_metadata_set` → 由调用者 `xsk_buff_free`。一份实现，没有版本分支，也不依赖 `CONFIG_PAGE_POOL`。

---

## 5. 总体设计

### 5.1 设计原则

1. **新代码集中在新文件。** 新逻辑全部放进 `r8125_xdp.c` / `r8125_xdp.h`，和 RSS、PTP 的拆法一样。
2. **调用点用空函数，不用 `#ifdef`。** `r8125_n.c` 里只写单行调用；关掉开关时由 `r8125_xdp.h` 提供空的 `static inline` 实现。这沿用了官方 `rtl8125_lib_reset_prepare` 的写法（`r8125.h:3036`）。唯一的 `#ifdef` 在 `net_device_ops` 初始化里。
3. **不发明新的 buffer 生命周期。** XDP 的三种判决分别映射到官方已有的三条路径（§7.2）。
4. **所有重配置走同一个函数。** MTU 修改、挂/卸程序、XSK pool 设置，都调用 `rtl8125_rings_rebuild()`。
5. **准入条件在设置时一次检查完。** 数据路径里不做运行时能力判断。
6. **官方 `start_xmit` 一行不改。** TX 类型枚举里让 `SKB = 0`，官方原有的 `last->skb = skb` 和 `memset` 自动就是正确的。

### 5.2 文件布局

```
src/
├── r8125_xdp.h      兼容宏、空函数、ring/tp 新字段的访问函数、调用点声明
├── r8125_xdp.c      全部 XDP / XSK 逻辑
├── r8125_n.c        约 15 处单行调用点 + 隐患修复 + 两个重构
├── r8125.h          ring/tp/ring_info 新增字段（放在 ENABLE_XDP_SUPPORT 下）
└── Makefile         ENABLE_XDP_SUPPORT 开关 + r8125_xdp.o
```

`r8125_xdp.c` 内部：

```
rtl8125_xdp_bpf()              ndo_bpf：XDP_SETUP_PROG / XDP_SETUP_XSK_POOL
rtl8125_xdp_xmit()             ndo_xdp_xmit
rtl8125_xsk_wakeup()           ndo_xsk_wakeup
rtl8125_q_napi()               ISR 版本 → NAPI 映射（唯一知道 ISR 差异的函数）
rtl8125_run_xdp()              页路径执行程序，返回判决
rtl8125_xdp_finalize()         xdp_do_flush + XDP_TX 门铃
rtl8125_rx_zc()                ZC RX 主循环
rtl8125_xsk_ring_fill()        ZC 补 buffer（允许只补一部分）
rtl8125_xsk_ring_clear()       ZC 归还 buffer
rtl8125_xsk_construct_skb()    ZC 的 PASS（拷贝）
rtl8125_xsk_tx()               ZC TX（从 TX 清理路径调用）
rtl8125_xdp_tx_frame()         XDP_TX / ndo_xdp_xmit 共用的写描述符函数
rtl8125_xdp_padto()            短包和 PTP 勘误的填充长度（线性 buffer 版本）
rtl8125_xdp_ring_setup()       每个环的 xdp_rxq 内存模型切换
rtl8125_xdp_init/exit()        填充 buffer 的分配和释放、xdp_features
```

### 5.3 `r8125_n.c` 调用点清单

| # | 位置 | 改动（实现后的形态） |
|---|---|---|
| H1 | `net_device_ops` | `.ndo_bpf`、`.ndo_xdp_xmit`、`.ndo_xsk_wakeup`（唯一的 `#ifdef`） |
| H2 | 页复用分支开头 | ZC 环：`skb = rtl8125_xsk_rx(...)`；返回 NULL 计数后 `continue`，否则做完 RSS hash / 校验和后 `goto rx_skb_ready` 汇入官方后半段。**不另写收包循环**（§7.3） |
| H3 | 页复用分支，sync 之后、`build_skb` 之前 | `if (rtl8125_xdp_rx(tp, ring, cur_rx, rxb, &frame, &xdp_status)) { 计数; continue; }`；PASS 时 `skb_reserve(frame.offset)`、`skb_put(frame.len)`、`rtl8125_xdp_skb_meta()` |
| H4 | 收包循环结束后 | `rtl8125_xdp_finalize(tp, xdp_status);` |
| H5 | `rtl8125_init_ring` | 填充前 `rtl8125_xdp_ring_setup(tp, ring)`；ZC 环允许只填一部分，差额记进 `dirty_rx` |
| H6 | `_rtl8125_rx_clear`、`rtl8125_rx_fill`、`rtl8125_recycle_rx_buffer` 开头 | ZC 环分别转到 `rtl8125_xsk_rx_clear` / `rtl8125_xsk_rx_fill` / `rtl8125_xsk_rx_free` |
| H7 | `init_ring` | `ring->rx_offset = tp->rx_headroom` |
| H8 | `rtl8125_set_rxbufsize` | `tp->rx_headroom = rtl8125_xdp_headroom(tp)`；`tp->rms = rtl8125_mtu_to_rms(mtu)` |
| H9 | `rtl8125_rx_page_order` | 参数从 `page_size`（没用到）改为 `headroom` |
| H10 | TX 完成两个循环 + `tx_clear_range` | 合并成 `rtl8125_tx_release()`；非 skb 类型交给 `rtl8125_xdp_tx_release()`；循环后 `rtl8125_xdp_tx_done()` |
| H11 | `rtl8125_tx_interrupt` 包装函数 | 清理完之后 `rtl8125_xsk_xmit(ring)`，所有 ISR 版本只需这一处 |
| H12 | `rtl8125_change_mtu` | 函数体换成 `rtl8125_rings_rebuild()`，前面 `rtl8125_xdp_check_mtu()` |
| H13 | `rtl8125_alloc_rx_desc` / `rtl8125_free_rx_desc` | `rtl8125_xdp_rxq_reg/unreg`，和描述符环同生共死（open 出错路径也就自动对称） |
| H14 | probe / remove | `rtl8125_xdp_init/exit(tp)`：8125A/B 的零填充 buffer 和 `xdp_features` |
| H15 | 隐患 A / B / C / D 的修复点 | 见 §3 |
| H16 | `rtl8125_enable_napi` | `rtl8125_xdp_napi_enabled(tp)`：登记队列到 NAPI 的映射（§7.8） |
| H17 | 收包函数返回前 | ZC 环不开 need_wakeup 又没补满时返回 budget，让 NAPI 继续轮询 |

---

## 6. 数据结构

### 6.1 `rtl8125_rx_buffer`：用 union，不另开平行数组

```c
struct rtl8125_rx_buffer {
        union {
                struct page *page;      /* 页路径 */
                struct xdp_buff *xdp;   /* ZC 路径 */
        };
        u32 page_offset;
        dma_addr_t dma;
        void *data;
        struct sk_buff *skb;
};
```

- priv 里已经有 `rx_buffer[16][1024]`（约 650KB），不能再加一个平行数组。
- 官方代码里用到 `rxb->page` 的只有 rx_fill / put / free / clear 这几个函数，ZC 环在 H5 / H6 处被分流，永远不会走到它们。

### 6.2 `rtl8125_rx_ring` 新增

```c
#ifdef ENABLE_XDP_SUPPORT
        struct xsk_buff_pool *xsk_pool;   /* 已配置的 pool（由 SETUP_XSK_POOL 写入） */
        bool zc;                          /* 实际生效 = xsk_pool && tp->xdp_prog，在 init_ring 时计算 */
        struct xdp_rxq_info xdp_rxq;
#endif
```

"已配置"和"生效"分开存：pool 可以先绑定、后挂程序（igc 的语义），反过来也行。是否走 ZC 只在环重建时计算一次。

### 6.3 `rtl8125_tx_ring` 新增

```c
#ifdef ENABLE_XDP_SUPPORT
        struct xsk_buff_pool *xsk_pool;   /* 只在 tx_ring[0..num_tx_rings) 上，和 rx_ring[qid] 一一对应 */
#endif
```

### 6.4 `ring_info`：利用已有的 pad，结构体不变大

```c
enum rtl8125_tx_type {
        RTL_TX_SKB = 0,   /* 必须为 0：官方 start_xmit / memset 不用改 */
        RTL_TX_XDP,       /* XDP_TX 或 ndo_xdp_xmit：完成时 unmap + xdp_return_frame */
        RTL_TX_XSK,       /* 零拷贝：只计数，最后统一 xsk_tx_completed */
};

struct ring_info {
        union {
                struct sk_buff   *skb;
                struct xdp_frame *xdpf;
        };
        u32 len;              /* 非 0 表示这个描述符需要 unmap（沿用官方语义） */
        unsigned int bytecount;
        unsigned short gso_segs;
        u8 type;              /* 放进原来的 __pad */
};
```

沿用官方约定：**`len` 记在每个需要 unmap 的描述符上，完成信息（skb / xdpf / type / bytecount）记在一帧的最后一个描述符上。**
- XSK 和填充描述符的 `len = 0`，所以不会被 unmap。
- 填充描述符的 ptr 为 NULL、type 为 0，释放时什么都不做。

### 6.5 `rtl8125_private` 新增

```c
#ifdef ENABLE_XDP_SUPPORT
        struct bpf_prog *xdp_prog;
        u16 rx_headroom;            /* 无程序时 R8125_RX_ALIGN；有程序时 XDP_PACKET_HEADROOM + R8125_RX_ALIGN */
        void *tx_pad_buf;           /* SHORT_PACKET_PADDING_BUF_SIZE 字节全零，dma_alloc_coherent */
        dma_addr_t tx_pad_dma;
#endif
```

---

## 7. 关键流程

### 7.1 统一的重配置函数 `rtl8125_rings_rebuild()`

从 `rtl8125_change_mtu` 的函数体里提取出来（行为不变，补丁 4 已完成）：

```c
/* 持 rtnl。在 down 和 init_ring 之间调用 mutate() 改配置 */
int rtl8125_rings_rebuild(struct net_device *dev,
                          void (*mutate)(struct rtl8125_private *, void *), void *arg)
{
        struct rtl8125_private *tp = netdev_priv(dev);
        int ret = 0;

        if (!netif_running(dev)) {
                if (mutate)
                        mutate(tp, arg);
                return 0;
        }

        rtl8125_down(dev);                 /* 关 NAPI + 复位 + 清空 TX/RX */
        if (mutate)
                mutate(tp, arg);           /* 切 prog / 切 pool / 改 mtu */
        rtl8125_set_rxbufsize(tp, dev);    /* 重新算 headroom 和页 order */
        ret = rtl8125_init_ring(dev);      /* ZC 环在这里重新计算 ring->zc */
        rtl8125_enable_napi(tp);           /* 失败也要打开：设备仍在运行（见隐患 C 的补充修复） */
        if (ret < 0)
                return ret;
        if (tp->link_ok(dev))
                rtl8125_link_on_patch(dev);
        else
                rtl8125_link_down_patch(dev);
        return 0;
}
```

使用者有三个：`change_mtu`、`XDP_SETUP_PROG`（有无程序发生切换时）、`XDP_SETUP_XSK_POOL`（设备运行中且已挂程序时）。

### 7.2 页路径的 XDP：直接复用官方的"页翻转"模型（`MEM_TYPE_PAGE_SHARED`）

挂载程序后 `rx_headroom = XDP_PACKET_HEADROOM + R8125_RX_ALIGN`。按 H9 的计算，页会变成 order-1，每半页 4K。

```c
hard_start = rxb->data + rxb->page_offset - ring->rx_offset;
xdp_init_buff(&xdp, tp->rx_buf_page_size / 2, &ring->xdp_rxq);
xdp_prepare_buff(&xdp, hard_start, ring->rx_offset, pkt_size, false);
/* 运行程序之前必须先做 dma_sync_single_range_for_cpu（官方原来是在 build_skb 之后才 sync，这里要提前） */
```

| 判决 | 走官方哪条路径 | 引用计数 |
|---|---|---|
| `DROP` / `ABORTED` / 无效值 | `goto release_descriptor`：同一块 buffer、同一个地址原地重新交给硬件（官方处理错误包的路径） | 不变 |
| `PASS` | 原有 `build_skb` 流程，reserve 用 `xdp.data - hard_start`、len 用 `xdp.data_end - xdp.data`（顺带覆盖了隐患 A） | 原样 |
| `TX` / `REDIRECT` | `rtl8125_put_rx_buffer()`：翻转到另一半页，引用计数 +1。效果等同于 PASS 时 skb 持有一个引用 | 帧持有一个引用 |

- **实现：先翻转，再交出。** TX / REDIRECT 必须在交出帧**之前**调用 `rtl8125_put_rx_buffer()`（和官方 PASS 路径一致）：redirect 到拷贝模式的 XSK 时，内核会在 `xdp_do_redirect()` 返回之前就释放帧的页引用；如果先交出再加引用，这一页会在仍处于映射、仍在环里时被释放。交出失败时由驱动把这个引用还回去。
- **实现：DROP 走 `rtl8125_recycle_rx_buffer()`**，不再"原地交还"（§3 隐患 D）。
- **XDP_TX 必须重新映射。** 用 `dma_map_single(xdpf->data, TO_DEVICE)` 新建映射（igb 也是这样），**不能**复用 RX 的映射：官方在复用失败时会 `dma_unmap_page`，而 TX 可能还在读这块内存。
- **页的释放。** `dev_alloc_pages` 带 `__GFP_COMP`，所以 `xdp_return_frame` 内部的 `page_frag_free` 能正确找到复合页的头页。
- **以后可以换成 page_pool**，作为独立补丁，不和 ZC 绑在一起。

### 7.3 ZC 的 RX

**补 buffer（实现名 `rtl8125_xsk_rx_fill`）**
- 用 `xsk_buff_alloc_batch` 批量申请，处理 ring 回绕。
- 每个 buffer 都经导出的 `rtl8125_rx_arm()`（包装 `rtl8125_map_to_asic`）交给硬件。**每次都重写 `addr`**，自然避开了 V3/V4 的覆盖问题。
- **允许只补到一部分。** 用户态常常先 bind、后填 fill 环。补不满时不报错：
  - init_ring 视为成功，缺的数量记进 `dirty_rx`（`dirty_rx -= num_rx_desc - filled`），poll 里自然补上；
  - 没有 buffer 的描述符 `DescOwn = 0`，硬件会停在这里，等补满后自然继续。
- 重建时（§3 隐患 C 已修复，NAPI 已停）先 clear 再 fill：被 `xsk_buff_free` 归还的 buffer 进入 free_list，`alloc_batch` 优先取回它们，**不依赖 fill 环**。

**实现：不另写收包循环。** 设计初稿是独立的 `rtl8125_rx_zc`，但它要用到的描述符解析、错误检查、校验和、VLAN 函数在官方代码里都是热路径上的 `static inline`，全部导出会让官方页路径失去内联。实现改为同一个收包循环服务两种环：
1. 描述符的读取、`DescOwn` / `recheck_desc_ownbit`、`rmb()`、错误检查、`pkt_size`、分片帧丢弃全部沿用官方代码。
2. 只有"缓冲区怎么处理"一步分叉：ZC 环调用 `rtl8125_xsk_rx()`——`xsk_buff_set_size` + `rtl8125_xsk_sync_for_cpu` 后运行程序：
   - `REDIRECT`：`xdp_do_redirect`，失败则 `xsk_buff_free`
   - `TX`：`xdp_convert_buff_to_frame` 拷贝成独立的页（原 buffer 随即归还）后走 §7.4
   - `PASS`：`rtl8125_xsk_construct_skb` 拷贝成 skb 返回给循环，循环做完校验和（和 RSS hash）后 `goto rx_skb_ready` 汇入官方的 `eth_type_trans` / VLAN / `rtl8125_rx_skb`
   - `DROP` / 其他：`xsk_buff_free`
3. 无论哪种判决，槽位都留空；出错帧经 `rtl8125_recycle_rx_buffer()` 开头的分流直接还给 pool。于是"`dirty_rx` 到 `cur_rx` 之间都是空槽"在两种环上都成立，循环末尾的补充（`rtl8125_rx_fill` 开头分流到 `rtl8125_xsk_rx_fill`）不用改。
4. 打开了 need_wakeup 时：补不满就 set，否则 clear；不打开 need_wakeup 时，补不满就返回 budget 让 NAPI 继续轮询（H17）。实测：不加这一条时，"先挂程序、不开 need_wakeup"的 l2fwd 会在突发时丢掉约 0.35% 的帧。

**归还（实现名 `rtl8125_xsk_rx_clear`）：** 对每个非空的 `rxb->xdp` 调用 `xsk_buff_free` 并置 NULL。调用时 NAPI 一定已经停了（close / rebuild，以及修复后的 link down / ESD / reset 路径）。

### 7.4 TX：XDP_TX、`ndo_xdp_xmit`、XSK 统一写描述符，统一释放

**写描述符（`rtl8125_xdp_tx_frame`，调用者持 `__netif_tx_lock`）**

```
padto = rtl8125_xdp_padto(tp, data, len);  /* 只有 CFG 2~7 的 PTP 勘误帧返回 > len，其余一律返回 len */
ndesc = (padto > len) ? 2 : 1;
检查空闲槽位 >= ndesc，否则返回 -EBUSY
如果需要填充：描述符 1 = tp->tx_pad_dma，长度 padto-len，LastFrag|Own（先写）
wmb()
描述符 0 = 数据 DMA，长度 len，FirstFrag|(ndesc==1 ? LastFrag : 0)|Own（后写，和官方顺序一致）
opts1 用 rtl8125_get_txd_opts1() 计算（自动处理 RingEnd）；opts2 = 0
ring_info[描述符 0].len = 需要 unmap 时为 len，否则为 0
ring_info[最后一个描述符].{type, xdpf, bytecount = len, gso_segs = 1}
netdev_tx_sent_queue(txq, len)          /* BQL 要和完成时的统计对得上 */
WRITE_ONCE(ring->cur_tx, cur_tx + ndesc)
```

- **普通短帧单描述符发送：** §10.1 实测硬件会自己补零到 60 字节，所以 42~59 字节的帧直接按原长度交给硬件，不再需要填充描述符。
- **填充描述符只服务于 PTP 勘误：** 8125A/B 上 L4 长度不足 47 字节的 UDP 319 / 320 帧，不补就会被硬件悄悄丢掉（§2.5）。填充用第二个描述符而不是写进数据缓冲区，是因为 UMEM 是用户的内存，不能往里写零；unaligned 模式下也不知道 chunk 的边界在哪；redirect 进来的帧尾部还有多少空间也不确定。
- **8125A/B 的 PTP 勘误：** `rtl8125_xdp_padto` 解析线性 buffer 中的 IPv4/IPv6 + UDP 目标端口 319/320，逻辑和官方 `rtl8125_get_patch_pad_len` / r8169 `rtl8125_quirk_udp_padto` 一样。
- **发完之后：** 如果 `!rtl8125_tx_slots_avail()`，就 `netif_tx_stop_queue`。否则官方 `start_xmit` 会打印 *"BUG! Tx Ring full when queue awake"*（`r8125_n.c:20617`）。官方的完成路径会自动唤醒队列。
- 每个调用点都调用 `txq_trans_cond_update(txq)`，避免误报 TX 超时（igc / igb 也这样做）。

**三个入口**

| 入口 | 环 | 时机 |
|---|---|---|
| `XDP_TX`（页路径 / ZC） | `tx_ring[smp_processor_id() % num_tx_rings]` | RX NAPI 中；门铃在 `rtl8125_xdp_finalize` 里持锁敲一次 |
| `ndo_xdp_xmit` | 同上 | 任意 CPU；带 `XDP_XMIT_FLUSH` 时敲门铃；返回成功发出的帧数 |
| XSK TX（实现名 `rtl8125_xsk_xmit`） | `tx_ring[q]`，q 与绑定的 rx 队列相同 | H11：TX 清理之后、持锁；`xsk_tx_peek_release_desc_batch(pool, 空闲槽位 / 2)`，**不受 NAPI budget 限制**（TX 不计入 budget，一次唤醒可以填满环）；`xsk_buff_raw_get_dma` + `raw_dma_sync_for_device`；XSK 的 `len = 0`（不 unmap）；最后敲门铃，并 `xsk_set_tx_need_wakeup` |

**统一释放（H10，`rtl8125_tx_release`）**

```c
static void rtl8125_tx_release(tp, ring, entry, budget, &pkts, &bytes, &xsk_frames)
{
        struct ring_info *ri = &ring->tx_skb[entry];

        if (ri->len)
                rtl8125_unmap_tx_skb(tp->pci_dev, ri, ring->TxDescArray + entry);

        switch (ri->type) {
        case RTL_TX_XDP: xdp_return_frame(ri->xdpf);  account(ri); break;
        case RTL_TX_XSK: (*xsk_frames)++;              account(ri); break;
        default:         if (ri->skb) { account(ri); RTL_NAPI_CONSUME_SKB_ANY(ri->skb, budget); }
        }
        ri->skb = NULL;
        ri->type = RTL_TX_SKB;
}
```

- 两个完成循环（noclose / close）和 `tx_clear_range` 都改成调用它，顺便去掉了官方三处重复的代码。
- 循环结束后：如果 `xsk_frames` 非 0，调用 `xsk_tx_completed(ring->xsk_pool, xsk_frames)`。
- 在 `tx_clear_range`（复位 / 关闭）路径里，XSK 帧同样要计数并调用 `xsk_tx_completed`，否则用户态的 completion 环会少计数。

### 7.5 XSK pool 设置（`XDP_SETUP_XSK_POOL`）

```
启用 (pool != NULL):
  检查 §8 的准入规则              → 不满足返回 -EINVAL / -EOPNOTSUPP
  xsk_pool_dma_map(pool, &pdev->dev, DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_WEAK_ORDERING)
  if (netif_running && tp->xdp_prog)
          rtl8125_rings_rebuild(dev, set_pool, {qid, pool})
          失败 → 取消 pool 的 DMA 映射，返回错误
          成功 → rtl8125_xsk_wakeup(dev, qid, XDP_WAKEUP_RX)（启动收包，igc / igb 的写法）
  else
          只记录 rx_ring[qid].xsk_pool / tx_ring[qid].xsk_pool，等下次 open 或挂程序时生效

关闭 (pool == NULL):              ← 内核不允许这一步失败
  pool = rx_ring[qid].xsk_pool（这时它仍然注册在 qid 上，§2.3）
  if (netif_running && tp->xdp_prog) rtl8125_rings_rebuild(dev, set_pool, {qid, NULL})
  else 直接清空指针
  xsk_pool_dma_unmap(pool, 同样的 attrs)
  如果重建失败：打印错误，但仍然清空指针并返回 0。设备会停在 down 状态，和官方 change_mtu 失败时的语义一致
```

**实现：** 两处重建失败都会用 `netdev_err` 打印出来，日志里带队列号和错误码，以前这两处的返回值都被直接丢掉。

- 解绑时重建失败：返回值仍是 0。
- 绑定失败后的回滚重建也失败：返回给调用方的仍是第一次绑定失败的错误码，不会被回滚的错误码覆盖。

两种情况下都不会留下悬空的 pool 指针：`rtl8125_rings_rebuild()` 先调用 `set_pool` 清空指针，再执行 `init_ring`，所以重建失败时指针也已经清空，之后的 `xsk_pool_dma_unmap()` 是安全的。

这套控制流有一个主机回归测试：`tools/xdp/test_xsk_pool_setup.py`，见 XDP.md §5。

### 7.6 挂/卸程序（`XDP_SETUP_PROG`）

```
如果 MTU 和 ZC 帧大小冲突（有任一 pool 的 frame_size < rx_buf_sz）→ -EINVAL
old = tp->xdp_prog
if (!!old != !!new && netif_running)
        rtl8125_rings_rebuild(dev, swap_prog, new)   /* headroom 改变、ZC 是否生效改变 */
else
        xchg(&tp->xdp_prog, new)                      /* 热替换：只换程序，不重建 */
if (old) bpf_prog_put(old)
new ? xdp_features_set_redirect_target(dev, false) : xdp_features_clear_redirect_target(dev)
```

### 7.7 唤醒与 NAPI 映射（整个驱动里唯一知道 ISR 差异的地方）

```c
static struct napi_struct *rtl8125_q_napi(struct rtl8125_private *tp, u32 q, bool tx)
{
        switch (tp->HwCurrIsrVer) {
        case 1:         return &tp->r8125napi[0].napi;
        case 3: case 4: return &tp->r8125napi[q].napi;
        case 7:         return &tp->r8125napi[tx ? 27 + q : q].napi;
        case 2:         return &tp->r8125napi[tx ? 16 + 2 * q : q].napi;
        default:        return &tp->r8125napi[tx ? 16 + q : q].napi;   /* v5 */
        }
}

int rtl8125_xsk_wakeup(struct net_device *dev, u32 q, u32 flags)
{
        /* 检查：运行中且 link up；q < num_rx_rings && q < num_tx_rings；
           tp->xdp_prog 非空；rx_ring[q].zc 为真 */
        if (flags & XDP_WAKEUP_RX) kick(rtl8125_q_napi(tp, q, false));
        if (flags & XDP_WAKEUP_TX) kick(rtl8125_q_napi(tp, q, true));
        return 0;
}

static void kick(struct napi_struct *napi)
{
        if (!napi_if_scheduled_mark_missed(napi) && napi_schedule_prep(napi))
                __napi_schedule(napi);       /* fec 的写法 */
}
```

ISR v1 / v3 / v4 下 RX 和 TX 是同一个 NAPI，`kick` 两次时第二次会变成"标记 missed"，不会出问题。

### 7.8 `xdp_rxq_info`

- **实现：** 在 `rtl8125_alloc_rx_desc` 里对每个 rx 环 `xdp_rxq_info_reg(&ring->xdp_rxq, dev, i, napi_id)`，在 `rtl8125_free_rx_desc` 里 unreg（H13）。队列号用循环变量：`ring->index` 要到 init_ring 才赋值。
- `rtl8125_xdp_ring_setup`（H5）先 `xdp_rxq_info_unreg_mem_model`，再注册对应的内存模型：
  - ZC：`MEM_TYPE_XSK_BUFF_POOL` + `xsk_pool_set_rxq_info(pool, &ring->xdp_rxq)`
  - 其他：`MEM_TYPE_PAGE_SHARED`
- **实现：NAPI id。** 从 6.13 起 NAPI id 在 `napi_enable` 时才分配，且驱动没有持久的 NAPI 配置时每次 enable 都会换新 id；`xdp_rxq_info.napi_id` 在 6.13 被删掉，AF_XDP 改为从 `netif_queue_set_napi()` 登记的队列映射里取。所以 `rtl8125_enable_napi()` 末尾调用 `rtl8125_xdp_napi_enabled()`（H16）：6.13 之前写 `xdp_rxq.napi_id`，6.8 起对 RX 和 TX 队列都调用 `netif_queue_set_napi()`（持 rtnl，所有调用方都满足）。
- **实现：** `xdp_features` 在 probe 时一次设好：`BASIC | REDIRECT | NDO_XMIT | XSK_ZEROCOPY`。TX 环始终存在，`ndo_xdp_xmit` 不依赖程序是否挂着，所以不需要随挂/卸程序动态切换 redirect target。

---

## 8. 约束与准入规则

| 规则 | 在哪里检查 | 原因 |
|---|---|---|
| `ENABLE_XDP_SUPPORT` 不能和 `ENABLE_LIB_SUPPORT` / `ENABLE_RX_PACKET_FRAGMENT` 同时开，并且必须开 `ENABLE_PAGE_REUSE` | 编译期 `#error` | lib ring 会接管环；第一版不做多缓冲；页路径 XDP 依赖页翻转模型 |
| 内核 ≥ 6.6 | 编译期 `#error` | §4 |
| ZC 只允许 `qid < num_rx_rings && qid < num_tx_rings`（默认配置下就是只有队列 0） | pool 设置 | 和 igc / igb / fec 一致；TX 环不足 |
| `xsk_pool_get_rx_frame_size(pool) >= tp->rx_buf_sz` | pool 设置、挂程序、改 MTU | ZC 描述符长度和全局 `rx_buf_sz` 一致，官方所有重新标记描述符的路径都不用改；2048 的 chunk 可用 1792 字节 ≥ 1522 |
| RX 对齐：**不设规则**。unaligned pool 可以接受，headroom 取任何值都可以 | — | §2.2 实测任意字节对齐都可以 |
| `XDP_SETUP_XSK_POOL(NULL)` 不允许失败 | pool 关闭 | §2.3 |
| 开着 XDP 时改 MTU：重新检查上面的帧大小规则 | `rtl8125_xdp_check_mtu` | 防止 ZC 环被撑破 |

---

## 9. 补丁序列

每个补丁都能单独编译和测试。下表是实际提交的序列（`main`）。

| # | 提交 | 是否改变行为 | 上机验证（A7Z） |
|---|---|---|---|
| 0 | `vendor`：原样导入 Realtek 9.018.00 | — | — |
| 1 | 隐患 A：页复用路径无条件 `skb_reserve(rx_offset)` | 修 bug | 第 0 步 1~258 字节偏移 |
| 2 | 隐患 B：`reset_task` 改为 clear + `init_ring` | 修 bug | 7 次 reset × TCP 流，10.3 GiB（V3/V4 未上机） |
| 3 | 隐患 C：`rtl8125_reset_rings()`；`change_mtu` 失败也重开 NAPI | 修 bug | 6 次重新协商 + 4 次改 MTU，7.5 GiB（ESD 未能触发） |
| 4 | `rtl8125_rings_rebuild()` | 不变 | 6 次改 MTU，14.9 GiB |
| 5 | 隐患 D：丢包经 `dirty_rx` 回收 | 修 bug | 修复前每万次丢包泄漏约 5000 页，修复后为 0 |
| 6 | `rtl8125_tx_release()` + `ring_info.type` | 不变 | 后续全部 TX 测试 |
| 7 | `tp->rx_headroom`，按 headroom 计算页阶数 | 不变 | 同上 |
| 8 | 页复用路径只同步收到的字节 + 预取 | 性能 | 95 kpps 全协议栈下无可测差别 |
| 9 | XDP 框架：文件、Makefile、`xdp_rxq`、队列到 NAPI 映射 | 关掉开关时不变 | — |
| 10 | 页路径 XDP：DROP / PASS / REDIRECT，挂/卸程序 | 新功能 | 挂卸 × TCP 流、DROP 无泄漏、cpumap |
| 11 | XDP_TX + `ndo_xdp_xmit` + 8125A/B 勘误填充 | 新功能 → 软路由终点 | 回弹 / devmap、81 字节 PTP 填充 |
| 12 | AF_XDP 零拷贝 RX | 新功能 | 校验和、PASS 混合流量、生命周期 |
| 13 | AF_XDP 零拷贝 TX，声明 `XSK_ZEROCOPY` | 新功能 | l2fwd、各长度 TX、发送速率 |
| — | `tools/xdp/` 测试程序，`docs/XDP.md` | — | — |
| 14 | XDP 的 TX 加锁改为无条件加锁、调用方持锁检查 carrier（sparse 干净） | 不变 | 调试内核全套 |
| 15 | 隐患 E：兼容宏不再覆盖 `dma_mapping_error()` | 修 bug | DMA_API_DEBUG 下错误计数为 0 |
| 16 | XSK 解绑和回滚时，重建失败会打印错误（§7.5）；`xsktest` 核对实际协商到的模式；主机回归测试 | 只多了日志 | 未上机；主机回归 31 项通过，开 ASan / UBSan 也通过 |

后续可选：XDP RX hash 元数据、多缓冲 / jumbo、page_pool（转发场景下页复用失败率高时再做，见 XDP.md）、ethtool coalesce + DIM、开 RSS 后的多队列零拷贝。

Realtek 发新版时：更新 `vendor` 分支，`git rebase`。补丁 1~3、5 和 15 可以单独整理后反馈给 Realtek（nicfae@realtek.com）。

---

## 10. 第 0 步与测试计划

### 10.1 第 0 步：开发前的准备

1. **纳入版本管理。** 当前目录不是 git 仓库。`git init`，第一个提交原样放官方代码，作为 `vendor` 分支。
2. **测试环境。**
   - Linux 机器，带 RTL8125，内核 ≥ 6.6（**6.6、6.10 前后各一个、7.x 至少各测一个**，覆盖兼容宏的两个分支）
   - 开启 `CONFIG_XDP_SOCKETS=y`、`CONFIG_BPF_SYSCALL=y`
   - 安装 bpftool、xdp-tools（`xdp-bench`）、bpf-examples（`xdpsock`）
   - 对端发包机：pktgen 或 TRex
3. **确认芯片。** 读 `/proc/net/r8125/<ifname>/driver_var`，确认 `mcfg`（CFG_METHOD）、`HwCurrIsrVer`、`EnableTxNoClose`、`InitRxDescType`。
4. **RX 对齐探测（已完成，结果见下）。** 用 `exp/phase0` 分支的模块参数 `rx_offset` 改变缓冲区偏移，做法见该分支的 `docs/PHASE0.md` §4。结果决定 §8 的对齐准入规则。
5. **短包自动填充探测（已完成，结果见下）。** 用该分支的模块参数 `tx_pad` 关掉部分或全部软件填充，做法见 `docs/PHASE0.md` §5。结果决定 §7.4 能不能简化。
6. **基线数据。**
   - generic XDP：`xdp-bench drop|tx -m skb`
   - copy 模式：`xdpsock -r / -t / -l -S -c`
   - 软路由：nft flowtable 下的转发 pps 和 CPU 占用（之后用来和补丁 8 的 `xdp-forward` 对比）
   - **硬件上限（只在 DMA 一致的平台上测）：** x86 上用 DPDK `testpmd` + `net/r8169` 跑 64B 的 rxonly / txonly。Realtek 公布的数字是 RTL8125B 64B 约 1709 Mbit/s，约 2.5~3.3 Mpps。Rockchip 板子上不测这一项（§2.4）
   - 记录 Mpps 和 CPU 占用，后面每个补丁都和这组数对比。

#### 第 0 步实测结果（2026-11-06）

**环境：** Radxa Cubie A7Z（Allwinner A733，arm64，PCIe DMA 非一致），通过 FPC 转接板外接 RTL8125B（`mcfg 5`，PCIe Gen2 x1）；Armbian trixie，vendor 内核 6.6.98；`exp/phase0` 分支（f656656）。驱动拿到 1 个中断向量（`HwCurrIsrVer = 1`），RX 描述符 V1，`EnableTxNoClose = 1`。

**RX 对齐：** 对端是网关（OpenWrt，同样是 RTL8125），中间经过交换机。

| 偏移 | 0 | 1 | 2 | 4 | 8 | 16 | 32 | 64 | 258 |
|---|---|---|---|---|---|---|---|---|---|
| ICMP 逐字节比对（25 种长度 × 5 种内容 × 2 轮） | 250/250 | 250/250 | 250/250 | 250/250 | 250/250 | 250/250 | 250/250 | 250/250 | 250/250 |
| 1 GiB 字节流坏块 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 页阶数 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 |

各项协议栈校验和错误计数和接口错误计数的增量都是 0。字节流速度约 260 MB/s，各偏移之间没有差别。

**TX 短包填充：** A7Z 用网线直连 Windows 上的 Intel I225-V，用 `pktmon` 抓包。每组 20 帧。

| 模式 | 目的端口 | 负载 0 字节（42 字节帧） | 负载 17 字节（59 字节帧） |
|---|---|---|---|
| 3（官方默认） | 9 | 60 字节，补零，校验和正确 | 同左 |
| 3 | 319 / 320 | 81 字节（勘误填充），补零，校验和正确 | 同左 |
| 1（只关勘误填充） | 9 | 60 字节，补零，校验和正确 | 同左 |
| 1 | 319 / 320 | **一帧都没收到** | **一帧都没收到** |
| 0（全部关掉） | 9 | **60 字节，补零，校验和正确（硬件补齐）** | 同左 |
| 0 | 319 / 320 | **一帧都没收到** | **一帧都没收到** |

- 模式 0 / 1 下 PTP 帧丢失时，驱动的 `tx_packets` 照常增加，`tx_errors` / `tx_dropped` 为 0，对端没有接收错误计数。
- **抓包环境的教训：** 中间那台交换机会按 IPv4 总长度截掉帧尾，再补零到 60 字节（两个方向都会）。所以经过它抓包时，短帧的长度和填充内容都不可信；凡是和帧尾有关的测试，都必须直连。直连时，用原始套接字发出的 81 字节、尾部填 0x77 的对照帧能原样到达。

### 10.2 功能测试矩阵

| 类别 | 用例 |
|---|---|
| XDP 动作 | `xdp-bench drop / pass / tx / redirect`（redirect 到 cpumap、devmap、另一块网卡） |
| AF_XDP | `xdpsock rxdrop / txonly / l2fwd`，`-z` 和 `-c` 对比；打开和不打开 need_wakeup（`-m`） |
| 绑定时序 | 先 bind 后填 fill 环；先挂程序后建 socket；先建 socket 后挂程序；socket 存在时卸载程序 |
| 生命周期 | 流量进行中：拔插网线（隐患 C 的路径）、改 MTU、`ip link down/up`、热替换程序、关闭 socket；触发 TX 超时，走 reset_task（隐患 B 的路径） |
| 短包 | 42~59 字节帧经过 XDP_TX / XSK TX，对端确认是 60 字节且填充为全零；8125A/B 上测 UDP 319/320 的短包 |
| 拓扑 | ISR v1（单向量或 8125CP）和 v2/v5（分离向量）各至少一种芯片；`ENABLE_RSS_SUPPORT = y` 时队列 0 和非 0 队列（应拒绝） |
| 平台 | **x86（一致）和 arm64 非一致平台（R5S 或 E52C）都是必测项**。非一致平台上开 `CONFIG_DMA_API_DEBUG`，并长时间跑 XDP_TX 和 l2fwd，专门抓漏掉的 sync |
| 软路由 | 两个 r8125 口之间跑 `xdp-forward`（依赖补丁 8 的 `ndo_xdp_xmit`）和 nft flowtable，对比 pps 和 CPU 占用；PPPoE / VLAN 场景下 XDP_PASS 回到协议栈是否正常 |
| 官方回归 | 关掉 `ENABLE_XDP_SUPPORT`，行为和官方完全一致（iperf3、TSO、VLAN、PTP、WoL 各跑一次） |
| 自测 | `tools/testing/selftests/bpf/xskxceiver`（需要两个口或环回线） |
| 压力 | 长时间 `l2fwd -z`，同时开 KASAN、`CONFIG_DMA_API_DEBUG`，检查泄漏 / 重复 unmap / DMA 越界 |

---

## 11. 风险与待确认项

| # | 风险 | 应对 |
|---|---|---|
| R1 | ~~RX 对齐要求未知~~ | **已解决：** §10.1 实测任意字节对齐都可以，§8 不设对齐规则 |
| R2 | 非一致性 DMA 架构上，程序写 headroom 和 DMA 区域共用一条 cache line（`NET_IP_ALIGN = 2` 时边界不对齐） | **arm64 上不存在**：`NET_IP_ALIGN = 0`，256 字节的 headroom 天然按缓存行对齐。只在 32 位 arm / riscv / mips 上需要考虑，必要时让 headroom 按缓存行对齐 |
| R8 | 硬件小包上限（约 2.5~3.3 Mpps）可能让补丁 9~10 的收益很小 | 先看 §10.1 的硬件上限和补丁 8 的实测，再决定做不做零拷贝 |
| R3 | 硬件对很小的 TX 分片（勘误填充描述符只有 1~43 字节）的处理没有文档 | skb 的 frag 本来就可能很小，属于中等置信度；短包测试必须覆盖 |
| R4 | 隐患 A/B/C 的修复改变了官方行为 | 每个单独一个补丁，可以单独回滚；回归测试覆盖 |
| R5 | 重建失败会让设备停在 down 状态 | 和官方 `change_mtu` 的语义一致；打印错误。后续可以考虑先建新环再替换（fec 的写法） |
| R6 | 官方驱动在 7.x 内核上能否编译没有验证（它最高只处理到 6.13） | 第 0 步先编译官方原版确认 |
| R7 | 2.5G 下零拷贝的收益可能有限 | 第 8 个补丁之后做评估，再决定是否继续 |
| R9 | 硬件自动补零只在 RTL8125B（CFG_5）上实测过，§7.4 的单描述符发送依赖它 | §10.2 的短包用例在每种芯片上都直连抓包确认；如果有芯片不补齐，对它额外打开填充描述符 |

---

## 12. 参考资料

### 本地（只读参考）

所在目录：项目内 `docs/refs/`（约 8.5MB、283 个文件，均为 GPL-2.0 的上游内核源码，只供对照阅读，不参与编译；已去掉稀疏克隆的 `.git`）

| 路径 | 内容 |
|---|---|
| `linux/`（v7.3 稀疏检出） | `drivers/net/ethernet/{intel/igc, engleder (tsnep), realtek (r8169, rtase), stmicro/stmmac, intel/ice/ice_xsk*}`、`include/net/{xdp.h, xdp_sock_drv.h, xsk_buff_pool.h, page_pool/}`、`net/xdp/`、`Documentation/networking/af_xdp.rst` |
| `extra/` | v7.3 的 `fec_main.c`（整机复位方式的 XSK 先例）、`igb_xsk.c` / `igb_main.c`（最接近的模板） |
| `hdr/<tag>/` | v5.10 ~ v7.3 共 13 个版本的 XDP / XSK / page_pool 头文件，用于 §4 的矩阵 |
| `old/` | r8169 的 v2.6.30 / v3.0 / v4.4 版本（8168 的对齐依据） |

### 重点对照的函数

| 本方案 | 参考 |
|---|---|
| `rtl8125_xsk_tx` | igb `igb_xmit_zc` + igb_main.c:8440-8451（持锁调用） |
| `rtl8125_rx_zc` | igc `igc_clean_rx_irq_zc`、igb `igb_clean_rx_irq_zc` |
| `rtl8125_xsk_ring_fill` | igb `igb_fill_rx_descs` / `igb_alloc_rx_buffers_zc` |
| `rtl8125_xsk_construct_skb` | igc `igc_construct_skb_zc` |
| pool 设置 + 整机复位 | fec `fec_xsk_restart_napi` / `fec_setup_xsk_pool` |
| `rtl8125_xsk_wakeup` | fec `fec_enet_xsk_wakeup` |
| XDP_TX 共用环 | igc `igc_xdp_xmit_back` / `igc_finalize_xdp` |
| 填充勘误 | r8169 `rtl8125_quirk_udp_padto` / `rtl_quirk_packet_padto` |

### 在线资料

- [r8169: Add XDP support for pass and drop actions（lore）](https://lore.kernel.org/all/7b36b2ac-223f-a34a-4f86-7f4f0b027898@gmail.com/T/)
- [Heiner Kallweit 的回复（lkml）](https://lkml.org/lkml/2021/9/14/1000)
- [torvalds/linux v7.3](https://github.com/torvalds/linux/tree/v7.3)
- [AF_XDP 文档](https://docs.kernel.org/networking/af_xdp.html)
