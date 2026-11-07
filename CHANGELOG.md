# 更新记录

## 9.018.00-xdp.1

基于 Realtek r8125 9.018.00 的第一个版本。

### 官方缺陷修复

- **A** `r8125: reserve rx_offset on the page reuse build_skb path`
  页复用路径无条件 `skb_reserve(rx_offset)`。`NET_IP_ALIGN = 2` 的架构上，收到的帧不再错位。
- **B** `r8125: rebuild the rings in reset_task instead of re-arming them`
  `reset_task` 改为清空后重新初始化环，不再把可能已被 RSS 信息覆盖的描述符原样交还硬件。
- **C** `r8125: stop NAPI before rebuilding the rings on link down and ESD`
  link down 和 ESD 重建环之前先停 NAPI。`change_mtu` 重建失败时也会重新打开 NAPI，之后执行 close 不再卡死。
- **D** `r8125: recycle dropped page reuse buffers through dirty_rx`
  丢包经 `dirty_rx` 回收，不再泄漏页和 DMA 映射。
- **E** `r8125: stop compiling out every DMA mapping error check`
  兼容宏只在 2.6.27 之前的内核上生效，DMA 映射错误检查恢复工作。

### 新功能

- `r8125: add the XDP framework`
  新增 `r8125_xdp.[ch]` 和 Makefile 开关，注册 `xdp_rxq`，登记队列到 NAPI 的映射。
- `r8125: run XDP programs on the page reuse path`
  原生 XDP：DROP / PASS / REDIRECT；挂/卸程序时重建环。
- `r8125: add XDP_TX and ndo_xdp_xmit`
  XDP_TX 和 `ndo_xdp_xmit`；8125A/B 的 PTP 勘误填充。
- `r8125: add AF_XDP zero-copy receive`
  零拷贝 RX、XSK pool 绑定、唤醒，以及不开 need_wakeup 时的持续轮询。
- `r8125: add AF_XDP zero-copy transmit`
  零拷贝 TX，对外声明 `NETDEV_XDP_ACT_XSK_ZEROCOPY`。

### 重构与性能（不改变行为）

- `r8125: factor the ring rebuild out of change_mtu`：`rtl8125_rings_rebuild()`
- `r8125: release TX descriptors through one function`：`rtl8125_tx_release()`，并在 `ring_info` 里加类型字段
- `r8125: size RX pages from a per device headroom`：`tp->rx_headroom`
- `r8125: sync only the received bytes of a page reuse buffer`：只同步收到的字节，并预取帧头
- `r8125: make the XDP TX lock unconditional`：加锁结构让 sparse 能够检查

### 工具与文档

- `tools/xdp/`：只处理 UDP 测试帧的 XDP 程序，以及 AF_XDP 测试工具 `xsktest`
- `tools/r5s-envcheck.sh`：R5S 环境检查
- `README.md`、`CHANGELOG.md`、`CLAUDE.md`
- `docs/XDP.md`：用法和全部实测数据
- `docs/AF_XDP_ZC_DESIGN.md`：设计
- `docs/TEST_R5S.md`：R5S 测试计划

### 验证

- RTL8125B（Radxa Cubie A7Z，arm64）上完整实测，包括：
  - DMA_API_DEBUG、lockdep、KFENCE、KASAN 下的全套测试；
  - 两小时混合压测。
- x86、开 RSS 后的多队列、8125A / 8125D 尚未上机。详见 `README.md` 的“支持状态”。
