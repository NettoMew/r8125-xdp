# 第 0 步实验：RX 对齐与 TX 短包填充

配套文档：[AF_XDP_ZC_DESIGN.md](AF_XDP_ZC_DESIGN.md)（§2.2、§2.5、§7.4、§8、§10.1）、[TEST_R5S.md](TEST_R5S.md)

本实验只放在 `exp/phase0` 分支上，**不进补丁序列**。它回答设计里两个只能上机才能确定的问题：

| 问题 | 影响的设计 |
|---|---|
| RX 描述符里的缓冲区地址要按几字节对齐？ | §8 的 XSK pool 准入规则：要不要拒绝 unaligned pool，headroom 要满足什么对齐 |
| 硬件会不会自己把短于 60 字节的帧补齐？补的是不是零？ | §7.4：XDP_TX / XSK 发送要不要一律带填充描述符 |

---

## 1. 分支改了什么

驱动只多了两个只读模块参数，加载时读取一次。**默认值和官方驱动完全一致**，不传参数时行为不变。

| 参数 | 取值 | 作用 |
|---|---|---|
| `rx_offset` | 0~512，默认 `NET_IP_ALIGN`（x86 / arm64 上是 0） | 每个 RX 缓冲区在所在半页里的起始偏移，也就是写进描述符的地址的低位。页的阶数会随偏移自动放大（例如 4K 页上 258 会让每个缓冲区占一整页） |
| `tx_pad` | 位掩码，默认 3 | bit 0：短于 60 字节的帧由软件补齐；bit 1：8125A/B（CFG 2~7）的 PTP / 短 L4 头勘误填充。清掉 bit 0 时，短包的校验和仍然由软件计算，只去掉填充本身 |

- 只有页复用模式（`ENABLE_PAGE_REUSE = y`，默认）有 `rx_offset` 参数。
- 取值越界时加载失败并返回 `-EINVAL`；取了非默认值时，内核日志里会打一条 `experiment:` 警告。
- 当前取值可以从 `/sys/module/r8125/parameters/` 读到。
- **页复用的 `build_skb` 路径现在总会 `skb_reserve(skb, ring->rx_offset)`**，也就是隐患 A 的修复。不加这一行，任何非零偏移都会让 `skb->data` 比真正的帧头早 `rx_offset` 字节，实验就没法做。x86 / arm64 上默认偏移是 0，这一行不改变任何行为。

---

## 2. 工具：`tools/phase0.py`

一个 Python 3.9+ 脚本，只用标准库。

| 子命令 | 在哪跑 | 做什么 |
|---|---|---|
| `rx-offset IFACE ADDR/PLEN PEER` | R5S，root | 依次用每个偏移重新加载模块，然后做 ICMP 逐字节比对和 TCP 字节流比对，并记录协议栈错误计数 |
| `tx-pad IFACE ADDR/PLEN PEER` | R5S，root | 依次用每种填充模式重新加载模块，然后发 42 和 59 字节的 UDP 帧 |
| `serve` | 对端 | 给 `rx-offset` 提供确定内容的字节流，默认端口 5125 |
| `capture IFACE DUT_MAC` | 对端（Linux），root | 抓 `tx-pad` 发出的帧，统计线上长度、填充内容和校验和，并另存一份 pcap |

- 每做完一个取值，结果就追加一行到 `phase0.jsonl`，并在下一次重新加载模块之前 `fsync` 到磁盘。如果某个取值把内核搞崩了，前面的结果仍然留在文件里。
- 跑完默认会用官方默认值重新加载一次。`--no-restore` 可以保留最后一个取值。
- `tx-pad` 给每种模式分配了不同的 UDP 源端口（40000 + 模式），所以对端抓包时不用和 R5S 同步，看源端口就知道每一帧属于哪种模式。

---

## 3. 准备

**拓扑。** 和 [TEST_R5S.md](TEST_R5S.md) §1 一样：R5S 的一个 2.5G 口用网线直连对端，中间不接交换机。下文假设：

| | 接口 | 地址 |
|---|---|---|
| R5S | `$IF` | 192.168.125.2/24 |
| 对端 | `$PEER_IF` | 192.168.125.1/24 |

**对端的要求：**
- `rx-offset`：对端只需要能回 ping、能跑 `serve`。任何机器都可以，网卡速率不限，Mac 也行。
- `tx-pad`：`capture` 用的是 `AF_PACKET`，只能在 Linux 上跑。最好用 Intel 网卡（igc / igb / e1000e），它们的 `ethtool -S` 有 runt / undersize 计数。

**R5S 上：**

```sh
cd ~/r8125 && git switch exp/phase0
make -C /lib/modules/$(uname -r)/build M=$PWD/src -j4 modules
# 被测口不要交给 NetworkManager / systemd-networkd 管理，否则每次重新加载模块后它会去改地址
nmcli device set $IF managed no 2>/dev/null || true
```

**安全：**
- RX 偏移不合法时，最坏的情况是硬件把数据写到缓冲区以外，可能造成内存损坏，甚至 panic。
- 实验前接好串口或 netconsole（[TEST_R5S.md](TEST_R5S.md) §6）。
- 不要在 R5S 上同时跑别的重要任务。

---

## 4. RX 对齐探测

```sh
# 对端
python3 tools/phase0.py serve
# R5S
sudo python3 tools/phase0.py rx-offset $IF 192.168.125.2/24 192.168.125.1
```

默认依次测 `0,1,2,4,8,16,32,64,258`，可以用 `--offsets` 指定。每个偏移做这几件事：

1. 用 `rx_offset=N` 重新加载模块，确认 sysfs 里读到的确实是 N。
2. 把接口拉起来，等链路 up，配置地址，然后 `ethtool -K $IF rx off`，让内核用软件验证所有校验和。**硬件校验的是线上收到的数据，DMA 写错位置时它照样会报"校验通过"，所以这一步必不可少。**
3. ICMP 扫描：25 种长度（1~1472，覆盖 2 的幂附近的边界）× 5 种填充内容（00、ff、a5、递增、随机）× 2 轮，每个回包逐字节比对，并验证 ICMP 校验和。
4. 字节流：从对端拉 1 GiB（`--bytes` 可改，0 表示跳过）。每 64 KiB 块开头带块序号，其余内容固定。每块都比对，出错时记下第一个错误的位置。
5. 记录 `Ip.InHdrErrors`、`IpExt.InCsumErrors`、`Icmp/Tcp/Udp.InCsumErrors` 和接口的 rx 错误计数的增量，再加上这段时间内的内核日志。

**看结果：**

| 现象 | 说明 |
|---|---|
| 所有偏移都 `PASS` | 硬件接受任意字节对齐的地址。§8 不需要对齐规则，unaligned pool 可以接受 |
| 偏移是 2^k 的倍数时 `PASS`，其余 `FAIL` | 对齐粒度是 2^k。§8 要求 `xsk_pool_get_headroom(pool) % 2^k == 0`，并拒绝 unaligned pool |
| `FAIL`，回包基本全丢，`Ip.InHdrErrors` 上涨 | 硬件很可能把地址向下对齐了，整帧错位，IP 头校验不过 |
| `FAIL`，只有少量字节不对（`echo_samples` 或 `first_bad` 里有位置） | 部分写入出错。把错误位置和包长报告上来，需要单独分析 |
| 258 和小偏移结果不同 | 258 会让页的阶数变化（4K 页上从 0 变成 1）。要分开看是偏移的问题还是页大小的问题 |

---

## 5. TX 短包填充探测

```sh
# 对端：如果网卡支持，先打开下面两项，这样本来会被丢弃的 runt 帧也能直接看到
sudo ethtool -K $PEER_IF rx-all on rx-fcs on
# 对端：开始抓包（默认抓 120 秒，Ctrl-C 可以提前结束）
sudo python3 tools/phase0.py capture $PEER_IF <R5S 上 $IF 的 MAC>
# R5S
sudo python3 tools/phase0.py tx-pad $IF 192.168.125.2/24 192.168.125.1
```

**R5S 发了什么。** 默认依次测模式 `3,1,0`。每种模式下，对每个目的端口（9：discard；319、320：PTP）× 每种负载长度（0、17，对应 42、59 字节的帧）各发 20 帧。负载字节是 0x5a，不是零，所以如果硬件把内存里的旧数据当填充发出去，很容易看出来。

**对端输出。** 每组给出：收到的帧数、线上长度（不含 FCS）、填充内容的分类（`zero`、`nonzero`，或者 `none`，表示帧不到 60 字节）、IP / UDP 校验和错误数，以及接收错误计数的增量。

**看结果：**

| 现象 | 说明 | 对 §7.4 的影响 |
|---|---|---|
| 模式 3：全部 60 字节，填充为 `zero` | 基线，说明抓包本身没问题 | 无 |
| 模式 0 / 1，端口 9：60 字节，填充为 `zero` | 硬件会自动补零 | 填充描述符只在 8125A/B 的 PTP 勘误时才需要（§7.4 的简化方案成立） |
| 模式 0 / 1，端口 9：60 字节，填充为 `nonzero` | 硬件会补齐，但补的是内存里的旧数据，会泄露信息 | 保留软件填充 |
| 模式 0 / 1，端口 9：收不到，对端的 runt / undersize 计数上涨 | 硬件不补齐，发出去的是 runt 帧 | 保留填充描述符 |
| 模式 0，端口 319 / 320 和端口 9 表现不同（收不到、长度不对、校验和错） | 8125A/B 的勘误确实存在 | 保留 PTP 勘误填充 |
| 模式 1，端口 319 / 320 正常 | 勘误填充单独就够用 | 支持只保留勘误填充 |

- `mcfg` 会写进日志。不在 CFG 2~7 范围内的芯片没有这个勘误，bit 1 不起作用。
- 每种芯片版本（`mcfg`）都要各测一次。

### 5.1 没有另一台 Linux 机器时：R5S 两个 2.5G 口对接

`tx-pad` 每种模式都要重新加载模块，而重新加载会把第二个 r8125 口也一起销毁。所以这种接法要手动逐个模式加载，并且用 `--loaded` 跳过重新加载。第二个口放进网络命名空间，否则发给它的包会直接走本机回环，不会上线。

```sh
# 每种模式重复一次：N 依次取 3、1、0
sudo rmmod r8125; sudo insmod src/r8125.ko tx_pad=N
sudo ip netns add peer 2>/dev/null; sudo ip link set $IF2 netns peer
sudo ip netns exec peer sh -c "ip link set $IF2 up && ip addr add 192.168.125.1/24 dev $IF2 \
    && ethtool -K $IF2 rx-all on rx-fcs on"
sudo ip netns exec peer python3 tools/phase0.py capture $IF2 <$IF 的 MAC> --seconds 30 &
sudo python3 tools/phase0.py tx-pad $IF 192.168.125.2/24 192.168.125.1 --loaded
wait
```

这时接收端也是 RTL8125。打开 `rx-all` 后它会把 runt 帧也交上来，所以硬件不补齐的话，会直接看到长度不到 60 的帧（填充为 `none`）。

---

## 6. 结果怎么交给我

- R5S：`phase0.jsonl`、命令的完整输出、`git log -1 --oneline`
- 对端：`capture` 的完整输出（需要时附上 pcap）
- 两块网卡各自的 `mcfg`（日志里有）
- 如果出现异常：串口或 netconsole 日志

拿到结果后，结论会写进设计文档的 §2.2、§2.5、§8 和 §7.4，然后从 `main` 开始写补丁 1。
