# NanoPi R5S 测试方案

配套文档：[AF_XDP_ZC_DESIGN.md](AF_XDP_ZC_DESIGN.md)（§10 测试计划、§2.4 平台差异）

| 项 | 内容 |
|---|---|
| 被测设备（DUT） | NanoPi R5S：RK3568B2，4 个 A55；2 个 RTL8125B(G) 2.5G 网口 + 1 个板载 1G 网口（RK3568 自带 GMAC） |
| 平台特点 | PCIe **DMA 不一致**、**没有 IOMMU**（§2.4）。任何漏掉的 `dma_sync` 都会暴露出来，适合做正确性测试 |
| 开发机 | Mac：写代码、git 管理。**不在 Mac 上编译**，模块在 R5S 上原生编译 |

---

## 1. 硬件拓扑

```
            ┌────────────── 管理网（家里的路由器 / 交换机）──────────────┐
            │                                                            │
        Mac (ssh, git push, 收 netconsole)                      R5S 板载 1G 口（stmmac）
                                                                         │
 ┌─────────── 流量机（推荐 x86 + 双口 Intel i226）────────────┐        R5S
 │  portA ─────────── 网线 ──────────────────► R5S 2.5G 口 #1 (r8125) │
 │  portB ◄────────── 网线 ─────────────────── R5S 2.5G 口 #2 (r8125) │
 └─────────────────────────────────────────────────────────────┘
          （可选）USB-TTL 串口 ──► R5S 调试串口，用于看 oops / panic
```

| 角色 | 要求 | 说明 |
|---|---|---|
| **管理口** | R5S 板载 1G 口 | SSH 和 netconsole 都只走这个口。它用的是 stmmac 驱动，r8125 崩了也不影响。**不要在这个口上做任何测试** |
| **测试口** | 两个 2.5G 口 | 都交给我们的 r8125。网线直连流量机，**不要经过交换机** |
| **流量机** | x86 + 双口 2.5G 网卡，**首选 Intel i226 / i225（igc 驱动）** | igc 支持原生 XDP 和 AF_XDP 零拷贝，`xdp-trafficgen` 能用它发出接近线速的小包。双口才能测转发：A → R5S → B |
| 没有流量机时 | 用一根网线把 R5S 的两个 2.5G 口直接对接 | **只能做功能测试**：收发和测量都在同一颗弱 CPU 上，性能数字没有参考价值 |
| 串口（建议） | USB-TTL 3.3V，接 R5S 调试串口；波特率 Rockchip 板通常是 1500000，以 FriendlyElec wiki 为准 | 内核 panic 时 netconsole 可能来不及发出日志，只有串口最可靠 |
| 散热 | 风扇或大散热片 | RK3568 满载会降频，测性能时必须先排除这个因素 |

---

## 2. 系统选择

| 选项 | 是否推荐 | 原因 |
|---|---|---|
| **Armbian（Debian 13 / Ubuntu minimal），主线内核 `current` 或 `edge`** | ✅ **首选** | 主线内核（≥ 6.6，最好 ≥ 6.10，兼容宏只走一个分支）、和我们分析过的主线设备树一致、有现成的 headers 包、能原生编译 |
| FriendlyElec 官方 Debian/Ubuntu 镜像 | ⚠️ 看内核版本 | 如果是 BSP 6.1 内核就**不满足**本方案的 ≥ 6.6 要求 |
| FriendlyWrt / OpenWrt | ❌ 不适合开发 | 没有原生工具链（musl）、BSP 内核、编译模块很麻烦。等功能稳定后再移植 |

- 6.6 ~ 6.9 的兼容分支（`xsk_buff_dma_sync_for_cpu` 双参数版本）放到后期，单独编一个 6.6 LTS 内核来测。
- Armbian 当前提供的内核版本以安装时的实际情况为准，装好后运行 §7 的检查脚本确认。

---

## 3. 内核配置要求

用 `/boot/config-$(uname -r)` 或 `/proc/config.gz` 检查（§7 的脚本会自动检查）：

| 配置项 | 要求 | 用途 |
|---|---|---|
| `CONFIG_BPF_SYSCALL` | =y | BPF |
| `CONFIG_BPF_JIT` | =y | 性能（arm64 JIT） |
| `CONFIG_XDP_SOCKETS` | =y | AF_XDP |
| `CONFIG_DEBUG_INFO_BTF` | =y | bpftool、xdp-tools 加载 CO-RE 程序 |
| `CONFIG_NET_PKTGEN` | =m | 在 R5S 上做 TX 基线 |
| `CONFIG_NETCONSOLE` | =m | 内核日志经管理口发到 Mac |
| `CONFIG_PCI_MSI` | =y | 多向量中断（决定 ISR 版本） |
| `CONFIG_PAGE_POOL` | 有更好 | 方案不依赖它（§4.3） |

**调试内核（第二阶段，需要自己编译主线内核）：**
- `CONFIG_DMA_API_DEBUG=y`：检查 map/unmap、sync 是否成对，**在非一致平台上最有用**
- `CONFIG_KASAN=y`（generic）：内存越界、释放后使用
- `CONFIG_PROVE_LOCKING=y`、`CONFIG_DEBUG_ATOMIC_SLEEP=y`：锁和上下文问题（tx lock、NAPI）

---

## 4. 软件包（Debian 系）

```sh
sudo apt install build-essential bc bison flex libelf-dev git rsync \
    ethtool iproute2 iperf3 tcpdump pciutils sysstat trace-cmd \
    linux-perf bpftool xdp-tools libxdp-dev libbpf-dev clang llvm \
    netcat-openbsd
# 内核头文件：Armbian 一般叫 linux-headers-<branch>-rockchip64，以 armbian-config 或 apt search 为准
ls -d /lib/modules/$(uname -r)/build   # 这个目录必须存在
```

- **xdp-tools** 需要包含 `xdp-bench`、`xdp-trafficgen`、`xdp-forward` 三个工具。发行版自带的版本没有 `xdp-forward` 的话，从 `github.com/xdp-project/xdp-tools` 源码编译。
- **xdpsock**：从 `github.com/xdp-project/bpf-examples` 编译 `AF_XDP-example`。
- **流量机**同样装 xdp-tools，用 `xdp-trafficgen` 发包、`xdp-bench drop` 收包计数。

---

## 5. 把网卡从 r8169 手里拿过来

```sh
# 开机时不让 r8169 抢走 RTL8125（R5S 的板载 1G 口用的是 stmmac，不受影响）
echo "blacklist r8169" | sudo tee /etc/modprobe.d/blacklist-r8169.conf
sudo update-initramfs -u
sudo reboot
lspci -nnk -d 10ec:      # 两个 [10ec:8125] 应该显示没有 driver in use
```

> ⚠️ **不要运行官方的 `autorun.sh`，也不要运行顶层的 `make` / `make install`。** `autorun.sh` 会把系统的 `r8169.ko` 改名成 `.bak`、安装到 `/lib/modules`、再更新 initramfs。开发阶段一律用下面的 `insmod` / `rmmod` 流程，不往系统里装任何东西。

---

## 6. 开发循环

**一次性设置：**

```sh
# R5S 上
git init ~/r8125 && git -C ~/r8125 config receive.denyCurrentBranch updateInstead
# Mac 上（在仓库目录里）
git remote add r5s ssh://<user>@<r5s-管理口IP>/~/r8125
git push r5s main
```

**每轮：**

```sh
# Mac
git push r5s <branch>                         # 推送后 R5S 的工作区会自动更新
# R5S
cd ~/r8125 && git switch <branch>
make -C /lib/modules/$(uname -r)/build M=$PWD/src -j4 modules
sudo rmmod r8125 2>/dev/null; sudo insmod src/r8125.ko
dmesg | tail -30
```

**日志：**

```sh
# R5S：把内核日志经管理口（假设叫 end0/eth0）发到 Mac 的 6666 端口
sudo modprobe netconsole netconsole=@/<管理口>,6666@<Mac-IP>/<Mac-MAC>
# Mac
nc -u -l 6666
```

---

## 7. 环境检查（**现在请先做这一步**）

在 R5S 上运行（脚本只读，不修改系统）：

```sh
sh ~/r8125/tools/r5s-envcheck.sh | tee envcheck.txt
```

然后：
1. `sudo insmod src/r8125.ko` 加载**官方原版**（`vendor` 分支）驱动后，**再跑一次**，这次能拿到 `driver_var`；
2. 两次的输出都贴给我。

我需要从输出里确认：内核版本和配置；两块网卡的 CFG_METHOD、`HwCurrIsrVer`（MSI-X 是否拿到 22 个以上向量）、`EnableTxNoClose`、`InitRxDescType`；PCIe 链路速度；设备树里没有 `dma-coherent`、也没有 IOMMU；各工具的版本。

---

## 8. 测试阶段

| 阶段 | 内容 | 前提 |
|---|---|---|
| **A. 环境就绪** | §1~§7 | 现在 |
| **B. 官方原版基线** | 用 `vendor` 分支：<br>iperf3 双向单流 / 多流；<br>流量机 `xdp-trafficgen` 发 64B/512B/1518B，R5S 用 `ip -s link`、`mpstat` 记录收包 pps 和 CPU；<br>`xdp-bench drop|pass -m skb`（generic XDP）；<br>nft flowtable 转发 A→B；<br>R5S 上用 `pktgen` 做 TX 基线 | A |
| **C. 第 0 步实验** | RX 对齐探测、短包自动填充探测（设计文档 §10.1 第 3、4 项）。我会写一个 `exp/phase0` 分支，用模块参数切换 offset 和是否填充，**不用每次重编** | A |
| **D. 补丁回归** | 每个补丁合入后：B 的基线重跑一遍，再加上设计文档 §10.2 里对应的功能项 | B |
| **E. 调试内核** | 自己编译的主线内核，开 DMA_API_DEBUG / KASAN / lockdep，长时间跑 XDP_TX、l2fwd 和拔插网线 | 补丁 7 之后 |
| **F. 6.6 兼容** | 编一个 6.6 LTS 内核，覆盖兼容宏的另一个分支 | 补丁 9 之后 |

### 8.1 测量要点

- **固定 CPU 频率：** 把所有核的 `cpufreq` governor 设成 `performance`；测试过程中记录 `/sys/class/thermal/thermal_zone*/temp`。
- **中断绑核：** 停掉 `irqbalance`，把每块 RTL8125 的中断向量分别绑到不同的核（`/proc/irq/<n>/smp_affinity_list`），**每次测试都记下绑定关系**。
- **每组跑 3 次，取中位数**；同时记下包长、流数、内核版本、分支和 commit。
- **验证对齐时，数据完整性不能依赖硬件校验和**：
  - 先 `ethtool -K <if> rx off`，关掉 RX 校验和卸载，让内核用软件校验；
  - 再用 `nc` 传一个 1GB 文件，两端比对 sha256；
  - 加上 `ping -p <pattern> -s <各种长度>`。
  - 原因：硬件校验的是线上收到的数据，如果 DMA 写错了位置，它照样会报"校验通过"。
- **判断短包有没有被硬件补齐：**
  - 看接收端（流量机）的 runt / undersize / length 错误计数（`ethtool -S`）；
  - 网卡支持的话，打开 `ethtool -K <if> rx-all on rx-fcs on`，再用 tcpdump 直接看帧长。

---

## 9. 结果怎么交给我

每次测试回传以下内容：
- `git log -1 --oneline`
- 测试命令
- 原始输出
- `dmesg` 中新增的部分
- 如果有异常：netconsole 或串口日志

格式不限，直接贴文本就行。
