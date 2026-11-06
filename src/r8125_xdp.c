// SPDX-License-Identifier: GPL-2.0-only
/*
 * XDP and AF_XDP support for the r8125 driver.
 *
 * The receive side reuses the vendor page split model: every page holds
 * two buffers, and a frame that leaves the driver takes a page reference
 * with it. Zero-copy rings take their buffers from an XSK pool instead.
 * Transmission shares the TX rings, and their lock, with the stack.
 */

#include <linux/netdevice.h>
#include <net/xdp.h>

#include "r8125.h"
#include "r8125_xdp.h"

/*
 * The NAPI context that serves RX or TX queue q. This is the only place
 * that knows how the interrupt versions lay out their vectors.
 */
struct napi_struct *rtl8125_q_napi(struct rtl8125_private *tp, u32 q, bool tx)
{
        unsigned int vec = 0;

        if (tp->features & RTL_FEATURE_MSIX) {
                switch (tp->HwCurrIsrVer) {
                case 2:
                        vec = tx ? 16 + 2 * q : q;
                        break;
                case 3:
                case 4:
                        vec = q;
                        break;
                case 5:
                        vec = tx ? 16 + q : q;
                        break;
                case 7:
                        vec = tx ? 27 + q : q;
                        break;
                }
        }

        return &tp->r8125napi[vec].napi;
}

int rtl8125_xdp_rxq_reg(struct rtl8125_private *tp,
                        struct rtl8125_rx_ring *ring, u32 q)
{
        int err;

        err = xdp_rxq_info_reg(&ring->xdp_rxq, tp->dev, q,
                               rtl8125_q_napi(tp, q, false)->napi_id);
        if (err)
                return err;

        err = xdp_rxq_info_reg_mem_model(&ring->xdp_rxq,
                                         MEM_TYPE_PAGE_SHARED, NULL);
        if (err)
                xdp_rxq_info_unreg(&ring->xdp_rxq);

        return err;
}

void rtl8125_xdp_rxq_unreg(struct rtl8125_rx_ring *ring)
{
        if (xdp_rxq_info_is_reg(&ring->xdp_rxq))
                xdp_rxq_info_unreg(&ring->xdp_rxq);
}

/*
 * Tell the core which NAPI context serves each queue, once NAPI is enabled
 * and its ID is known; newer kernels hand out a new ID on every enable.
 * AF_XDP busy polling reads the ID from the RX queue info before Linux
 * 6.13 and from the queue to NAPI mapping since then. Called with rtnl.
 */
void rtl8125_xdp_napi_enabled(struct rtl8125_private *tp)
{
        int i;

        for (i = 0; i < tp->num_rx_rings; i++) {
                struct napi_struct *napi = rtl8125_q_napi(tp, i, false);

#if LINUX_VERSION_CODE < KERNEL_VERSION(6,13,0)
                tp->rx_ring[i].xdp_rxq.napi_id = napi->napi_id;
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,8,0)
                netif_queue_set_napi(tp->dev, i, NETDEV_QUEUE_TYPE_RX, napi);
#endif
        }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,8,0)
        for (i = 0; i < tp->num_tx_rings; i++)
                netif_queue_set_napi(tp->dev, i, NETDEV_QUEUE_TYPE_TX,
                                     rtl8125_q_napi(tp, i, true));
#endif
}
