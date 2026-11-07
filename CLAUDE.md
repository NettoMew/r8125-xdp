# CLAUDE.md

Guidance for working in this repository: a Realtek r8125 9.018.00 vendor driver
with defect fixes and XDP / AF_XDP zero-copy support added on top.

## Layout

- `src/r8125_n.c`, `src/r8125.h`: vendor driver. Changes here are kept to
  single-line hooks, the defect fixes and a few refactors. Keep the vendor
  formatting; do not reformat vendor code.
- `src/r8125_xdp.c`, `src/r8125_xdp.h`: all XDP / AF_XDP logic. The header
  provides an empty `static inline` stub for every hook when
  `ENABLE_XDP_SUPPORT` is off, so `r8125_n.c` needs no `#ifdef` except in
  `net_device_ops`.
- `tools/xdp/`: `r8125_test.bpf.c` (UDP-only test programs) and `xsktest.c`
  (AF_XDP rx / l2fwd / tx).
- `docs/`: in Chinese. `XDP.md` is the user document; `AF_XDP_ZC_DESIGN.md`
  holds the design, the hook table (§5.3) and the patch table (§9). Keep both
  in sync with the code.
- `README`: the vendor README, left untouched. `README.md` is the project page.

## Branches

- `vendor`: the untouched vendor import. New Realtek releases go here, then
  `main` is rebased onto it.
- `main`: the patch series. Every commit must build on its own.
- `exp/phase0`: hardware experiments; never merged.

## Invariants the code relies on

- **Page reuse ring:** every slot from `dirty_rx` up to `cur_rx` is empty.
  Buffers only go back through `rtl8125_put_rx_buffer()` (flip) or
  `rtl8125_recycle_rx_buffer()` (same buffer). Never re-arm a descriptor in
  place.
- **Frames leaving the driver (PASS, XDP_TX, XDP_REDIRECT):** the frame owns
  one page reference. Flip the page before handing the frame on, because a
  receiver may drop it before the call returns.
- **Zero-copy ring:** slots are left empty after every frame and refilled
  from the pool at the end of the poll. `ring->zc` changes only in
  `init_ring`, with NAPI stopped.
- **Ring teardown and refill:** goes through `rtl8125_rings_rebuild()` for
  configuration changes and `rtl8125_reset_rings()` for link down and ESD.
  Callers hold rtnl and enter with NAPI enabled.
- **XDP and XSK transmit:** happen under the stack's TX queue lock, and check
  the carrier with the lock held. TX descriptors are released only through
  `rtl8125_tx_release()`, keyed by `ring_info.type`.
- **TX padding:** the MAC pads short frames with zeros itself. Only
  CFG_METHOD_2..7 need the PTP erratum padding (`rtl8125_xdp_pad_len()`).

## Building and checking

- Native: `make -C /lib/modules/$(uname -r)/build M=$PWD/src modules`.
- Before committing, build every commit against x86 and arm64 kernels 6.6,
  6.18 and 7.3:
  - build with XDP on and with `ENABLE_XDP_SUPPORT=n`;
  - compare the warnings against the previous commit; no new ones are
    allowed.
- Check the XDP code with `W=1 C=1` (sparse). The only accepted findings are
  the `TxDesc` endianness ones, which the vendor layout also produces.
- Kernel API differences go behind `LINUX_VERSION_CODE` checks. Verify the
  version against the actual headers of each kernel before writing one.
- Vendor compat fallbacks guarded by `#ifndef name` silently replace kernel
  functions that are not macros; that is how `dma_mapping_error()` became 0
  (hazard E). Gate such fallbacks on the kernel version instead.
- Run the test set on a kernel with `CONFIG_DMA_API_DEBUG`, `PROVE_LOCKING`
  and `KFENCE`: the DMA API error count must not grow, `debug_locks` must stay
  1 and the log must stay free of WARNING, BUG, lockdep and KFENCE reports.
  `nm -u r8125.ko` should list `debug_dma_mapping_error`.

## Testing

- **Test frames:** use `tools/xdp/r8125_test.bpf.c`. Its programs act only on
  UDP to ports 9, 319 and 320, so ARP and management traffic keep working.
  Programs that bounce every frame also bounce ARP and break the test.
- **Short-frame padding:** test over a direct cable. Some switches truncate
  IPv4 frames to the IP length and re-pad them.
- **Data integrity:** check end to end with a known byte pattern, not only
  with packet counts.
- **Truncated frames after close/open:** after an interface close and open, the
  hardware may deliver one frame cut to 60 bytes with no error bit. The
  unmodified vendor driver does it too. Tell it apart from data corruption
  before blaming the driver.
- **Memory leaks:** check by comparing `nr_free_pages` across rounds, after
  `drop_caches`.
- **Trigger paths:** to exercise the reset or ESD path, use a local test-only
  trigger; never commit one.

## Commits

- Kernel style: subject `r8125: ...`, `docs: ...` or `tools: ...`; the body
  is third-person prose explaining what changed and why, plus what was
  verified on hardware.
- No AI attribution trailers (no `Co-Authored-By`, no "Generated with"), no
  emoji.
- State plainly what was not tested.
