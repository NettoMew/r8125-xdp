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

#define RTL8125_XSK_DMA_ATTRS   (DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_WEAK_ORDERING)
#define RTL8125_XSK_FILL_BATCH  32

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
                                NETDEV_XDP_ACT_NDO_XMIT | NETDEV_XDP_ACT_XSK_ZEROCOPY;

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

/* A zero-copy ring hands the hardware chunks of the pool's frame size */
static bool rtl8125_xsk_fits(struct xsk_buff_pool *pool, unsigned int mtu)
{
        return xsk_pool_get_rx_frame_size(pool) >= rtl8125_mtu_to_rms(mtu);
}

int rtl8125_xdp_check_mtu(struct rtl8125_private *tp, int mtu)
{
        int i;

        if (tp->xdp_prog && !rtl8125_xdp_fits(mtu))
                return -EINVAL;

        for (i = 0; i < tp->num_rx_rings; i++)
                if (tp->rx_ring[i].xsk_pool &&
                    !rtl8125_xsk_fits(tp->rx_ring[i].xsk_pool, mtu))
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

struct rtl8125_xsk_binding {
        struct xsk_buff_pool *pool;
        u16 qid;
};

static void rtl8125_xsk_set_pool(struct rtl8125_private *tp, void *arg)
{
        const struct rtl8125_xsk_binding *b = arg;

        tp->rx_ring[b->qid].xsk_pool = b->pool;
        tp->tx_ring[b->qid].xsk_pool = b->pool;
}

/*
 * Bind or unbind an XSK pool. Without a program the pool is only recorded,
 * and becomes live at the next ring rebuild that finds both. Unbinding must
 * not fail: the core has already decided.
 */
static int rtl8125_xsk_pool_setup(struct net_device *dev,
                                  struct xsk_buff_pool *pool, u16 qid)
{
        struct rtl8125_private *tp = netdev_priv(dev);
        struct rtl8125_xsk_binding b = { .pool = pool, .qid = qid };
        bool live = netif_running(dev) && tp->xdp_prog;
        int err;

        if (qid >= tp->num_rx_rings || qid >= tp->num_tx_rings)
                return -EINVAL;

        if (!pool) {
                pool = tp->rx_ring[qid].xsk_pool;
                if (!pool)
                        return -EINVAL;

                if (live) {
                        err = rtl8125_rings_rebuild(dev, rtl8125_xsk_set_pool, &b);
                        if (err)
                                netdev_err(dev, "Failed to rebuild rings after XSK unbind on queue %u: %d\n",
                                           qid, err);
                } else {
                        rtl8125_xsk_set_pool(tp, &b);
                }

                xsk_pool_dma_unmap(pool, RTL8125_XSK_DMA_ATTRS);
                return 0;
        }

        if (!rtl8125_xsk_fits(pool, dev->mtu))
                return -EINVAL;

        err = xsk_pool_dma_map(pool, &tp->pci_dev->dev, RTL8125_XSK_DMA_ATTRS);
        if (err)
                return err;

        if (!live) {
                rtl8125_xsk_set_pool(tp, &b);
                return 0;
        }

        err = rtl8125_rings_rebuild(dev, rtl8125_xsk_set_pool, &b);
        if (err) {
                int rollback_err;

                b.pool = NULL;
                rollback_err = rtl8125_rings_rebuild(dev, rtl8125_xsk_set_pool, &b);
                if (rollback_err)
                        netdev_err(dev, "Failed to rebuild rings after XSK bind rollback on queue %u: %d\n",
                                   qid, rollback_err);
                xsk_pool_dma_unmap(pool, RTL8125_XSK_DMA_ATTRS);
                return err;
        }

        /* Start receiving without waiting for an interrupt */
        rtl8125_xsk_wakeup(dev, qid, XDP_WAKEUP_RX);

        return 0;
}

int rtl8125_xdp_bpf(struct net_device *dev, struct netdev_bpf *bpf)
{
        switch (bpf->command) {
        case XDP_SETUP_PROG:
                return rtl8125_xdp_setup_prog(dev, bpf->prog, bpf->extack);
        case XDP_SETUP_XSK_POOL:
                return rtl8125_xsk_pool_setup(dev, bpf->xsk.pool, bpf->xsk.queue_id);
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
 * XDP shares the TX queue lock with the stack. Callers check the carrier
 * with the lock held: the down path drops the carrier before
 * netif_tx_disable() takes every queue lock, so whoever wins the lock
 * afterwards sees the carrier off and leaves a ring being torn down alone.
 */
static void rtl8125_xdp_tx_lock(struct rtl8125_tx_ring *ring)
        __acquires(&txring_txq(ring)->_xmit_lock)
{
        __netif_tx_lock(txring_txq(ring), smp_processor_id());
}

static void rtl8125_xdp_tx_unlock(struct rtl8125_tx_ring *ring)
        __releases(&txring_txq(ring)->_xmit_lock)
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
        rtl8125_xdp_tx_lock(ring);

        if (unlikely(!netif_carrier_ok(dev))) {
                rtl8125_xdp_tx_unlock(ring);
                return -ENETDOWN;
        }

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

        rtl8125_xdp_tx_lock(ring);
        if (likely(netif_carrier_ok(tp->dev)))
                err = rtl8125_xdp_xmit_frame(tp, ring, xdpf);
        rtl8125_xdp_tx_unlock(ring);

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

/*
 * Pick the buffer source of a ring as init_ring refills it: an XSK pool
 * when one is bound and a program runs, pages otherwise. Registering a
 * memory model without an allocator cannot fail on a registered queue.
 */
void rtl8125_xdp_ring_setup(struct rtl8125_private *tp, struct rtl8125_rx_ring *ring)
{
        ring->zc = ring->xsk_pool && tp->xdp_prog;

        xdp_rxq_info_unreg_mem_model(&ring->xdp_rxq);
        if (ring->zc) {
                xdp_rxq_info_reg_mem_model(&ring->xdp_rxq, MEM_TYPE_XSK_BUFF_POOL, NULL);
                xsk_pool_set_rxq_info(ring->xsk_pool, &ring->xdp_rxq);
        } else {
                xdp_rxq_info_reg_mem_model(&ring->xdp_rxq, MEM_TYPE_PAGE_SHARED, NULL);
        }
}

/*
 * Arm the empty slots from start to end with pool buffers, as far as the
 * fill queue allows, and return how many were armed. With need_wakeup the
 * application learns that it has to refill and kick.
 */
u32 rtl8125_xsk_rx_fill(struct rtl8125_private *tp, struct rtl8125_rx_ring *ring,
                        u32 start, u32 end)
{
        struct xsk_buff_pool *pool = ring->xsk_pool;
        u32 cur = start;

        while (end - cur > 0) {
                struct xdp_buff *bufs[RTL8125_XSK_FILL_BATCH];
                u32 i, n;

                n = xsk_buff_alloc_batch(pool, bufs,
                                         min_t(u32, end - cur, RTL8125_XSK_FILL_BATCH));
                if (!n)
                        break;

                for (i = 0; i < n; i++, cur++) {
                        u32 entry = cur % ring->num_rx_desc;

                        ring->rx_buffer[entry].xdp = bufs[i];
                        rtl8125_rx_arm(tp, ring, entry, xsk_buff_xdp_get_dma(bufs[i]));
                }
        }

        if (xsk_uses_need_wakeup(pool)) {
                if (cur != end)
                        xsk_set_rx_need_wakeup(pool);
                else
                        xsk_clear_rx_need_wakeup(pool);
        }

        return cur - start;
}

/* Return every buffer to the pool; NAPI is stopped */
void rtl8125_xsk_rx_clear(struct rtl8125_rx_ring *ring)
{
        int i;

        for (i = 0; i < ring->num_rx_desc; i++)
                if (ring->rx_buffer[i].xdp)
                        rtl8125_xsk_rx_free(&ring->rx_buffer[i]);
}

/* XDP_PASS on a zero-copy ring: the stack gets a copy, the pool its buffer */
static struct sk_buff *rtl8125_xsk_construct_skb(struct rtl8125_private *tp,
                                                 struct rtl8125_rx_ring *ring,
                                                 struct xdp_buff *xdp)
{
        unsigned int metasize = xdp->data - xdp->data_meta;
        unsigned int len = xdp->data_end - xdp->data_meta;
        struct sk_buff *skb;

        skb = napi_alloc_skb(rtl8125_q_napi(tp, ring->index, false), len);
        if (unlikely(!skb))
                return NULL;

        memcpy(__skb_put(skb, len), xdp->data_meta, len);
        if (metasize) {
                __skb_pull(skb, metasize);
                skb_metadata_set(skb, metasize);
        }

        return skb;
}

/*
 * Run the program on a frame in a zero-copy buffer. Returns the skb for
 * XDP_PASS, NULL once the buffer has been dealt with. The slot is left
 * empty either way and refilled from the pool at the end of the poll.
 */
struct sk_buff *rtl8125_xsk_rx(struct rtl8125_private *tp, struct rtl8125_rx_ring *ring,
                               struct rtl8125_rx_buffer *rxb, u32 len, u32 *status)
{
        struct bpf_prog *prog = READ_ONCE(tp->xdp_prog);
        struct xdp_buff *xdp = rxb->xdp;
        struct net_device *dev = tp->dev;
        struct sk_buff *skb;
        u32 act;

        rxb->xdp = NULL;
        xsk_buff_set_size(xdp, len);
        rtl8125_xsk_sync_for_cpu(xdp, ring->xsk_pool);

        act = bpf_prog_run_xdp(prog, xdp);
        switch (act) {
        case XDP_REDIRECT:
                if (likely(!xdp_do_redirect(dev, xdp, prog))) {
                        *status |= RTL8125_XDP_REDIR;
                        return NULL;
                }
                trace_xdp_exception(dev, prog, act);
                break;
        case XDP_TX:
                /* Copied into a page of its own, the buffer is already back */
                if (likely(rtl8125_xdp_tx_back(tp, xdp)))
                        *status |= RTL8125_XDP_TX;
                else
                        trace_xdp_exception(dev, prog, act);
                return NULL;
        case XDP_PASS:
                skb = rtl8125_xsk_construct_skb(tp, ring, xdp);
                xsk_buff_free(xdp);
                return skb;
        default:
                bpf_warn_invalid_xdp_action(dev, prog, act);
                fallthrough;
        case XDP_ABORTED:
                trace_xdp_exception(dev, prog, act);
                fallthrough;
        case XDP_DROP:
                break;
        }

        xsk_buff_free(xdp);
        return NULL;
}

static void rtl8125_napi_kick(struct napi_struct *napi)
{
        if (napi_if_scheduled_mark_missed(napi))
                return;

        local_bh_disable();
        napi_schedule(napi);
        local_bh_enable();
}

int rtl8125_xsk_wakeup(struct net_device *dev, u32 qid, u32 flags)
{
        struct rtl8125_private *tp = netdev_priv(dev);

        if (!netif_running(dev) || !netif_carrier_ok(dev))
                return -ENETDOWN;

        if (qid >= tp->num_rx_rings || qid >= tp->num_tx_rings ||
            !READ_ONCE(tp->rx_ring[qid].zc))
                return -EINVAL;

        if (flags & XDP_WAKEUP_RX)
                rtl8125_napi_kick(rtl8125_q_napi(tp, qid, false));
        if (flags & XDP_WAKEUP_TX)
                rtl8125_napi_kick(rtl8125_q_napi(tp, qid, true));

        return 0;
}

void rtl8125_xdp_finalize(struct rtl8125_private *tp, u32 status)
{
        if (status & RTL8125_XDP_REDIR)
                xdp_do_flush();

        if (status & RTL8125_XDP_TX) {
                struct rtl8125_tx_ring *ring = rtl8125_xdp_tx_ring(tp);

                rtl8125_xdp_tx_lock(ring);
                if (netif_carrier_ok(tp->dev))
                        rtl8125_doorbell(tp, ring);
                rtl8125_xdp_tx_unlock(ring);
        }
}

/*
 * Send what an AF_XDP socket queued on a zero-copy queue, as much as half
 * the free descriptors take, since a frame may need a padding descriptor.
 * TX work does not count against the NAPI budget, so a single wakeup can
 * fill the ring. The frames go out straight from the UMEM and complete to
 * the pool in rtl8125_xdp_tx_done().
 */
void rtl8125_xsk_xmit(struct rtl8125_tx_ring *ring)
{
        struct rtl8125_private *tp = ring->priv;
        struct xsk_buff_pool *pool = ring->xsk_pool;
        struct xdp_desc *descs;
        u32 i, n;

        if (!pool || !tp->rx_ring[ring->index].zc)
                return;

        rtl8125_xdp_tx_lock(ring);
        if (unlikely(!netif_carrier_ok(tp->dev))) {
                rtl8125_xdp_tx_unlock(ring);
                return;
        }

        n = xsk_tx_peek_release_desc_batch(pool, rtl8125_tx_free(ring) / 2);
        descs = pool->tx_descs;
        for (i = 0; i < n; i++) {
                dma_addr_t dma = xsk_buff_raw_get_dma(pool, descs[i].addr);
                void *data = xsk_buff_raw_get_data(pool, descs[i].addr);
                u32 len = descs[i].len;

                xsk_buff_raw_dma_sync_for_device(pool, dma, len);
                rtl8125_xdp_post(tp, ring, dma, len, rtl8125_xdp_pad_len(tp, data, len),
                                 false, RTL_TX_XSK, NULL);
        }

        if (n) {
                rtl8125_doorbell(tp, ring);
                rtl8125_xdp_tx_maybe_stop(tp, ring);
        }

        if (xsk_uses_need_wakeup(pool))
                xsk_set_tx_need_wakeup(pool);

        rtl8125_xdp_tx_unlock(ring);
}
