#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# r5s-envcheck.sh - read-only environment report for r8125 XDP development.
#
# Collects kernel, hardware, driver and tool information needed by
# docs/TEST_R5S.md. It never modifies the system. Run as a normal user;
# running with sudo additionally shows dmesg, PCIe capabilities and
# /proc/net/r8125 details that may be root-only.
#
#   sh tools/r5s-envcheck.sh | tee envcheck.txt

sec() { printf '\n===== %s =====\n' "$1"; }
run() { printf '$ %s\n' "$*"; sh -c "$*" 2>&1 || true; }
have() { command -v "$1" >/dev/null 2>&1; }

sec "system"
run "date -u"
run "uname -a"
run "cat /etc/os-release | grep -E '^(PRETTY_NAME|VERSION_ID)='"
run "cat /proc/device-tree/model 2>/dev/null; echo"
run "nproc"
run "free -m | head -2"

sec "cpu frequency / thermal"
for p in /sys/devices/system/cpu/cpufreq/policy*; do
	[ -d "$p" ] || continue
	printf '%s: gov=%s cur=%s max=%s\n' "${p##*/}" \
		"$(cat "$p/scaling_governor" 2>/dev/null)" \
		"$(cat "$p/scaling_cur_freq" 2>/dev/null)" \
		"$(cat "$p/cpuinfo_max_freq" 2>/dev/null)"
done
for t in /sys/class/thermal/thermal_zone*; do
	[ -d "$t" ] || continue
	printf '%s: %s %s\n' "${t##*/}" "$(cat "$t/type" 2>/dev/null)" "$(cat "$t/temp" 2>/dev/null)"
done

sec "kernel config"
CFG=""
if [ -r "/boot/config-$(uname -r)" ]; then
	CFG="cat /boot/config-$(uname -r)"
elif [ -r /proc/config.gz ]; then
	CFG="zcat /proc/config.gz"
fi
if [ -n "$CFG" ]; then
	for k in BPF_SYSCALL BPF_JIT XDP_SOCKETS DEBUG_INFO_BTF NET_PKTGEN NETCONSOLE \
		 PCI_MSI PAGE_POOL DMA_API_DEBUG KASAN PROVE_LOCKING DEBUG_ATOMIC_SLEEP \
		 R8169 ARM64_DMA_USE_IOMMU SWIOTLB; do
		v=$($CFG | grep -E "^(# )?CONFIG_${k}[= ]" | head -1)
		printf '%-28s %s\n' "$k" "${v:-(absent)}"
	done
else
	echo "no kernel config found (/boot/config-* or /proc/config.gz)"
fi
run "ls -l /sys/kernel/btf/vmlinux"

sec "build environment"
run "ls -ld /lib/modules/$(uname -r)/build"
run "gcc --version | head -1"
run "make --version | head -1"
run "clang --version | head -1"

sec "modules"
run "lsmod | grep -E '^(r8125|r8169|stmmac|dwmac)' "
run "grep -rh r8169 /etc/modprobe.d/ 2>/dev/null"

sec "pci (realtek 10ec)"
run "lspci -nnk -d 10ec:"
if have lspci; then
	for dev in $(lspci -D -d 10ec: | awk '{print $1}'); do
		echo "--- $dev"
		lspci -vv -s "$dev" 2>/dev/null | grep -E 'LnkCap:|LnkSta:|MSI-X:|MSI:' || true
		printf 'iommu_group: %s\n' "$(readlink "/sys/bus/pci/devices/$dev/iommu_group" 2>/dev/null || echo none)"
	done
fi

sec "dma coherency / iommu"
run "find /proc/device-tree -name dma-coherent 2>/dev/null | head -20"
run "echo dma-coherent nodes: \$(find /proc/device-tree -name dma-coherent 2>/dev/null | wc -l)"
run "ls /sys/kernel/iommu_groups/ | wc -l"
run "dmesg 2>/dev/null | grep -i -E 'smmu|iommu' | head -10"

sec "network interfaces"
run "ip -br link"
for n in /sys/class/net/*; do
	ifc=${n##*/}
	drv=$(basename "$(readlink "$n/device/driver" 2>/dev/null)" 2>/dev/null)
	[ "$drv" = "r8125" ] || [ "$drv" = "r8169" ] || continue
	echo "--- $ifc (driver $drv)"
	run "ethtool -i $ifc"
	run "ethtool $ifc | grep -E 'Speed|Duplex|Link detected'"
	run "ethtool -l $ifc"
	run "ethtool -g $ifc"
	run "ethtool -k $ifc | grep -E 'rx-checksumming|tx-checksumming|scatter-gather|tcp-segmentation|generic-receive|rx-all|rx-fcs'"
	run "grep -c $ifc /proc/interrupts"
	dv=/proc/net/r8125/$ifc/debug/driver_var
	if [ -r "$dv" ]; then
		echo "driver_var (selected):"
		grep -E '^(driver|mcfg|chipset|chipset_name|mtu|NUM_RX_DESC|NUM_TX_DESC|rx_buf_sz|rx_buf_page_order|rx_buf_page_size|page_reuse_fail_cnt|aspm|UseSwPaddingShortPkt|ShortPacketSwChecksum|HwSuppTxNoCloseVer|EnableTxNoClose|InitRxDescType|RxDescLength|num_rx_rings|num_tx_rings|HwSuppNumRxQueues|HwSuppNumTxQueues|EnableRss|EnablePtp|min_irq_nvecs|irq_nvecs|hw_supp_irq_nvecs|ring_lib_enabled|HwSuppIsrVer|HwCurrIsrVer)[[:space:]]' "$dv"
	else
		echo "driver_var: not available (load r8125, or run with sudo)"
	fi
done

sec "tools"
for t in bpftool xdp-loader xdp-bench xdp-trafficgen xdp-forward xdpsock iperf3 tcpdump ethtool perf mpstat trace-cmd nft; do
	if have "$t"; then printf '%-16s %s\n' "$t" "$(command -v "$t")"; else printf '%-16s MISSING\n' "$t"; fi
done
run "bpftool version | head -2"
run "xdp-loader --version 2>/dev/null | head -1"
run "ethtool --version"

sec "done"
