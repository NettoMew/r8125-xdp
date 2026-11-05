#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Phase 0 probes for the r8125 XDP work, see docs/PHASE0.md.

On the device under test, as root, with the exp/phase0 module built:

    phase0.py rx-offset IFACE ADDR/PLEN PEER [--offsets 0,1,2,4,8,16,32,64,258]
    phase0.py tx-pad    IFACE ADDR/PLEN PEER [--modes 3,1,0 | --loaded]

On the peer:

    phase0.py serve   [--port 5125]              byte stream for rx-offset
    phase0.py capture IFACE DUT_MAC [--seconds N] frame capture for tx-pad

Only the Python standard library is used. Every finished run is appended to
a JSON lines journal and synced to disk before the next module reload, so a
probe that takes the kernel down still leaves the earlier results behind.
"""

import argparse
import collections
import json
import os
import platform
import random
import re
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

if sys.version_info < (3, 9):
    sys.exit("phase0.py needs Python 3.9 or newer")

MODULE = "r8125"
PARAMS = Path("/sys/module", MODULE, "parameters")
DEFAULT_KO = Path(__file__).resolve().parent.parent / "src" / f"{MODULE}.ko"
SO_BINDTODEVICE = getattr(socket, "SO_BINDTODEVICE", 25)
ETH_P_ALL = 0x0003
ETH_ZLEN = 60

RX_OFFSET_MAX = 512
STREAM_PORT = 5125
BLOCK = 1 << 16
BODY = random.Random(0x8125).randbytes(BLOCK)

ECHO_SIZES = (1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65,
              127, 128, 255, 256, 511, 512, 1023, 1024, 1471, 1472)
ECHO_PATTERNS = {
    "00": lambda n: bytes(n),
    "ff": lambda n: b"\xff" * n,
    "a5": lambda n: b"\xa5" * n,
    "ramp": lambda n: bytes(i & 0xff for i in range(n)),
    "random": lambda n: random.Random(n).randbytes(n),
}

# tx-pad tags each mode with its own UDP source port, so the capture on the
# peer can attribute every frame without any coordination.
SPORT_BASE = 40000
TX_MODES = range(4)
TX_DPORTS = (9, 319, 320)       # discard, PTP event, PTP general
TX_PAYLOADS = (0, 17)           # 42 and 59 byte frames
TX_FILL = 0x5a                  # non zero, so leaked pad bytes stand out

COUNTERS = ("Ip.InHdrErrors", "IpExt.InCsumErrors", "Icmp.InCsumErrors",
            "Tcp.InCsumErrors", "Udp.InCsumErrors")
RX_ERRORS = ("rx_errors", "rx_dropped", "rx_crc_errors", "rx_frame_errors",
             "rx_length_errors", "rx_missed_errors", "rx_over_errors")
IF_COUNTERS = ("rx_packets", *RX_ERRORS, "tx_packets", "tx_errors")
PEER_COUNTER_RE = re.compile(
    r"runt|undersize|short|fragment|length|crc|align|symbol|error|missed|drop",
    re.IGNORECASE)


class ProbeError(Exception):
    """A single run failed; the sweep records it and moves on."""


def log(msg=""):
    print(msg, flush=True)


def run(*argv):
    proc = subprocess.run(argv, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True)
    if proc.returncode:
        raise ProbeError(f"{' '.join(argv)}: {proc.stdout.strip()}")
    return proc.stdout


def read(path, default=None):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return default


def wait_for(cond, timeout, what):
    deadline = time.monotonic() + timeout
    while not cond():
        if time.monotonic() > deadline:
            raise ProbeError(f"timed out waiting for {what}")
        time.sleep(0.2)


def require_root():
    if os.geteuid():
        sys.exit("this subcommand needs root")


def int_list(text, valid):
    values = [int(v, 0) for v in text.split(",") if v]
    bad = [v for v in values if v not in valid]
    if bad:
        raise argparse.ArgumentTypeError(f"out of range: {bad}")
    return values


def size(text):
    units = {"": 1, "k": 1 << 10, "m": 1 << 20, "g": 1 << 30}
    m = re.fullmatch(r"(\d+)([kmg]?)", text.lower())
    if not m:
        raise argparse.ArgumentTypeError(f"bad size: {text}")
    return int(m[1]) * units[m[2]]


def inet_checksum(data):
    """RFC 1071 sum; a packet with a valid checksum folds to zero."""
    if len(data) & 1:
        data += b"\0"
    s = sum(struct.unpack(f"!{len(data) // 2}H", data))
    s = (s & 0xffff) + (s >> 16)
    s += s >> 16
    return ~s & 0xffff


def bound_socket(iface, *args):
    sock = socket.socket(*args)
    sock.setsockopt(socket.SOL_SOCKET, SO_BINDTODEVICE, iface.encode())
    return sock


class Journal:
    def __init__(self, path):
        self.path = Path(path)

    def write(self, record):
        with self.path.open("a") as f:
            f.write(json.dumps(record, sort_keys=True) + "\n")
            f.flush()
            os.fsync(f.fileno())


class Kmsg:
    """Kernel log lines that appear between two points in time."""

    def __init__(self):
        self.mark = len(self._lines())

    @staticmethod
    def _lines():
        try:
            return run("dmesg").splitlines()
        except ProbeError:
            return []

    def since(self, limit=40):
        lines = self._lines()[self.mark:]
        return lines[-limit:]


# ---------------------------------------------------------------- DUT side

class Dut:
    def __init__(self, args):
        self.ko = Path(args.ko)
        self.iface = args.iface
        self.addr = args.addr
        self.local = args.addr.split("/")[0]
        self.peer = args.peer
        if not self.ko.is_file():
            sys.exit(f"{self.ko}: no such module, build exp/phase0 first")

    def reload(self, **params):
        if Path("/sys/module", MODULE).exists():
            run("rmmod", MODULE)
        os.sync()
        run("insmod", str(self.ko), *(f"{k}={v}" for k, v in params.items()))
        for key, value in params.items():
            got = read(PARAMS / key)
            if got != str(value):
                raise ProbeError(f"{key}={value} requested, module reports {got}")

    def bring_up(self, rx_csum=True):
        net = Path("/sys/class/net", self.iface)
        wait_for(net.exists, 10, f"{self.iface} to appear")
        run("ip", "link", "set", self.iface, "up")
        wait_for(lambda: read(net / "carrier") == "1", 30, f"{self.iface} link")
        run("ip", "addr", "replace", self.addr, "dev", self.iface)
        if not rx_csum:
            run("ethtool", "-K", self.iface, "rx", "off")
        pinger = Pinger(self.iface, self.peer)
        deadline = time.monotonic() + 15
        while pinger.echo(b"phase0", timeout=0.5) is None:
            if time.monotonic() > deadline:
                raise ProbeError(f"{self.peer} does not answer on {self.iface}")
        return pinger

    def driver_var(self, *keys):
        text = read(Path("/proc/net", MODULE, self.iface, "driver_var"), "")
        found = dict(line.split(None, 1) for line in text.splitlines()
                     if len(line.split(None, 1)) == 2)
        return {k: found.get(k) for k in keys}

    def counters(self):
        values = {}
        for table in ("/proc/net/snmp", "/proc/net/netstat"):
            lines = Path(table).read_text().splitlines()
            for head, body in zip(lines[::2], lines[1::2]):
                proto, *names = head.split()
                _, *nums = body.split()
                for name, num in zip(names, nums):
                    values[f"{proto.rstrip(':')}.{name}"] = int(num)
        stats = Path("/sys/class/net", self.iface, "statistics")
        values = {k: values.get(k, 0) for k in COUNTERS}
        for key in IF_COUNTERS:
            values[key] = int(read(stats / key, "0"))
        return values

    def restore(self):
        log("== restoring vendor defaults")
        try:
            self.reload()
            self.bring_up()
        except ProbeError as err:
            log(f"   restore failed: {err}")


def delta(before, after):
    return {k: after[k] - before[k] for k in after if after[k] != before[k]}


class Pinger:
    """ICMP echo that checks every reply byte for byte."""

    def __init__(self, iface, peer):
        self.sock = bound_socket(iface, socket.AF_INET, socket.SOCK_RAW,
                                 socket.IPPROTO_ICMP)
        self.peer = peer
        self.ident = os.getpid() & 0xffff
        self.seq = 0

    def echo(self, payload, timeout=1.0):
        """Returns (reply_payload, checksum_ok), or None when nothing came back."""
        self.seq = (self.seq + 1) & 0xffff
        head = struct.pack("!BBHHH", 8, 0, 0, self.ident, self.seq)
        csum = inet_checksum(head + payload)
        self.sock.sendto(struct.pack("!BBHHH", 8, 0, csum, self.ident, self.seq)
                         + payload, (self.peer, 0))
        deadline = time.monotonic() + timeout
        while (left := deadline - time.monotonic()) > 0:
            self.sock.settimeout(left)
            try:
                packet = self.sock.recv(65535)
            except socket.timeout:
                break
            icmp = packet[(packet[0] & 0x0f) * 4:]
            if len(icmp) < 8:
                continue
            kind, _, _, ident, seq = struct.unpack("!BBHHH", icmp[:8])
            if kind == 0 and ident == self.ident and seq == self.seq:
                return icmp[8:], inet_checksum(icmp) == 0
        return None


def first_difference(want, got):
    for i, (a, b) in enumerate(zip(want, got)):
        if a != b:
            return f"byte {i}: want {a:02x} got {b:02x}"
    return f"length: want {len(want)} got {len(got)}"


def echo_sweep(pinger, rounds, give_up=20):
    stats = collections.Counter()
    samples = []
    streak = 0
    for size_ in ECHO_SIZES:
        for name, make in ECHO_PATTERNS.items():
            payload = make(size_)
            for _ in range(rounds):
                stats["sent"] += 1
                reply = pinger.echo(payload)
                if reply is None:
                    stats["lost"] += 1
                    streak += 1
                    if streak >= give_up:
                        stats["aborted"] = 1
                        return dict(stats), samples
                    continue
                streak = 0
                data, csum_ok = reply
                if not csum_ok:
                    stats["bad_csum"] += 1
                if data != payload:
                    stats["corrupt"] += 1
                    if len(samples) < 8:
                        samples.append(f"size {size_} pattern {name}: "
                                       + first_difference(payload, data))
                elif csum_ok:
                    stats["ok"] += 1
    return dict(stats), samples


def expected_block(index, length):
    return index.to_bytes(8, "big") + BODY[8:length]


def stream_check(iface, peer, port, total):
    sock = bound_socket(iface, socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(10)
    try:
        sock.connect((peer, port))
    except OSError as err:
        raise ProbeError(f"stream server {peer}:{port}: {err}") from None
    sock.sendall(struct.pack("!Q", total))
    buf = bytearray(BLOCK)
    view = memoryview(buf)
    result = {"bytes": 0, "bad_blocks": 0}
    start = time.monotonic()
    index = 0
    with sock:
        while result["bytes"] < total:
            length = min(BLOCK, total - result["bytes"])
            got = 0
            while got < length:
                n = sock.recv_into(view[got:length])
                if not n:
                    raise ProbeError(f"stream closed after {result['bytes'] + got} bytes")
                got += n
            want = expected_block(index, length)
            if buf[:length] != want:
                result["bad_blocks"] += 1
                result.setdefault("first_bad", f"block {index}, "
                                  + first_difference(want, buf[:length]))
            result["bytes"] += length
            index += 1
    result["MBps"] = round(total / (time.monotonic() - start) / 1e6, 1)
    return result


def rx_offset_verdict(rec):
    if "error" in rec:
        return "ERROR"
    echo = rec["echo"]
    stream = rec.get("stream", {})
    dirty = (echo.get("lost") or echo.get("corrupt") or echo.get("bad_csum")
             or stream.get("bad_blocks")
             or any(rec["counters"].get(k) for k in COUNTERS + RX_ERRORS))
    return "FAIL" if dirty else "PASS"


def cmd_rx_offset(args):
    require_root()
    dut = Dut(args)
    journal = Journal(args.journal)
    rows = []
    for offset in args.offsets:
        log(f"== rx_offset {offset}")
        rec = {"probe": "rx-offset", "rx_offset": offset,
               "kernel": platform.release(), "time": time.time()}
        kmsg = Kmsg()
        try:
            dut.reload(rx_offset=offset)
            pinger = dut.bring_up(rx_csum=False)
            rec["driver"] = dut.driver_var("mcfg", "rx_buf_sz",
                                           "rx_buf_page_order", "rx_buf_page_size")
            before = dut.counters()
            rec["echo"], rec["echo_samples"] = echo_sweep(pinger, args.rounds)
            log(f"   echo   {rec['echo']}")
            if args.bytes:
                rec["stream"] = stream_check(args.iface, args.peer, args.port, args.bytes)
                log(f"   stream {rec['stream']}")
            rec["counters"] = delta(before, dut.counters())
            log(f"   delta  {rec['counters']}")
        except (ProbeError, OSError) as err:
            rec["error"] = str(err)
            log(f"   error  {err}")
        rec["dmesg"] = kmsg.since()
        rec["verdict"] = rx_offset_verdict(rec)
        journal.write(rec)
        rows.append(rec)

    log("\n offset  verdict  echo ok/sent  corrupt  stream bad  page order")
    for rec in rows:
        echo = rec.get("echo", {})
        bad = rec.get("stream", {}).get("bad_blocks", "-")
        order = rec.get("driver", {}).get("rx_buf_page_order") or "-"
        log(f" {rec['rx_offset']:>6}  {rec['verdict']:<7}  "
            f"{echo.get('ok', 0):>5}/{echo.get('sent', 0):<5}  "
            f"{echo.get('corrupt', 0):>7}  {bad!s:>10}  {order:>10}")
    log(f"\njournal: {args.journal}")
    if args.restore:
        dut.restore()


def cmd_tx_pad(args):
    require_root()
    dut = Dut(args)
    journal = Journal(args.journal)
    if args.loaded:
        loaded = read(PARAMS / "tx_pad")
        if loaded is None:
            sys.exit(f"--loaded given, but {MODULE} is not loaded")
        args.modes, args.restore = [int(loaded)], False
    for mode in args.modes:
        log(f"== tx_pad {mode} (source port {SPORT_BASE + mode})")
        rec = {"probe": "tx-pad", "tx_pad": mode, "kernel": platform.release(),
               "time": time.time(), "count": args.count,
               "dports": TX_DPORTS, "payloads": TX_PAYLOADS}
        kmsg = Kmsg()
        try:
            if not args.loaded:
                dut.reload(tx_pad=mode)
            dut.bring_up()
            rec["driver"] = dut.driver_var("mcfg", "chipset_name",
                                           "UseSwPaddingShortPkt",
                                           "ShortPacketSwChecksum")
            before = dut.counters()
            sock = bound_socket(args.iface, socket.AF_INET, socket.SOCK_DGRAM)
            with sock:
                sock.bind((dut.local, SPORT_BASE + mode))
                for dport in TX_DPORTS:
                    for payload in TX_PAYLOADS:
                        data = bytes([TX_FILL]) * payload
                        for _ in range(args.count):
                            sock.sendto(data, (args.peer, dport))
                            time.sleep(0.001)
            time.sleep(0.5)
            rec["counters"] = delta(before, dut.counters())
            log(f"   sent {args.count} per (port, payload), delta {rec['counters']}")
        except (ProbeError, OSError) as err:
            rec["error"] = str(err)
            log(f"   error  {err}")
        rec["dmesg"] = kmsg.since()
        journal.write(rec)
    log(f"\njournal: {args.journal}; the result is in the peer capture")
    if args.restore:
        dut.restore()


# --------------------------------------------------------------- peer side

def cmd_serve(args):
    with socket.create_server(("", args.port)) as server:
        log(f"serving the phase 0 byte stream on port {args.port}")
        while True:
            conn, addr = server.accept()
            with conn:
                try:
                    head = conn.recv(8, socket.MSG_WAITALL)
                    if len(head) != 8:
                        continue
                    total, = struct.unpack("!Q", head)
                    sent, index = 0, 0
                    while sent < total:
                        length = min(BLOCK, total - sent)
                        conn.sendall(expected_block(index, length))
                        sent += length
                        index += 1
                    log(f"{addr[0]}: {sent} bytes")
                except OSError as err:
                    log(f"{addr[0]}: {err}")


class Pcap:
    def __init__(self, path):
        self.file = open(path, "wb")
        self.file.write(struct.pack("<IHHiIII", 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1))

    def write(self, frame):
        now = time.time()
        self.file.write(struct.pack("<IIII", int(now), int(now % 1 * 1e6),
                                    len(frame), len(frame)) + frame)

    def close(self):
        self.file.close()


def parse_mac(text):
    try:
        mac = bytes.fromhex(text.replace(":", "").replace("-", ""))
    except ValueError:
        mac = b""
    if len(mac) != 6:
        raise argparse.ArgumentTypeError(f"bad MAC address: {text}")
    return mac


def rx_fcs_enabled(iface):
    try:
        return "rx-fcs: on" in run("ethtool", "-k", iface)
    except (ProbeError, OSError):
        return False


def peer_counters(iface):
    values = {}
    try:
        for line in run("ethtool", "-S", iface).splitlines():
            name, _, value = line.strip().partition(":")
            if PEER_COUNTER_RE.search(name) and value.strip().isdigit():
                values[name] = int(value)
    except (ProbeError, OSError):
        pass
    stats = Path("/sys/class/net", iface, "statistics")
    for key in IF_COUNTERS:
        values[f"sysfs.{key}"] = int(read(stats / key, "0"))
    return values


def classify(frame, fcs):
    """Decodes a tx-pad probe frame, or returns None for anything else."""
    if fcs:
        frame = frame[:-4]
    if len(frame) < 14 + 20 + 8 or frame[12:14] != b"\x08\x00":
        return None
    ip = frame[14:]
    ihl = (ip[0] & 0x0f) * 4
    if ip[9] != socket.IPPROTO_UDP or len(ip) < ihl + 8:
        return None
    sport, dport, ulen, ucsum = struct.unpack("!HHHH", ip[ihl:ihl + 8])
    if sport - SPORT_BASE not in TX_MODES:
        return None
    total = int.from_bytes(ip[2:4], "big")
    pad = frame[14 + total:]
    if len(frame) < ETH_ZLEN:
        pad_kind = "none"
    elif any(pad):
        pad_kind = "nonzero"
    else:
        pad_kind = "zero"
    pseudo = ip[12:20] + struct.pack("!BBH", 0, socket.IPPROTO_UDP, ulen)
    udp_ok = ucsum == 0 or inet_checksum(pseudo + ip[ihl:ihl + ulen]) == 0
    return {"key": (sport - SPORT_BASE, dport, ulen - 8), "len": len(frame),
            "pad": pad_kind, "pad_hex": pad[:16].hex(),
            "ip_ok": inet_checksum(ip[:ihl]) == 0, "udp_ok": udp_ok}


def cmd_capture(args):
    require_root()
    fcs = rx_fcs_enabled(args.iface)
    sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
    sock.bind((args.iface, 0))
    pcap = Pcap(args.pcap)
    before = peer_counters(args.iface)
    groups = collections.defaultdict(lambda: {
        "frames": 0, "len": collections.Counter(), "pad": collections.Counter(),
        "ip_bad": 0, "udp_bad": 0, "pad_hex": None})
    log(f"capturing on {args.iface} for {args.seconds} s (rx-fcs {'on' if fcs else 'off'}),"
        " start tx-pad on the DUT now; Ctrl-C ends early")
    deadline = time.monotonic() + args.seconds
    try:
        while (left := deadline - time.monotonic()) > 0:
            sock.settimeout(left)
            try:
                frame = sock.recv(65535)
            except socket.timeout:
                break
            if frame[6:12] != args.dut_mac:
                continue
            probe = classify(frame, fcs)
            if probe is None:
                continue
            pcap.write(frame)
            group = groups[probe["key"]]
            group["frames"] += 1
            group["len"][probe["len"]] += 1
            group["pad"][probe["pad"]] += 1
            group["ip_bad"] += not probe["ip_ok"]
            group["udp_bad"] += not probe["udp_ok"]
            if probe["pad"] == "nonzero" and group["pad_hex"] is None:
                group["pad_hex"] = probe["pad_hex"]
    except KeyboardInterrupt:
        pass
    pcap.close()
    after = peer_counters(args.iface)

    log("\n mode  dport  payload  frames  length (no FCS)  pad              ip_bad  udp_bad")
    for (mode, dport, payload), g in sorted(groups.items()):
        lengths = ",".join(f"{n}x{c}" for n, c in sorted(g["len"].items()))
        pads = ",".join(f"{k}x{c}" for k, c in sorted(g["pad"].items()))
        log(f" {mode:>4}  {dport:>5}  {payload:>7}  {g['frames']:>6}  {lengths:<15}  "
            f"{pads:<15}  {g['ip_bad']:>6}  {g['udp_bad']:>7}")
        if g["pad_hex"]:
            log(f"       first non zero pad: {g['pad_hex']}")
    if not groups:
        log(" no probe frames seen")
    log(f"\nreceive error counters that moved: {delta(before, after) or 'none'}")
    log(f"pcap: {args.pcap}")


# -------------------------------------------------------------------- main

def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    sub = parser.add_subparsers(dest="command", required=True)

    def dut_parser(name, help_):
        p = sub.add_parser(name, help=help_)
        p.add_argument("iface", help="r8125 port under test")
        p.add_argument("addr", help="address for that port, e.g. 192.168.125.2/24")
        p.add_argument("peer", help="peer address on the same link")
        p.add_argument("--ko", default=DEFAULT_KO, help=f"module to load (default {DEFAULT_KO})")
        p.add_argument("--journal", default="phase0.jsonl", help="JSON lines results file")
        p.add_argument("--no-restore", dest="restore", action="store_false",
                       help="leave the last setting loaded")
        return p

    p = dut_parser("rx-offset", "RX buffer alignment probe (DUT)")
    p.add_argument("--offsets", default=[0, 1, 2, 4, 8, 16, 32, 64, 258],
                   type=lambda t: int_list(t, range(RX_OFFSET_MAX + 1)),
                   help="comma separated rx_offset values")
    p.add_argument("--rounds", type=int, default=2, help="echoes per size and pattern")
    p.add_argument("--bytes", type=size, default=size("1g"),
                   help="byte stream length, 0 skips it (default 1g)")
    p.add_argument("--port", type=int, default=STREAM_PORT, help="peer stream port")
    p.set_defaults(func=cmd_rx_offset)

    p = dut_parser("tx-pad", "TX short frame padding probe (DUT)")
    p.add_argument("--modes", default=[3, 1, 0],
                   type=lambda t: int_list(t, TX_MODES),
                   help="comma separated tx_pad values")
    p.add_argument("--count", type=int, default=20, help="frames per port and payload")
    p.add_argument("--loaded", action="store_true",
                   help="probe the module as loaded, without reloading it")
    p.set_defaults(func=cmd_tx_pad)

    p = sub.add_parser("serve", help="byte stream server for rx-offset (peer)")
    p.add_argument("--port", type=int, default=STREAM_PORT)
    p.set_defaults(func=cmd_serve)

    p = sub.add_parser("capture", help="frame capture for tx-pad (peer)")
    p.add_argument("iface", help="peer port facing the DUT")
    p.add_argument("dut_mac", type=parse_mac, help="MAC address of the DUT port")
    p.add_argument("--seconds", type=int, default=120)
    p.add_argument("--pcap", default="phase0-tx-pad.pcap")
    p.set_defaults(func=cmd_capture)

    args = parser.parse_args()
    try:
        args.func(args)
    except ProbeError as err:
        sys.exit(f"phase0: {err}")
    except KeyboardInterrupt:
        sys.exit(130)


if __name__ == "__main__":
    main()
