// SPDX-License-Identifier: GPL-2.0-only
/*
 * XDP and AF_XDP support for the r8125 driver.
 *
 * The receive side reuses the vendor page split model: every page holds
 * two buffers, and a frame that leaves the driver takes a page reference
 * with it. Zero-copy rings take their buffers from an XSK pool instead.
 * Transmission shares the TX rings, and their lock, with the stack.
 */

#include <linux/bpf_trace.h>
#include <linux/filter.h>
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

int rtl8125_xdp_init(struct rtl8125_private *tp)
{
        tp->dev->xdp_features = NETDEV_XDP_ACT_BASIC | NETDEV_XDP_ACT_REDIRECT;

        return 0;
}

void rtl8125_xdp_exit(struct rtl8125_private *tp)
{
}

/*
 * A program sees each frame in one buffer, half of a page: the frame, the
 * XDP headroom in front and the skb_shared_info behind must fit in a page.
 */
static bool rtl8125_xdp_fits(unsigned int mtu)
{
        return SKB_DATA_ALIGN(rtl8125_mtu_to_rms(mtu) + XDP_PACKET_HEADROOM + R8125_RX_ALIGN) +
               SKB_DATA_ALIGN(sizeof(struct skb_shared_info)) <= PAGE_SIZE;
}

int rtl8125_xdp_check_mtu(struct rtl8125_private *tp, int mtu)
{
        if (tp->xdp_prog && !rtl8125_xdp_fits(mtu))
                return -EINVAL;

        return 0;
}

static void rtl8125_xdp_set_prog(struct rtl8125_private *tp, void *prog)
{
        tp->xdp_prog = prog;
}

static int rtl8125_xdp_setup_prog(struct net_device *dev, struct bpf_prog *prog,
                                  struct netlink_ext_ack *extack)
{
        struct rtl8125_private *tp = netdev_priv(dev);
        struct bpf_prog *old = tp->xdp_prog;
        int err;

        if (prog && !rtl8125_xdp_fits(dev->mtu)) {
                NL_SET_ERR_MSG_MOD(extack, "MTU too large for XDP");
                return -EINVAL;
        }

        if (!old == !prog) {
                /* Replacing one program by another changes nothing else */
                old = xchg(&tp->xdp_prog, prog);
        } else {
                /* Attaching or detaching moves the RX headroom */
                err = rtl8125_rings_rebuild(dev, rtl8125_xdp_set_prog, prog);
                if (err) {
                        tp->xdp_prog = old;
                        NL_SET_ERR_MSG_MOD(extack, "Failed to refill the RX rings");
                        return err;
                }
        }

        if (old)
                bpf_prog_put(old);

        return 0;
}

int rtl8125_xdp_bpf(struct net_device *dev, struct netdev_bpf *bpf)
{
        switch (bpf->command) {
        case XDP_SETUP_PROG:
                return rtl8125_xdp_setup_prog(dev, bpf->prog, bpf->extack);
        default:
                return -EOPNOTSUPP;
        }
}

/*
 * Run the program on a frame in a page reuse buffer. Returns false for
 * XDP_PASS, with frame describing what the stack gets, and true once the
 * buffer has been dealt with.
 *
 * A frame that leaves the driver takes a page reference with it, the way
 * an skb does: rtl8125_put_rx_buffer() takes it and flips the page before
 * the frame is handed on, since the receiver may drop it right away.
 */
bool rtl8125_xdp_rx(struct rtl8125_private *tp, struct rtl8125_rx_ring *ring,
                    u32 cur_rx, struct rtl8125_rx_buffer *rxb,
                    struct rtl8125_rx_frame *frame, u32 *status)
{
        struct bpf_prog *prog = READ_ONCE(tp->xdp_prog);
        struct net_device *dev = tp->dev;
        struct xdp_buff xdp;
        void *hard_start;
        u32 act;

        if (!prog)
                return false;

        hard_start = rxb->data + rxb->page_offset - ring->rx_offset;
        xdp_init_buff(&xdp, tp->rx_buf_page_size / 2, &ring->xdp_rxq);
        xdp_prepare_buff(&xdp, hard_start, ring->rx_offset, frame->len, true);

        act = bpf_prog_run_xdp(prog, &xdp);
        switch (act) {
        case XDP_PASS:
                frame->offset = xdp.data - hard_start;
                frame->len = xdp.data_end - xdp.data;
                frame->metasize = xdp.data - xdp.data_meta;
                return false;
        case XDP_REDIRECT:
                rtl8125_put_rx_buffer(tp, ring, cur_rx, rxb);
                if (!xdp_do_redirect(dev, &xdp, prog)) {
                        *status |= RTL8125_XDP_REDIR;
                        return true;
                }
                xdp_return_buff(&xdp);
                trace_xdp_exception(dev, prog, act);
                return true;
        default:
                bpf_warn_invalid_xdp_action(dev, prog, act);
                fallthrough;
        case XDP_ABORTED:
                trace_xdp_exception(dev, prog, act);
                fallthrough;
        case XDP_DROP:
                rtl8125_recycle_rx_buffer(tp, ring, rxb);
                return true;
        }
}

void rtl8125_xdp_finalize(struct rtl8125_private *tp, u32 status)
{
        if (status & RTL8125_XDP_REDIR)
                xdp_do_flush();
}
