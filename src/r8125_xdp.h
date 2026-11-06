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

/* Where a received frame sits in its buffer. A program may move either end. */
struct rtl8125_rx_frame {
        u32 offset;             /* from the start of the buffer */
        u32 len;
        u32 metasize;           /* XDP metadata right in front of the frame */
};

#ifdef ENABLE_XDP_SUPPORT

#if LINUX_VERSION_CODE < KERNEL_VERSION(6,6,0)
#error "ENABLE_XDP_SUPPORT requires Linux 6.6 or later"
#endif

#if !defined(ENABLE_PAGE_REUSE) || defined(ENABLE_RX_PACKET_FRAGMENT) || \
    defined(ENABLE_LIB_SUPPORT)
#error "ENABLE_XDP_SUPPORT needs ENABLE_PAGE_REUSE, without ENABLE_RX_PACKET_FRAGMENT or ENABLE_LIB_SUPPORT"
#endif

/* What an RX poll left for rtl8125_xdp_finalize() */
#define RTL8125_XDP_REDIR       BIT(0)
#define RTL8125_XDP_TX          BIT(1)

struct napi_struct *rtl8125_q_napi(struct rtl8125_private *tp, u32 q, bool tx);

int rtl8125_xdp_init(struct rtl8125_private *tp);
void rtl8125_xdp_exit(struct rtl8125_private *tp);
int rtl8125_xdp_rxq_reg(struct rtl8125_private *tp,
                        struct rtl8125_rx_ring *ring, u32 q);
void rtl8125_xdp_rxq_unreg(struct rtl8125_rx_ring *ring);
void rtl8125_xdp_napi_enabled(struct rtl8125_private *tp);

int rtl8125_xdp_bpf(struct net_device *dev, struct netdev_bpf *bpf);
int rtl8125_xdp_check_mtu(struct rtl8125_private *tp, int mtu);

bool rtl8125_xdp_rx(struct rtl8125_private *tp, struct rtl8125_rx_ring *ring,
                    u32 cur_rx, struct rtl8125_rx_buffer *rxb,
                    struct rtl8125_rx_frame *frame, u32 *status);
void rtl8125_xdp_finalize(struct rtl8125_private *tp, u32 status);

int rtl8125_xdp_xmit(struct net_device *dev, int n, struct xdp_frame **frames,
                     u32 flags);

/* Release what a non-skb TX descriptor completes */
static inline void rtl8125_xdp_tx_release(struct ring_info *ri,
                                          struct rtl8125_tx_done *done)
{
        if (!done->teardown) {
                done->packets++;
                done->bytes += ri->bytecount;
        }

        switch (ri->type) {
        case RTL_TX_XDP:
                xdp_return_frame(ri->xdpf);
                break;
        }
}

/* An attached program wants XDP_PACKET_HEADROOM in front of every frame */
static inline unsigned int rtl8125_xdp_headroom(struct rtl8125_private *tp)
{
        return tp->xdp_prog ? XDP_PACKET_HEADROOM + R8125_RX_ALIGN : R8125_RX_ALIGN;
}

static inline void rtl8125_xdp_skb_meta(struct sk_buff *skb,
                                        const struct rtl8125_rx_frame *frame)
{
        if (frame->metasize)
                skb_metadata_set(skb, frame->metasize);
}

#else /* ENABLE_XDP_SUPPORT */

static inline int rtl8125_xdp_init(struct rtl8125_private *tp)
{
        return 0;
}

static inline void rtl8125_xdp_exit(struct rtl8125_private *tp) {}

static inline int rtl8125_xdp_rxq_reg(struct rtl8125_private *tp,
                                      struct rtl8125_rx_ring *ring, u32 q)
{
        return 0;
}

static inline void rtl8125_xdp_rxq_unreg(struct rtl8125_rx_ring *ring) {}
static inline void rtl8125_xdp_napi_enabled(struct rtl8125_private *tp) {}

static inline int rtl8125_xdp_check_mtu(struct rtl8125_private *tp, int mtu)
{
        return 0;
}

#ifdef ENABLE_PAGE_REUSE
static inline bool rtl8125_xdp_rx(struct rtl8125_private *tp,
                                  struct rtl8125_rx_ring *ring,
                                  u32 cur_rx, struct rtl8125_rx_buffer *rxb,
                                  struct rtl8125_rx_frame *frame, u32 *status)
{
        return false;
}
#endif

static inline void rtl8125_xdp_finalize(struct rtl8125_private *tp, u32 status) {}

static inline void rtl8125_xdp_tx_release(struct ring_info *ri,
                                          struct rtl8125_tx_done *done) {}

static inline unsigned int rtl8125_xdp_headroom(struct rtl8125_private *tp)
{
        return R8125_RX_ALIGN;
}

static inline void rtl8125_xdp_skb_meta(struct sk_buff *skb,
                                        const struct rtl8125_rx_frame *frame) {}

#endif /* ENABLE_XDP_SUPPORT */

#endif /* _LINUX_R8125_XDP_H */
