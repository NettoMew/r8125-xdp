# r8125：带 XDP 与 AF_XDP 零拷贝的 Realtek 2.5G 驱动

基于 Realtek 官方 Linux 驱动 r8125-9.018.00，在尽量少改动官方代码的前提下，增加了原生 XDP、`ndo_xdp_xmit` 和 AF_XDP 零拷贝收发，并修复了官方驱动的五个缺陷。关掉 `ENABLE_XDP_SUPPORT` 时，驱动行为和官方完全一致。

官方原版说明见 [`README`](README)。

## 功能

- **原生 XDP**：DROP / PASS / TX / REDIRECT（devmap、cpumap、xskmap、其他网卡）
- **`ndo_xdp_xmit`**：可以作为其他网卡 redirect 的目标，`xdp-forward` 可直接使用
- **AF_XDP 零拷贝**：RX 和 TX，支持 need_wakeup，busy poll 所需的队列到 NAPI 映射
- **8125A/B 的 PTP 勘误**：XDP 发出的短 UDP PTP 帧会按官方规则补齐，其余短帧由硬件补零

## 修复的官方缺陷

| | 问题 | 影响 |
|---|---|---|
| A | 页复用路径漏了 `skb_reserve(rx_offset)` | `NET_IP_ALIGN = 2` 的架构（riscv、arm32 等）上收到的帧整体错位 2 字节 |
| B | `reset_task` 只把描述符原样交还硬件 | 开 RSS（V3/V4 描述符）时硬件可能 DMA 到 RSS 信息构成的地址 |
| C | link down / ESD 重建 RX 环时没有停 NAPI | 与正在运行的 poll 并发释放页 |
| D | 页复用模式下丢包会打破 `dirty_rx` 不变量 | 每次丢包后泄漏一页及其 DMA 映射，实测每万次丢包约 5000 页 |
| E | 兼容宏 `#define dma_mapping_error(a,b) 0` 在 2.6.27 以后的内核上也生效 | 全部 DMA 映射错误检查被编译掉，映射失败的地址照样交给硬件 |

## 编译与安装

```sh
./autorun.sh                       # 官方脚本：编译、安装、加载
# 或者
make -C /lib/modules/$(uname -r)/build M=$PWD/src modules
```

XDP 在内核 ≥ 6.6、页复用打开（默认）时自动编入，其余情况按官方原样编译。`make ENABLE_XDP_SUPPORT=n` 可以显式关闭。

## 使用

```sh
xdp-loader load -m native enp1s0 prog.o
xdp-bench drop enp1s0 -m native
xdp-forward load enp1s0 enp2s0
```

AF_XDP 零拷贝照常以 `XDP_ZEROCOPY` 绑定，挂程序和建 socket 的先后顺序都可以。详细的准入规则和测试方法见 [`docs/XDP.md`](docs/XDP.md)。

## 分支

| 分支 | 内容 |
|---|---|
| `vendor` | Realtek 9.018.00 原样导入，只用于 rebase 新版本 |
| `main` | 缺陷修复、重构、XDP / AF_XDP，以及文档和测试工具 |
| `exp/phase0` | 开发前的硬件实验（RX 对齐、短帧填充），不进补丁序列 |

## 文档

| 文件 | 内容 |
|---|---|
| [`docs/XDP.md`](docs/XDP.md) | 用法、准入规则、测试工具、实测结果、尚未覆盖的项目 |
| [`docs/AF_XDP_ZC_DESIGN.md`](docs/AF_XDP_ZC_DESIGN.md) | 设计推导、内核兼容、调用点清单、补丁序列 |
| [`docs/TEST_R5S.md`](docs/TEST_R5S.md) | NanoPi R5S 测试计划 |
| [`tools/xdp/`](tools/xdp) | XDP 测试程序与 AF_XDP 测试工具 `xsktest` |

## 验证状态

- **编译**：x86 6.12；arm64 6.6、6.18、7.3。XDP 开和关两种配置，以及打开 RSS、PTP、多 TX 队列，都没有新增警告。`W=1` 和 sparse 下，XDP 代码只有与官方代码同源的描述符字节序提示。
- **上机**：Radxa Cubie A7Z，RTL8125B，arm64，PCIe DMA 非一致，单中断向量。覆盖了 V1 和 V3 描述符、reset 和 ESD 恢复、XDP 全部判决、零拷贝收发及其生命周期，结果见 `docs/XDP.md`。
- **调试内核**：在打开 DMA_API_DEBUG、lockdep、KFENCE、DEBUG_ATOMIC_SLEEP 的内核上跑了全套测试，驱动不再产生任何 DMA API 错误，lockdep 和 KFENCE 都没有报告。缺陷 E 就是这样发现的。
- **KASAN 与长跑**：同一套测试在 KASAN generic 内核上也跑完了，并在其上做了两小时混合压测（零拷贝、拷贝模式、XDP_TX 共约 2.7 亿帧，期间反复断链、改 MTU）。r8125 没有引起任何 KASAN、KFENCE、DMA API 或 WARNING 报告，内存没有增长。
- **已知的硬件 / 官方行为**：接口 close 再 open（`ip link set down/up`、`ethtool -G` 等）之后，偶尔会有一个描述符报告收到 60 字节、没有任何错误位，缓冲区里却是原有的旧数据。官方原版驱动同样会出现。协议栈会按 IP 长度把这种帧丢掉；XDP 程序和 AF_XDP 应用应当像对待任何外部输入一样校验长度和内容。
- **尚未覆盖**：
  - x86 实机运行；
  - 开 RSS 后的多队列；
  - 8125A、8125D 等其他芯片；
  - 天级别的长跑。

## 许可证

GPL-2.0-only，与 Realtek 官方驱动相同。
