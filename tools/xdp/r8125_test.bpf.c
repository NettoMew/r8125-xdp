// SPDX-License-Identifier: GPL-2.0-only
/*
 * XDP programs for exercising the r8125 XDP paths.
 *
 * Every program acts only on IPv4 UDP frames to the test ports and passes
 * everything else, so ARP, SSH and the rest of the traffic on the link keep
 * working while a test runs.
 *
 *   xdp_drop       drop the test frames
 *   xdp_tx         swap the MAC addresses and send the frames back (XDP_TX)
 *   xdp_redirect   the same, through bpf_redirect() to the receiving port,
 *                  which goes through ndo_xdp_xmit
 *   xdp_xsk        hand the test frames to the AF_XDP socket of their queue
 *
 * Build: clang -O2 -g -target bpf -c r8125_test.bpf.c -o r8125_test.bpf.o
 */

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

/* discard, PTP event and PTP general */
#define TEST_PORT_DISCARD       9
#define TEST_PORT_PTP_EVENT     319
#define TEST_PORT_PTP_GENERAL   320

struct {
        __uint(type, BPF_MAP_TYPE_XSKMAP);
        __uint(max_entries, 64);
        __type(key, __u32);
        __type(value, __u32);
} xsks SEC(".maps");

static __always_inline int is_test_frame(struct xdp_md *ctx)
{
        void *data_end = (void *)(long)ctx->data_end;
        void *data = (void *)(long)ctx->data;
        struct ethhdr *eth = data;
        struct iphdr *iph = (void *)(eth + 1);
        struct udphdr *udph;
        __u16 dport;

        if ((void *)(iph + 1) > data_end || eth->h_proto != bpf_htons(ETH_P_IP) ||
            iph->protocol != IPPROTO_UDP || iph->ihl != 5)
                return 0;

        udph = (void *)(iph + 1);
        if ((void *)(udph + 1) > data_end)
                return 0;

        dport = bpf_ntohs(udph->dest);
        return dport == TEST_PORT_DISCARD || dport == TEST_PORT_PTP_EVENT ||
               dport == TEST_PORT_PTP_GENERAL;
}

static __always_inline void swap_mac(struct xdp_md *ctx)
{
        struct ethhdr *eth = (void *)(long)ctx->data;
        __u8 tmp[ETH_ALEN];

        __builtin_memcpy(tmp, eth->h_source, ETH_ALEN);
        __builtin_memcpy(eth->h_source, eth->h_dest, ETH_ALEN);
        __builtin_memcpy(eth->h_dest, tmp, ETH_ALEN);
}

SEC("xdp")
int xdp_drop(struct xdp_md *ctx)
{
        return is_test_frame(ctx) ? XDP_DROP : XDP_PASS;
}

SEC("xdp")
int xdp_tx(struct xdp_md *ctx)
{
        if (!is_test_frame(ctx))
                return XDP_PASS;

        swap_mac(ctx);
        return XDP_TX;
}

SEC("xdp")
int xdp_redirect(struct xdp_md *ctx)
{
        if (!is_test_frame(ctx))
                return XDP_PASS;

        swap_mac(ctx);
        return bpf_redirect(ctx->ingress_ifindex, 0);
}

SEC("xdp")
int xdp_xsk(struct xdp_md *ctx)
{
        if (!is_test_frame(ctx))
                return XDP_PASS;

        return bpf_redirect_map(&xsks, ctx->rx_queue_index, XDP_PASS);
}

char _license[] SEC("license") = "GPL";
