#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host regression for XSK setup/teardown and ring-rebuild control flow; no NIC or root required.

Run: python3 tools/xdp/test_xsk_pool_setup.py
RED baseline: python3 tools/xdp/test_xsk_pool_setup.py --revision 8231746

Compiles the unmodified production xsk_binding declaration, xsk_set_pool and
xsk_pool_setup functions extracted from src/r8125_xdp.c. Only external kernel
services/types are stubbed. Rebuild invokes the real mutation callback before
returning an injected error, matching r8125_n.c's rebuild contract. This checks
caller ownership/diagnostics, not real DMA, ring allocation, NAPI or concurrency.
A separate harness extracts the real rtl8125_rings_rebuild with stubbed services,
with and without CONFIG_R8125_NAPI. CFLAGS may enable sanitizers.
No source-text assertions or copied production implementation are used.
Requires Python 3 and a C compiler (CC, default cc); no third-party packages.
"""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


STUBS = r'''
#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned short u16;
#define RTL8125_XSK_DMA_ATTRS 3
#define XDP_WAKEUP_RX 1
struct xsk_buff_pool { bool mapped; };
struct ring { struct xsk_buff_pool *xsk_pool; };
struct pci_dev { int dev; };
struct rtl8125_private {
        struct ring rx_ring[2], tx_ring[2];
        int num_rx_rings, num_tx_rings;
        void *xdp_prog;
        struct pci_dev *pci_dev;
};
struct net_device {
        struct rtl8125_private *tp;
        int mtu;
        bool running;
};
static int errors[2], rebuilds, maps, unmaps, wakes, logs, failures;
static int map_error, fits_calls;
static bool fits, binding;
static struct net_device *current;
static struct xsk_buff_pool *expected_pool;
static char diagnostic[256];
#define CHECK(expr) do { if (!(expr)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); \
        failures++; \
} } while (0)
#define netdev_priv(dev) ((dev)->tp)
#define netif_running(dev) ((dev)->running)

static bool rtl8125_xsk_fits(struct xsk_buff_pool *pool, int mtu)
{
        CHECK(pool == expected_pool && mtu == current->mtu);
        fits_calls++;
        return fits;
}

static int xsk_pool_dma_map(struct xsk_buff_pool *pool, int *dev, int attrs)
{
        CHECK(pool == expected_pool && !pool->mapped);
        CHECK(dev == &current->tp->pci_dev->dev);
        CHECK(attrs == RTL8125_XSK_DMA_ATTRS);
        maps++;
        if (map_error)
                return map_error;
        pool->mapped = true;
        return 0;
}

static void xsk_pool_dma_unmap(struct xsk_buff_pool *pool, int attrs)
{
        CHECK(pool == expected_pool && pool->mapped);
        CHECK(attrs == RTL8125_XSK_DMA_ATTRS);
        CHECK(!current->tp->rx_ring[1].xsk_pool);
        CHECK(!current->tp->tx_ring[1].xsk_pool);
        pool->mapped = false;
        unmaps++;
}

static int rtl8125_rings_rebuild(struct net_device *dev,
        void (*mutate)(struct rtl8125_private *, void *), void *arg)
{
        CHECK(dev == current && expected_pool->mapped);
        if (rebuilds >= 2) {
                fputs("Unexpected extra rebuild\n", stderr);
                exit(2);
        }
        CHECK(dev->tp->rx_ring[1].xsk_pool ==
              ((binding && rebuilds == 0) ? NULL : expected_pool));
        CHECK(dev->tp->tx_ring[1].xsk_pool == dev->tp->rx_ring[1].xsk_pool);
        mutate(dev->tp, arg);
        CHECK(dev->tp->rx_ring[1].xsk_pool ==
              ((binding && rebuilds == 0) ? expected_pool : NULL));
        CHECK(dev->tp->tx_ring[1].xsk_pool == dev->tp->rx_ring[1].xsk_pool);
        return errors[rebuilds++];
}

static void rtl8125_xsk_wakeup(struct net_device *dev, u16 qid, int flags)
{
        CHECK(dev == current && qid == 1 && flags == XDP_WAKEUP_RX);
        CHECK(expected_pool->mapped);
        CHECK(dev->tp->rx_ring[qid].xsk_pool == expected_pool);
        CHECK(dev->tp->tx_ring[qid].xsk_pool == expected_pool);
        CHECK(rebuilds == 1 && !unmaps);
        wakes++;
}

static void __attribute__((unused, format(printf, 2, 3)))
netdev_err(struct net_device *dev, const char *format, ...)
{
        va_list args;

        CHECK(dev == current);
        va_start(args, format);
        vsnprintf(diagnostic, sizeof(diagnostic), format, args);
        va_end(args);
        logs++;
}
'''

TESTS = r'''
int main(void)
{
        static const struct {
                const char *name;
                bool bind, running, program, missing, reject_mtu;
                int rx_count, tx_count, map_error, first_error, rollback_error;
                int result, rebuilds, maps, unmaps, wakes, log_error;
                bool attached;
        } cases[] = {
#define BASE .rx_count = 2, .tx_count = 2
#define LIVE .running = true, .program = true
                {"live unbind", BASE, LIVE, .rebuilds = 1, .unmaps = 1},
                {"failed unbind", BASE, LIVE, .first_error = -ENOMEM,
                 .rebuilds = 1, .unmaps = 1, .log_error = -ENOMEM},
                {"down unbind", BASE, .program = true, .unmaps = 1},
                {"no-program unbind", BASE, .running = true, .unmaps = 1},
                {"bind rollback", BASE, LIVE, .bind = true, .first_error = -ENOMEM,
                 .result = -ENOMEM, .rebuilds = 2, .maps = 1, .unmaps = 1},
                {"failed bind rollback", BASE, LIVE, .bind = true,
                 .first_error = -ENOMEM, .rollback_error = -EIO, .result = -ENOMEM,
                 .rebuilds = 2, .maps = 1, .unmaps = 1, .log_error = -EIO},
                {"live bind", BASE, LIVE, .bind = true, .rebuilds = 1,
                 .maps = 1, .wakes = 1, .attached = true},
                {"down bind", BASE, .program = true, .bind = true,
                 .maps = 1, .attached = true},
                {"no-program bind", BASE, .running = true, .bind = true,
                 .maps = 1, .attached = true},
                {"down no-program bind", BASE, .bind = true,
                 .maps = 1, .attached = true},
                {"invalid RX bind", LIVE, .rx_count = 1, .tx_count = 2,
                 .bind = true, .result = -EINVAL},
                {"invalid TX bind", LIVE, .rx_count = 2, .tx_count = 1,
                 .bind = true, .result = -EINVAL},
                {"invalid RX unbind", LIVE, .rx_count = 1, .tx_count = 2,
                 .result = -EINVAL, .attached = true},
                {"invalid TX unbind", LIVE, .rx_count = 2, .tx_count = 1,
                 .result = -EINVAL, .attached = true},
                {"missing pool", BASE, LIVE, .missing = true, .result = -EINVAL},
                {"DMA map failure", BASE, LIVE, .bind = true, .map_error = -EIO,
                 .result = -EIO, .maps = 1},
                {"MTU rejection", BASE, LIVE, .bind = true, .reject_mtu = true,
                 .result = -EINVAL},
        };
        unsigned int i;

        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
                struct pci_dev pci = {0};
                struct xsk_buff_pool pool = {.mapped = !cases[i].bind && !cases[i].missing};
                struct xsk_buff_pool other = {.mapped = true};
                struct rtl8125_private tp = {
                        .num_rx_rings = cases[i].rx_count,
                        .num_tx_rings = cases[i].tx_count,
                        .xdp_prog = cases[i].program ? &pci : NULL,
                        .pci_dev = &pci,
                };
                struct net_device dev = {
                        .tp = &tp, .running = cases[i].running, .mtu = 1500,
                };
                int before = failures;
                char error_text[32];

                current = &dev;
                expected_pool = &pool;
                binding = cases[i].bind;
                fits = !cases[i].reject_mtu;
                map_error = cases[i].map_error;
                rebuilds = maps = unmaps = wakes = logs = fits_calls = 0;
                diagnostic[0] = '\0';
                errors[0] = cases[i].first_error;
                errors[1] = cases[i].rollback_error;
                tp.rx_ring[0].xsk_pool = tp.tx_ring[0].xsk_pool = &other;
                if (pool.mapped)
                        tp.rx_ring[1].xsk_pool = tp.tx_ring[1].xsk_pool = &pool;

                CHECK(rtl8125_xsk_pool_setup(&dev, binding ? &pool : NULL, 1)
                      == cases[i].result);
                CHECK(rebuilds == cases[i].rebuilds);
                CHECK(maps == cases[i].maps && unmaps == cases[i].unmaps);
                CHECK(wakes == cases[i].wakes);
                CHECK(pool.mapped == cases[i].attached);
                CHECK(tp.rx_ring[1].xsk_pool == (cases[i].attached ? &pool : NULL));
                CHECK(tp.tx_ring[1].xsk_pool == tp.rx_ring[1].xsk_pool);
                CHECK(tp.rx_ring[0].xsk_pool == &other && tp.tx_ring[0].xsk_pool == &other);
                CHECK(other.mapped);
                CHECK(fits_calls == (binding && cases[i].rx_count > 1 && cases[i].tx_count > 1));
                printf("%s: ownership/return/wakeup %s\n", cases[i].name,
                       failures == before ? "PASS" : "FAIL");

                before = failures;
                CHECK(logs == !!cases[i].log_error);
                if (cases[i].log_error && logs) {
                        snprintf(error_text, sizeof(error_text), "%d", cases[i].log_error);
                        CHECK(strstr(diagnostic, "queue 1") != NULL);
                        CHECK(strstr(diagnostic, error_text) != NULL);
                        CHECK(strstr(diagnostic, binding ? "rollback" : "unbind") != NULL);
                }
                printf("%s: diagnostic %s\n", cases[i].name,
                       failures == before ? "PASS" : "FAIL");
        }
        printf("setup: %zu cases, %d failed assertions\n",
               sizeof(cases) / sizeof(cases[0]), failures);
        return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
'''

REBUILD_STUBS = r'''
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct net_device;
struct rtl8125_private { bool (*link_ok)(struct net_device *); };
struct net_device { struct rtl8125_private *tp; bool running; };
#define netdev_priv(dev) ((dev)->tp)
#define netif_running(dev) ((dev)->running)
static char trace[32];
static int used, init_error, failures;
static bool link_up;
static struct net_device *current;
#define CHECK(expr) do { if (!(expr)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); failures++; \
} } while (0)
static void event(char c) { trace[used++] = c; trace[used] = '\0'; }
static void rtl8125_down(struct net_device *dev)
{ CHECK(dev == current); event('D'); }
static void mutate(struct rtl8125_private *tp, void *arg)
{ CHECK(tp == current->tp && arg == &init_error); event('M'); }
static void rtl8125_set_rxbufsize(struct rtl8125_private *tp, struct net_device *dev)
{ CHECK(dev == current && tp == dev->tp); event('S'); }
static int rtl8125_init_ring(struct net_device *dev)
{ CHECK(dev == current); event('I'); return init_error; }
#ifdef CONFIG_R8125_NAPI
static void rtl8125_enable_napi(struct rtl8125_private *tp)
{ CHECK(tp == current->tp); event('N'); }
#define NAPI "N"
#else
#define NAPI ""
#endif
static bool link_ok(struct net_device *dev)
{ CHECK(dev == current); event('L'); return link_up; }
static void rtl8125_link_on_patch(struct net_device *dev)
{ CHECK(dev == current); event('U'); }
static void rtl8125_link_down_patch(struct net_device *dev)
{ CHECK(dev == current); event('F'); }
'''

REBUILD_TESTS = r'''
int main(void)
{
        static const struct {
                const char *name;
                bool running, callback, link;
                int error;
                const char *trace;
        } cases[] = {
                {"down mutation only", false, true, false, 0, "M"},
                {"down null callback", false, false, false, 0, ""},
                {"live link up", true, true, true, 0, "DMSI" NAPI "LU"},
                {"live link down", true, true, false, 0, "DMSI" NAPI "LF"},
                {"init failure restores NAPI", true, true, true, -ENOMEM, "DMSI" NAPI},
                {"live null callback", true, false, true, 0, "DSI" NAPI "LU"},
                {"null callback init failure", true, false, false, -EIO, "DSI" NAPI},
        };
        struct rtl8125_private tp = {.link_ok = link_ok};
        struct net_device dev = {.tp = &tp};
        unsigned int i;
        current = &dev;
        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
                int before = failures;
                used = 0;
                trace[0] = '\0';
                dev.running = cases[i].running;
                link_up = cases[i].link;
                init_error = cases[i].error;
                CHECK(rtl8125_rings_rebuild(&dev, cases[i].callback ? mutate : NULL,
                                           &init_error) == cases[i].error);
                CHECK(strcmp(trace, cases[i].trace) == 0);
                printf("rebuild %s: %s (trace=%s)\n", cases[i].name,
                       failures == before ? "PASS" : "FAIL", trace);
        }
        printf("rebuild: %zu cases, %d failed assertions\n",
               sizeof(cases) / sizeof(cases[0]), failures);
        return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--revision', help='Read production source from a git revision')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]

    def source(name):
        if args.revision:
            return subprocess.check_output(
                ['git', 'show', f'{args.revision}:src/{name}'], cwd=root, text=True
            )
        return (root / 'src' / name).read_text()

    # Symbol boundaries select verbatim production code, not implementation copies.
    setup = source('r8125_xdp.c')
    start = setup.index('struct rtl8125_xsk_binding {')
    end = setup.index('\nint rtl8125_xdp_bpf(', start)
    rebuild = source('r8125_n.c')
    rb_start = rebuild.index('int\nrtl8125_rings_rebuild(')
    rb_end = rebuild.index('\nstatic void\nrtl8125_set_mtu(', rb_start)
    harnesses = [
        ('setup', STUBS + setup[start:end] + TESTS, []),
        ('rebuild-napi', REBUILD_STUBS + rebuild[rb_start:rb_end] + REBUILD_TESTS,
         ['-DCONFIG_R8125_NAPI']),
        ('rebuild-no-napi', REBUILD_STUBS + rebuild[rb_start:rb_end] + REBUILD_TESTS, []),
    ]
    failed = False
    with tempfile.TemporaryDirectory(prefix='r8125-xsk-test-') as directory:
        path = Path(directory)
        for name, code, defines in harnesses:
            cfile, binary = path / (name + '.c'), path / name
            cfile.write_text(code)
            command = (shlex.split(os.environ.get('CC', 'cc'))
                       + ['-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic']
                       + shlex.split(os.environ.get('CFLAGS', '')) + defines
                       + [str(cfile), '-o', str(binary)])
            print('COMPILE:', shlex.join(command), flush=True)
            subprocess.run(command, check=True)
            result = subprocess.run([str(binary)])
            print(f'{name}: exit={result.returncode}', flush=True)
            failed |= result.returncode != 0
    return int(failed)


if __name__ == '__main__':
    raise SystemExit(main())
