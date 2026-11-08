// SPDX-License-Identifier: GPL-2.0-only
/*
 * xsktest: AF_XDP traffic for exercising the r8125 zero-copy paths.
 *
 *   rx      receive the test frames and check their IPv4 and UDP checksums
 *   l2fwd   send every test frame back with the MAC addresses swapped
 *   tx      send UDP frames of a given length
 *
 * Only IPv4 UDP frames to ports 9, 319 and 320 reach the socket (see
 * r8125_test.bpf.c); everything else on the link passes to the stack.
 *
 * Build: gcc -O2 -Wall -o xsktest xsktest.c -lxdp -lbpf
 */

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <linux/if_link.h>
#include <linux/if_xdp.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <xdp/xsk.h>

#define NUM_FRAMES      4096
#define FRAME_SIZE      2048    /* 1792 bytes of payload room behind the headroom */
#define RING_SIZE       2048
#define BATCH           64

enum mode { MODE_RX, MODE_L2FWD, MODE_TX };

static struct {
        const char *ifname;
        const char *obj;
        enum mode mode;
        unsigned int queue;
        bool copy;
        bool wakeup;
        bool bind_first;        /* create the socket before attaching the program */
        unsigned int seconds;
        unsigned long count;    /* frames to send in tx mode, 0 for no limit */
        unsigned int len;       /* frame length in tx mode, without FCS */
        unsigned int dport;
        uint8_t dmac[ETH_ALEN];
        struct in_addr saddr, daddr;
} cfg = {
        .obj = "r8125_test.bpf.o",
        .wakeup = true,
        .seconds = 10,
        .len = 60,
        .dport = 9,
};

struct stats {
        unsigned long rx, rx_bad, tx, tx_done;
};

static struct xsk_ring_prod fq, tx;
static struct xsk_ring_cons cq, rx;
static struct xsk_socket *xsk;
static struct xsk_umem *umem;
static void *buffer;
static uint64_t free_frames[NUM_FRAMES];
static unsigned int nr_free;
static volatile sig_atomic_t stop;

static void die(const char *what, int err)
{
        fprintf(stderr, "xsktest: %s: %s\n", what, strerror(err < 0 ? -err : err));
        exit(1);
}

static void on_signal(int sig)
{
        (void)sig;
        stop = 1;
}

static double now(void)
{
        struct timespec ts;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec + ts.tv_nsec / 1e9;
}

static uint16_t csum_fold(uint32_t sum)
{
        while (sum >> 16)
                sum = (sum & 0xffff) + (sum >> 16);
        return ~sum;
}

static uint32_t csum_add(uint32_t sum, const void *data, size_t len)
{
        const uint8_t *p = data;

        for (; len > 1; len -= 2, p += 2)
                sum += (p[0] << 8) | p[1];
        if (len)
                sum += p[0] << 8;
        return sum;
}

/* Check the IPv4 header checksum and, when present, the UDP checksum */
static bool frame_ok(const uint8_t *frame, uint32_t len)
{
        const struct iphdr *iph = (const void *)(frame + ETH_HLEN);
        const struct udphdr *udph = (const void *)(iph + 1);
        uint32_t ulen, sum;

        if (len < ETH_HLEN + sizeof(*iph) + sizeof(*udph))
                return false;
        if (csum_fold(csum_add(0, iph, sizeof(*iph))) != 0)
                return false;

        ulen = ntohs(udph->len);
        if (ETH_HLEN + sizeof(*iph) + ulen > len)
                return false;
        if (!udph->check)
                return true;

        sum = csum_add(0, &iph->saddr, 8);
        sum += IPPROTO_UDP + ulen;
        sum = csum_add(sum, udph, ulen);
        return csum_fold(sum) == 0;
}

/* An IPv4 UDP frame of cfg.len bytes with valid checksums */
static void build_frame(uint8_t *frame, const uint8_t *smac, uint32_t seq)
{
        struct ether_header *eth = (void *)frame;
        struct iphdr *iph = (void *)(eth + 1);
        struct udphdr *udph = (void *)(iph + 1);
        uint32_t ulen = cfg.len - ETH_HLEN - sizeof(*iph);
        uint8_t *payload = (uint8_t *)(udph + 1);
        uint32_t i, sum;

        memcpy(eth->ether_dhost, cfg.dmac, ETH_ALEN);
        memcpy(eth->ether_shost, smac, ETH_ALEN);
        eth->ether_type = htons(ETHERTYPE_IP);

        memset(iph, 0, sizeof(*iph));
        iph->version = 4;
        iph->ihl = 5;
        iph->ttl = 64;
        iph->protocol = IPPROTO_UDP;
        iph->tot_len = htons(sizeof(*iph) + ulen);
        iph->saddr = cfg.saddr.s_addr;
        iph->daddr = cfg.daddr.s_addr;
        iph->check = htons(csum_fold(csum_add(0, iph, sizeof(*iph))));

        udph->source = htons(40000);
        udph->dest = htons(cfg.dport);
        udph->len = htons(ulen);
        udph->check = 0;
        for (i = 0; i < ulen - sizeof(*udph); i++)
                payload[i] = (seq + i) & 0xff;

        sum = csum_add(0, &iph->saddr, 8);
        sum += IPPROTO_UDP + ulen;
        sum = csum_add(sum, udph, ulen);
        udph->check = htons(csum_fold(sum) ?: 0xffff);
}

static void kick_tx(void)
{
        if (cfg.copy || !cfg.wakeup || xsk_ring_prod__needs_wakeup(&tx))
                sendto(xsk_socket__fd(xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);
}

static void kick_rx(void)
{
        if (cfg.wakeup && xsk_ring_prod__needs_wakeup(&fq))
                recvfrom(xsk_socket__fd(xsk), NULL, 0, MSG_DONTWAIT, NULL, NULL);
}

/* Refill the fill ring from the free list */
static void refill(void)
{
        unsigned int n, i;
        uint32_t idx;

        n = xsk_prod_nb_free(&fq, nr_free);
        if (n > nr_free)
                n = nr_free;
        if (!n || xsk_ring_prod__reserve(&fq, n, &idx) != n)
                return;
        for (i = 0; i < n; i++)
                *xsk_ring_prod__fill_addr(&fq, idx + i) = free_frames[--nr_free];
        xsk_ring_prod__submit(&fq, n);
}

/* Move completed TX frames back to the free list */
static void reap(struct stats *st)
{
        unsigned int n, i;
        uint32_t idx;

        n = xsk_ring_cons__peek(&cq, BATCH, &idx);
        for (i = 0; i < n; i++)
                free_frames[nr_free++] = *xsk_ring_cons__comp_addr(&cq, idx + i);
        xsk_ring_cons__release(&cq, n);
        st->tx_done += n;
}

static void do_rx(struct stats *st)
{
        unsigned int n, i;
        uint32_t idx_rx, idx_tx = 0;

        kick_rx();
        n = xsk_ring_cons__peek(&rx, BATCH, &idx_rx);
        if (!n)
                return;

        if (cfg.mode == MODE_L2FWD)
                while (xsk_ring_prod__reserve(&tx, n, &idx_tx) != n) {
                        reap(st);
                        kick_tx();
                        if (stop)
                                return;
                }

        for (i = 0; i < n; i++) {
                const struct xdp_desc *d = xsk_ring_cons__rx_desc(&rx, idx_rx + i);
                uint8_t *frame = xsk_umem__get_data(buffer, d->addr);

                if (!frame_ok(frame, d->len)) {
                        if (!st->rx_bad++) {
                                unsigned int k;

                                printf("  first bad frame: len %u addr 0x%llx:", d->len,
                                       (unsigned long long)d->addr);
                                for (k = 0; k < 64 && k < d->len; k++)
                                        printf("%s%02x", k % 16 ? " " : "\n    ", frame[k]);
                                printf("\n");
                        }
                }

                if (cfg.mode == MODE_L2FWD) {
                        struct xdp_desc *t = xsk_ring_prod__tx_desc(&tx, idx_tx + i);
                        uint8_t mac[ETH_ALEN];

                        memcpy(mac, frame, ETH_ALEN);
                        memcpy(frame, frame + ETH_ALEN, ETH_ALEN);
                        memcpy(frame + ETH_ALEN, mac, ETH_ALEN);
                        t->addr = d->addr;
                        t->len = d->len;
                } else {
                        free_frames[nr_free++] = d->addr;
                }
        }
        xsk_ring_cons__release(&rx, n);
        st->rx += n;

        if (cfg.mode == MODE_L2FWD) {
                xsk_ring_prod__submit(&tx, n);
                st->tx += n;
                kick_tx();
                reap(st);
        }
        refill();
}

/* Frames are built once, up front; sending only posts descriptors */
static void do_tx(struct stats *st)
{
        unsigned int n, i;
        uint32_t idx;

        reap(st);
        n = nr_free < BATCH ? nr_free : BATCH;
        if (cfg.count && st->tx + n > cfg.count)
                n = cfg.count - st->tx;
        if (n && xsk_ring_prod__reserve(&tx, n, &idx) == n) {
                for (i = 0; i < n; i++) {
                        struct xdp_desc *t = xsk_ring_prod__tx_desc(&tx, idx + i);

                        t->addr = free_frames[--nr_free];
                        t->len = cfg.len;
                }
                xsk_ring_prod__submit(&tx, n);
                st->tx += n;
        }
        kick_tx();
}

static int attach_program(int ifindex, int *map_fd)
{
        struct bpf_object *obj = bpf_object__open_file(cfg.obj, NULL);
        struct bpf_program *prog;
        int err;

        if (!obj)
                die("bpf_object__open_file", errno);
        err = bpf_object__load(obj);
        if (err)
                die("bpf_object__load", err);

        prog = bpf_object__find_program_by_name(obj, "xdp_xsk");
        *map_fd = bpf_object__find_map_fd_by_name(obj, "xsks");
        if (!prog || *map_fd < 0)
                die("finding xdp_xsk and xsks", ENOENT);

        err = bpf_xdp_attach(ifindex, bpf_program__fd(prog), XDP_FLAGS_DRV_MODE, NULL);
        if (err)
                die("bpf_xdp_attach (native mode)", err);
        return 0;
}

static int create_socket(struct xdp_options *opts)
{
        struct xsk_socket_config xcfg = {
                .rx_size = RING_SIZE,
                .tx_size = RING_SIZE,
                .libxdp_flags = XSK_LIBXDP_FLAGS__INHIBIT_PROG_LOAD,
                .bind_flags = (cfg.copy ? XDP_COPY : XDP_ZEROCOPY) |
                              (cfg.wakeup ? XDP_USE_NEED_WAKEUP : 0),
        };
        socklen_t optlen = sizeof(*opts);
        bool zerocopy;
        int err;

        err = xsk_socket__create(&xsk, cfg.ifname, cfg.queue, umem, &rx, &tx, &xcfg);
        if (err) {
                fprintf(stderr, "xsktest: xsk_socket__create (%s): %s\n",
                        cfg.copy ? "copy" : "zero-copy", strerror(-err));
                return -1;
        }
        if (getsockopt(xsk_socket__fd(xsk), SOL_XDP, XDP_OPTIONS,
                       opts, &optlen)) {
                fprintf(stderr, "xsktest: getsockopt(XDP_OPTIONS): %s\n", strerror(errno));
                return -1;
        }
        if (optlen != sizeof(*opts)) {
                fprintf(stderr, "xsktest: XDP_OPTIONS size %u, expected %zu\n",
                        (unsigned int)optlen, sizeof(*opts));
                return -1;
        }
        zerocopy = !!(opts->flags & XDP_OPTIONS_ZEROCOPY);
        if (zerocopy != !cfg.copy) {
                fprintf(stderr, "xsktest: requested %s, negotiated %s (XDP_OPTIONS flags 0x%x)\n",
                        cfg.copy ? "copy" : "zero-copy", zerocopy ? "zero-copy" : "copy",
                        opts->flags);
                return -1;
        }
        return 0;
}

static void usage(void)
{
        fprintf(stderr,
                "usage: xsktest -i IFACE -m rx|l2fwd|tx [options]\n"
                "  -q QUEUE     queue to bind (0)\n"
                "  -c           copy mode instead of zero-copy\n"
                "  -w           no need_wakeup\n"
                "  -b           bind the socket before attaching the program\n"
                "  -t SECONDS   run time (10)\n"
                "  -o FILE      BPF object (r8125_test.bpf.o)\n"
                " tx mode:\n"
                "  -D MAC -s SRCIP -d DSTIP  addressing\n"
                "  -l LEN       frame length without FCS (60)\n"
                "  -p PORT      UDP destination port (9)\n"
                "  -N COUNT     frames to send (no limit)\n");
        exit(2);
}

int main(int argc, char **argv)
{
        struct xsk_umem_config ucfg = {
                .fill_size = RING_SIZE,
                .comp_size = RING_SIZE,
                .frame_size = FRAME_SIZE,
                .frame_headroom = 0,
        };
        struct stats st = { 0 }, last = { 0 };
        struct xdp_options opts = { 0 };
        bool attached = false;
        int status = 1;
        double start, tick;
        uint8_t smac[ETH_ALEN] = { 0 };
        int opt, err, ifindex, map_fd;
        unsigned int i;

        while ((opt = getopt(argc, argv, "i:m:q:cwbt:o:D:s:d:l:p:N:")) != -1) {
                switch (opt) {
                case 'i': cfg.ifname = optarg; break;
                case 'm':
                        if (!strcmp(optarg, "rx"))
                                cfg.mode = MODE_RX;
                        else if (!strcmp(optarg, "l2fwd"))
                                cfg.mode = MODE_L2FWD;
                        else if (!strcmp(optarg, "tx"))
                                cfg.mode = MODE_TX;
                        else
                                usage();
                        break;
                case 'q': cfg.queue = atoi(optarg); break;
                case 'c': cfg.copy = true; break;
                case 'w': cfg.wakeup = false; break;
                case 'b': cfg.bind_first = true; break;
                case 't': cfg.seconds = atoi(optarg); break;
                case 'o': cfg.obj = optarg; break;
                case 'D':
                        if (sscanf(optarg, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &cfg.dmac[0],
                                   &cfg.dmac[1], &cfg.dmac[2], &cfg.dmac[3], &cfg.dmac[4],
                                   &cfg.dmac[5]) != 6)
                                usage();
                        break;
                case 's': inet_pton(AF_INET, optarg, &cfg.saddr); break;
                case 'd': inet_pton(AF_INET, optarg, &cfg.daddr); break;
                case 'l': cfg.len = atoi(optarg); break;
                case 'p': cfg.dport = atoi(optarg); break;
                case 'N': cfg.count = strtoul(optarg, NULL, 0); break;
                default: usage();
                }
        }
        if (!cfg.ifname || cfg.len < 42 || cfg.len > 1514)
                usage();

        ifindex = if_nametoindex(cfg.ifname);
        if (!ifindex)
                die(cfg.ifname, errno);
        if (cfg.mode == MODE_TX) {
                char path[64];
                FILE *f;

                snprintf(path, sizeof(path), "/sys/class/net/%s/address", cfg.ifname);
                f = fopen(path, "r");
                if (!f || fscanf(f, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &smac[0], &smac[1],
                                 &smac[2], &smac[3], &smac[4], &smac[5]) != 6)
                        die(path, EINVAL);
                fclose(f);
        }

        buffer = mmap(NULL, (size_t)NUM_FRAMES * FRAME_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (buffer == MAP_FAILED)
                die("mmap", errno);
        err = xsk_umem__create(&umem, buffer, (size_t)NUM_FRAMES * FRAME_SIZE, &fq, &cq, &ucfg);
        if (err)
                die("xsk_umem__create", err);
        for (i = 0; i < NUM_FRAMES; i++) {
                free_frames[nr_free++] = (uint64_t)i * FRAME_SIZE;
                if (cfg.mode == MODE_TX)
                        build_frame(xsk_umem__get_data(buffer, (uint64_t)i * FRAME_SIZE),
                                    smac, i);
        }

        if (cfg.bind_first) {
                if (create_socket(&opts))
                        goto cleanup;
                attach_program(ifindex, &map_fd);
                attached = true;
        } else {
                attach_program(ifindex, &map_fd);
                attached = true;
                if (create_socket(&opts))
                        goto cleanup;
        }
        err = xsk_socket__update_xskmap(xsk, map_fd);
        if (err)
                die("xsk_socket__update_xskmap", err);

        if (cfg.mode != MODE_TX)
                refill();

        signal(SIGINT, on_signal);
        signal(SIGTERM, on_signal);
        start = tick = now();
        printf("%s queue %u, %s, negotiated %s, XDP_OPTIONS flags 0x%x, need_wakeup %s\n", cfg.ifname, cfg.queue,
               cfg.mode == MODE_RX ? "rx" : cfg.mode == MODE_L2FWD ? "l2fwd" : "tx",
               opts.flags & XDP_OPTIONS_ZEROCOPY ? "zero-copy" : "copy",
               opts.flags, cfg.wakeup ? "on" : "off");

        while (!stop && now() - start < cfg.seconds) {
                if (cfg.mode == MODE_TX) {
                        if (cfg.count && st.tx >= cfg.count && st.tx_done >= st.tx)
                                break;
                        do_tx(&st);
                } else {
                        struct pollfd pfd = { .fd = xsk_socket__fd(xsk), .events = POLLIN };

                        do_rx(&st);
                        if (!xsk_cons_nb_avail(&rx, 1))
                                poll(&pfd, 1, 10);
                }

                if (now() - tick >= 1.0) {
                        printf("  rx %8lu/s  tx %8lu/s  bad %lu\n", st.rx - last.rx,
                               st.tx_done - last.tx_done, st.rx_bad);
                        fflush(stdout);
                        last = st;
                        tick = now();
                }
        }

        /* Let the last frames complete */
        for (i = 0; i < 100 && st.tx_done < st.tx; i++) {
                kick_tx();
                reap(&st);
                usleep(10000);
        }

        printf("total rx %lu, bad %lu, tx %lu, completed %lu\n", st.rx, st.rx_bad, st.tx,
               st.tx_done);

        status = st.rx_bad || st.tx_done < st.tx ? 1 : 0;
cleanup:
        if (xsk)
                xsk_socket__delete(xsk);
        xsk_umem__delete(umem);
        if (attached)
                bpf_xdp_detach(ifindex, XDP_FLAGS_DRV_MODE, NULL);
        munmap(buffer, (size_t)NUM_FRAMES * FRAME_SIZE);

        return status;
}
