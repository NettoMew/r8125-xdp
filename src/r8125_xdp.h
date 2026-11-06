/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * XDP and AF_XDP support for the r8125 driver.
 *
 * r8125_n.c calls into this header at a fixed set of points. Without
 * ENABLE_XDP_SUPPORT every one of those calls is an empty inline function,
 * so the driver builds and behaves exactly as the vendor code does.
 */

#ifndef _LINUX_R8125_XDP_H
#define _LINUX_R8125_XDP_H

#include <linux/version.h>
#include "r8125.h"

#ifdef ENABLE_XDP_SUPPORT

#if LINUX_VERSION_CODE < KERNEL_VERSION(6,6,0)
#error "ENABLE_XDP_SUPPORT requires Linux 6.6 or later"
#endif

#if !defined(ENABLE_PAGE_REUSE) || defined(ENABLE_RX_PACKET_FRAGMENT) || \
    defined(ENABLE_LIB_SUPPORT)
#error "ENABLE_XDP_SUPPORT needs ENABLE_PAGE_REUSE, without ENABLE_RX_PACKET_FRAGMENT or ENABLE_LIB_SUPPORT"
#endif

struct napi_struct *rtl8125_q_napi(struct rtl8125_private *tp, u32 q, bool tx);

int rtl8125_xdp_rxq_reg(struct rtl8125_private *tp,
                        struct rtl8125_rx_ring *ring, u32 q);
void rtl8125_xdp_rxq_unreg(struct rtl8125_rx_ring *ring);
void rtl8125_xdp_napi_enabled(struct rtl8125_private *tp);

#else /* ENABLE_XDP_SUPPORT */

static inline int rtl8125_xdp_rxq_reg(struct rtl8125_private *tp,
                                      struct rtl8125_rx_ring *ring, u32 q)
{
        return 0;
}

static inline void rtl8125_xdp_rxq_unreg(struct rtl8125_rx_ring *ring) {}
static inline void rtl8125_xdp_napi_enabled(struct rtl8125_private *tp) {}

#endif /* ENABLE_XDP_SUPPORT */

#endif /* _LINUX_R8125_XDP_H */
