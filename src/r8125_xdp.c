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
#include <linux/if_vlan.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/netdevice.h>
#include <linux/pci.h>
#include <linux/ptp_classify.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <net/xdp.h>

#include "r8125.h"
#include "r8125_xdp.h"

/* The erratum padding of rtl8125_get_patch_pad_len(), MIN_PATCH_LEN there */
#define PTP_PAD_L4_LEN          47
#define PTP_PAD_MAX_FRAME       (128 + PTP_PAD_L4_LEN)

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

/*
 * Only the 8125A and 8125B need padding from the driver: the MAC pads short
 * frames with zeros by itself, but drops short UDP frames to the PTP ports
 * unless their L4 part reaches PTP_PAD_L4_LEN bytes (r8125 phase 0 tests).
 */
int rtl8125_xdp_init(struct rtl8125_private *tp)
{
        if (rtl8125_require_pad_ptp_pkt(tp)) {
                tp->tx_pad_buf = dma_alloc_coherent(&tp->pci_dev->dev,
                                                    SHORT_PACKET_PADDING_BUF_SIZE,
                                                    &tp->tx_pad_dma, GFP_KERNEL);
                if (!tp->tx_pad_buf)
                        return -ENOMEM;
        }

        tp->dev->xdp_features = NETDEV_XDP_ACT_BASIC | NETDEV_XDP_ACT_REDIRECT |
                                NETDEV_XDP_ACT_NDO_XMIT;

        return 0;
}

void rtl8125_xdp_exit(struct rtl8125_private *tp)
{
        if (tp->tx_pad_buf) {
                dma_free_coherent(&tp->pci_dev->dev, SHORT_PACKET_PADDING_BUF_SIZE,
                                  tp->tx_pad_buf, tp->tx_pad_dma);
                tp->tx_pad_buf = NULL;
        }
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
 * Bytes of padding the 8125A/B erratum needs behind a frame, the same rule
 * as rtl8125_get_patch_pad_len() applies to skbs: a UDP frame to a PTP port
 * needs PTP_PAD_L4_LEN bytes of L4 data, and a TCP or UDP header cut short
 * needs completing. Frames without either problem need nothing, since the
 * MAC pads short frames by itself.
 */
static u32 rtl8125_xdp_pad_len(struct rtl8125_private *tp, const u8 *data, u32 len)
{
        __be16 proto;
        u32 off = ETH_HLEN;
        u32 l4len, l4min;
        u8 l4proto;

        if (!tp->tx_pad_buf || len < ETH_HLEN || len >= PTP_PAD_MAX_FRAME)
                return 0;

        proto = ((const struct ethhdr *)data)->h_proto;
        while (proto == htons(ETH_P_8021Q) || proto == htons(ETH_P_8021AD)) {
                if (off + VLAN_HLEN > len)
                        return 0;
                proto = ((const struct vlan_hdr *)(data + off))->h_vlan_encapsulated_proto;
                off += VLAN_HLEN;
        }

        if (proto == htons(ETH_P_IP)) {
                const struct iphdr *iph = (const void *)(data + off);

                if (off + sizeof(*iph) > len)
                        return 0;
                l4proto = iph->protocol;
                off += iph->ihl * 4;
        } else if (proto == htons(ETH_P_IPV6)) {
                const struct ipv6hdr *ip6h = (const void *)(data + off);

                if (off + sizeof(*ip6h) > len)
                        return 0;
                l4proto = ip6h->nexthdr;
                off += sizeof(*ip6h);
        } else {
                return 0;
        }

        if (off > len)
                return 0;
        l4len = len - off;

        if (l4proto == IPPROTO_UDP && l4len > 3 && l4len < PTP_PAD_L4_LEN) {
                u16 dport = ntohs(((const struct udphdr *)(data + off))->dest);

                if (dport == PTP_EV_PORT || dport == PTP_GEN_PORT)
                        return PTP_PAD_L4_LEN - l4len;
        }

        if (l4proto == IPPROTO_TCP)
                l4min = sizeof(struct tcphdr);
        else if (l4proto == IPPROTO_UDP)
                l4min = sizeof(struct udphdr);
        else
                l4min = 0;

        return l4len < l4min ? l4min - l4len : 0;
}

/*
 * Post one frame on a TX ring whose lock the caller holds, with a second
 * descriptor pointing at the shared zero buffer when the erratum asks for
 * padding. As in start_xmit(), the descriptor that carries the mapping
 * records its length, the last one records what the frame completes, and
 * the first descriptor is handed to the hardware last.
 */
static void rtl8125_xdp_post(struct rtl8125_private *tp,
                             struct rtl8125_tx_ring *ring,
                             dma_addr_t dma, u32 len, u32 pad, bool mapped,
                             enum rtl8125_tx_type type, struct xdp_frame *xdpf)
{
        unsigned int first = ring->cur_tx % ring->num_tx_desc;
        unsigned int last = pad ? (first + 1) % ring->num_tx_desc : first;
        struct TxDesc *txd = ring->TxDescArray + first;
        struct ring_info *ri = ring->tx_skb + last;

        ring->tx_skb[first].len = mapped ? len : 0;
        ri->xdpf = xdpf;
        ri->type = type;
        ri->bytecount = len;
        ri->gso_segs = 1;

        if (pad) {
                struct TxDesc *ptxd = ring->TxDescArray + last;

                ptxd->addr = cpu_to_le64(tp->tx_pad_dma);
                ptxd->opts2 = 0;
                wmb();
                ptxd->opts1 = cpu_to_le32(rtl8125_get_txd_opts1(ring, DescOwn | LastFrag,
                                                                pad, last));
        }

        txd->addr = cpu_to_le64(dma);
        txd->opts2 = 0;
        wmb();
        txd->opts1 = cpu_to_le32(rtl8125_get_txd_opts1(ring,
                                                       DescOwn | FirstFrag | (pad ? 0 : LastFrag),
                                                       len, first));

        netdev_tx_sent_queue(txring_txq(ring), len);

        /* The completion path must see the descriptors before cur_tx */
        smp_wmb();
        WRITE_ONCE(ring->cur_tx, ring->cur_tx + (pad ? 2 : 1));
}

static unsigned int rtl8125_tx_free(const struct rtl8125_tx_ring *ring)
{
        return READ_ONCE(ring->dirty_tx) + ring->num_tx_desc - READ_ONCE(ring->cur_tx);
}

/* Stop the queue once the stack could no longer fit a full skb */
static void rtl8125_xdp_tx_maybe_stop(struct rtl8125_private *tp,
                                      struct rtl8125_tx_ring *ring)
{
        if (!rtl8125_tx_slots_avail(tp, ring)) {
                netif_tx_stop_queue(txring_txq(ring));
                smp_mb();
                if (rtl8125_tx_slots_avail(tp, ring))
                        netif_tx_start_queue(txring_txq(ring));
        }
}

/* Map and post an xdp_frame. The caller holds the TX queue lock. */
static int rtl8125_xdp_xmit_frame(struct rtl8125_private *tp,
                                  struct rtl8125_tx_ring *ring,
                                  struct xdp_frame *xdpf)
{
        u32 pad = rtl8125_xdp_pad_len(tp, xdpf->data, xdpf->len);
        dma_addr_t dma;

        if (unlikely(rtl8125_tx_free(ring) < (pad ? 2 : 1)))
                return -EBUSY;

        dma = dma_map_single(&tp->pci_dev->dev, xdpf->data, xdpf->len, DMA_TO_DEVICE);
        if (unlikely(dma_mapping_error(&tp->pci_dev->dev, dma)))
                return -ENOMEM;

        rtl8125_xdp_post(tp, ring, dma, xdpf->len, pad, true, RTL_TX_XDP, xdpf);
        rtl8125_xdp_tx_maybe_stop(tp, ring);

        return 0;
}

/* XDP transmits on the TX ring of the current CPU, shared with the stack */
static struct rtl8125_tx_ring *rtl8125_xdp_tx_ring(struct rtl8125_private *tp)
{
        return &tp->tx_ring[smp_processor_id() % tp->num_tx_rings];
}

/*
 * Lock a TX ring for XDP. The carrier is checked under the lock: the down
 * path drops the carrier before netif_tx_disable() takes every queue lock,
 * so a caller that wins the lock afterwards never touches a ring that is
 * being torn down.
 */
static bool rtl8125_xdp_tx_lock(struct rtl8125_private *tp,
                                struct rtl8125_tx_ring *ring)
{
        struct netdev_queue *txq = txring_txq(ring);

        __netif_tx_lock(txq, smp_processor_id());
        if (likely(netif_carrier_ok(tp->dev)))
                return true;

        __netif_tx_unlock(txq);
        return false;
}

static void rtl8125_xdp_tx_unlock(struct rtl8125_tx_ring *ring)
{
        struct netdev_queue *txq = txring_txq(ring);

        txq_trans_cond_update(txq);
        __netif_tx_unlock(txq);
}

int rtl8125_xdp_xmit(struct net_device *dev, int n, struct xdp_frame **frames,
                     u32 flags)
{
        struct rtl8125_private *tp = netdev_priv(dev);
        struct rtl8125_tx_ring *ring;
        int i;

        if (unlikely(flags & ~XDP_XMIT_FLAGS_MASK))
                return -EINVAL;

        ring = rtl8125_xdp_tx_ring(tp);
        if (unlikely(!rtl8125_xdp_tx_lock(tp, ring)))
                return -ENETDOWN;

        for (i = 0; i < n; i++)
                if (rtl8125_xdp_xmit_frame(tp, ring, frames[i]))
                        break;

        if (i && (flags & XDP_XMIT_FLUSH))
                rtl8125_doorbell(tp, ring);

        rtl8125_xdp_tx_unlock(ring);

        return i;
}

/*
 * XDP_TX: send the frame back out; the doorbell rings once per poll. On
 * failure the frame, and the page reference it holds, is released here.
 */
static bool rtl8125_xdp_tx_back(struct rtl8125_private *tp, struct xdp_buff *xdp)
{
        struct xdp_frame *xdpf = xdp_convert_buff_to_frame(xdp);
        struct rtl8125_tx_ring *ring = rtl8125_xdp_tx_ring(tp);
        int err = -ENETDOWN;

        if (unlikely(!xdpf)) {
                xdp_return_buff(xdp);
                return false;
        }

        if (likely(rtl8125_xdp_tx_lock(tp, ring))) {
                err = rtl8125_xdp_xmit_frame(tp, ring, xdpf);
                rtl8125_xdp_tx_unlock(ring);
        }

        if (unlikely(err)) {
                xdp_return_frame_rx_napi(xdpf);
                return false;
        }

        return true;
}

/*
 * Run the program on a frame in a page reuse buffer. Returns false for
 * XDP_PASS, with frame describing what the stack gets, and true once the
 * buffer has been dealt with.
 *
 * A frame that leaves the driver takes a page reference with it, the way
 * an skb does: rtl8125_put_rx_buffer() takes it and flips the page before
 * the frame is handed on, since the receiver may drop it right away. XDP_TX
 * maps the frame for the TX ring on its own, because a failed page reuse
 * unmaps the RX mapping while the frame may still be in flight.
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
        case XDP_TX:
                rtl8125_put_rx_buffer(tp, ring, cur_rx, rxb);
                if (rtl8125_xdp_tx_back(tp, &xdp)) {
                        *status |= RTL8125_XDP_TX;
                        return true;
                }
                trace_xdp_exception(dev, prog, act);
                return true;
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

        if (status & RTL8125_XDP_TX) {
                struct rtl8125_tx_ring *ring = rtl8125_xdp_tx_ring(tp);

                if (rtl8125_xdp_tx_lock(tp, ring)) {
                        rtl8125_doorbell(tp, ring);
                        rtl8125_xdp_tx_unlock(ring);
                }
        }
}
