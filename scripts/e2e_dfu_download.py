#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Xaloqi
"""[EDS#562] One firmware download to examples/safeboot_ecu over CAN: 0x10 02, 0x27
SecurityAccess, 0x34 RequestDownload, 0x36 TransferData, 0x37 RequestTransferExit,
timed, with the Flow Control the ECU advertised and a check of its console for the
CAN RX queue overflowing ("Msgq overflowed"). Python stdlib only, so it can be sent
to a Linux shell on the bench (the Pi 5's DomD) and run there.

The tester is the Linux kernel's CAN_ISOTP socket (net/can/isotp.c), which obeys the
ECU's Flow Control (BS, STmin) exactly, so the measured rate is the ECU's and the bus's,
not a userspace tester's. A raw socket on the same interface records the ECU's Flow
Control frames; --expect-fc fails the run when the firmware under test does not
advertise what the run is meant to compare (BS 4 / STmin 1 for the old override,
BS 16 / STmin 0 for the stack's default, EDS#558).

    e2e_dfu_download.py download IFACE IMAGE [--req 7E0] [--rsp 7E8] [--addr 0x100000]
                        [--key 000102030405060708090A0B0C0D0E0F] [--expect-fc BS,STMIN]
                        [--console-log FILE] [--reset]
    e2e_dfu_download.py console LOGFILE [--since BYTES]
    e2e_dfu_download.py selftest

IMAGE is a signed MCUboot image (west sign ... zephyr.signed.bin) of at most 512 KB
(README, "Flash layout"). --addr is the secondary slot, flash-DEVICE-relative
(0x100000 on the nucleo_h753zi layout). The default key is the compile-time
placeholder of core/uds_security_algo.c; pass the ECU's real level-1 key otherwise.

Timing, all from the tester's monotonic clock: the 0x34 phase includes the erase of the
secondary slot (the ECU answers 0x78 once per erased sector); the 0x36 phase is the
transfer proper and is the figure to compare; "total" is 0x34 to 0x37 and is what the
2026-09-26 record's 3.7 KB/s (108,132 bytes in 28.4 s) measured.

console: counts the "overflowed" lines a console logger wrote after byte offset BYTES
of LOGFILE (take the offset with `stat -c %s LOGFILE` before the run). Exit 0 when there
are none. On the bench the ST-LINK VCP logger runs on the host and the download runs in
DomD, so this runs on the host; --console-log does the same inside `download` when the
log is local (the native_sim rehearsal).

Exit 0 when the download completed and no overflow was logged.

Rehearsal on native_sim (no hardware, no sudo): build examples/safeboot_ecu for
native_sim with the flash simulator and a real-CAN overlay (the recipe is in the
2026-11-09 campaign record), then

    unshare -rn sh -c 'ip link add zcan0 type vcan && ip link set zcan0 up &&
        (cd /tmp/scratch && zephyr.exe -flash=flash.bin > ecu.log 2>&1 &) && sleep 2 &&
        python3 scripts/e2e_dfu_download.py download zcan0 image.signed.bin \\
            --addr 0x75000 --console-log /tmp/scratch/ecu.log'
"""

from __future__ import annotations

import argparse
import errno
import socket
import struct
import sys
import time
import zlib
from typing import Dict, List, Optional, Tuple

SOL_CAN_ISOTP = 106
CAN_ISOTP_OPTS = 1
CAN_ISOTP_TX_PADDING = 0x004
SOL_CAN_RAW = 101
CAN_RAW_FILTER = 1
CAN_EFF_FLAG = 0x80000000

DEFAULT_KEY = bytes(range(16))
MAX_IMAGE = 512 * 1024
P2STAR_S = 5.0            # the example's P2*max (5000 ms); the wait after each 0x78
MAX_PENDING = 200         # 0x78 answers tolerated for one request

# ---------------------------------------------------------------------------
# AES-128-CMAC (RFC 4493), stdlib only. The key to SendKey is the first 4 bytes of
# CMAC(level key, seed) (core/uds_security_algo.c).
# ---------------------------------------------------------------------------


def _build_sbox() -> List[int]:
    sbox = [0] * 256
    p = q = 1
    while True:
        p = p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)
        q ^= q << 1
        q ^= q << 2
        q ^= q << 4
        q &= 0xFF
        if q & 0x80:
            q ^= 0x09
        x = q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4))
        sbox[p] = (x ^ 0x63) & 0xFF
        if p == 1:
            break
    sbox[0] = 0x63
    return sbox


_SBOX = _build_sbox()


def _xtime(a: int) -> int:
    return ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else a << 1


def _expand_key(key: bytes) -> List[List[int]]:
    w = [list(key[i:i + 4]) for i in range(0, 16, 4)]
    rcon = 1
    for i in range(4, 44):
        t = list(w[i - 1])
        if i % 4 == 0:
            t = [_SBOX[b] for b in t[1:] + t[:1]]
            t[0] ^= rcon
            rcon = _xtime(rcon)
        w.append([a ^ b for a, b in zip(w[i - 4], t)])
    return [sum(w[4 * r:4 * r + 4], []) for r in range(11)]


def _aes_encrypt_block(rk: List[List[int]], block: bytes) -> bytes:
    s = [b ^ k for b, k in zip(block, rk[0])]
    for r in range(1, 11):
        s = [_SBOX[b] for b in s]
        s = [s[(i + 4 * (i % 4)) % 16] for i in range(16)]          # ShiftRows
        if r != 10:
            m = []
            for c in range(4):
                a = s[4 * c:4 * c + 4]
                m += [_xtime(a[0]) ^ _xtime(a[1]) ^ a[1] ^ a[2] ^ a[3],
                      a[0] ^ _xtime(a[1]) ^ _xtime(a[2]) ^ a[2] ^ a[3],
                      a[0] ^ a[1] ^ _xtime(a[2]) ^ _xtime(a[3]) ^ a[3],
                      _xtime(a[0]) ^ a[0] ^ a[1] ^ a[2] ^ _xtime(a[3])]
            s = m
        s = [b ^ k for b, k in zip(s, rk[r])]
    return bytes(s)


def aes_cmac(key: bytes, msg: bytes) -> bytes:
    rk = _expand_key(key)

    def dbl(b: bytes) -> bytes:
        n = int.from_bytes(b, "big") << 1
        if n >> 128:
            n = (n & ((1 << 128) - 1)) ^ 0x87
        return n.to_bytes(16, "big")

    k1 = dbl(_aes_encrypt_block(rk, bytes(16)))
    k2 = dbl(k1)
    n = max(1, (len(msg) + 15) // 16)
    last = msg[16 * (n - 1):]
    if len(msg) and len(last) == 16:
        last = bytes(a ^ b for a, b in zip(last, k1))
    else:
        last = last + b"\x80" + bytes(15 - len(last))
        last = bytes(a ^ b for a, b in zip(last, k2))
    x = bytes(16)
    for i in range(n - 1):
        x = _aes_encrypt_block(rk, bytes(a ^ b for a, b in zip(x, msg[16 * i:16 * i + 16])))
    return _aes_encrypt_block(rk, bytes(a ^ b for a, b in zip(x, last)))


def selftest() -> int:
    key = bytes.fromhex("2b7e151628aed2a6abf7158809cf4f3c")      # RFC 4493, section 4
    vectors = [
        (b"", "bb1d6929e95937287fa37d129b756746"),
        (bytes.fromhex("6bc1bee22e409f96e93d7e117393172a"), "070a16b46b4d4144f79bdd9dd04a287c"),
        (bytes.fromhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411"),
         "dfa66747de9ae63030ca32611497c827"),
    ]
    bad = [i for i, (m, t) in enumerate(vectors) if aes_cmac(key, m).hex() != t]
    print("selftest: AES-CMAC RFC 4493 vectors " + ("PASS" if not bad else "FAIL %s" % bad))
    return 1 if bad else 0

# ---------------------------------------------------------------------------
# UDS over the kernel's ISO-TP socket
# ---------------------------------------------------------------------------


class UdsError(Exception):
    pass


class Tester:
    def __init__(self, iface: str, req: int, rsp: int) -> None:
        self.s = socket.socket(socket.AF_CAN, socket.SOCK_DGRAM, socket.CAN_ISOTP)
        # struct can_isotp_options: flags, frame_txtime, ext_address, txpad, rxpad, rx_ext_address
        self.s.setsockopt(SOL_CAN_ISOTP, CAN_ISOTP_OPTS,
                          struct.pack("=IIBBBB", CAN_ISOTP_TX_PADDING, 0, 0, 0xAA, 0, 0))
        self.s.bind((iface, rsp, req))
        self.s.settimeout(P2STAR_S)
        # The ECU's Flow Control frames (PCI 3x) on the response ID, as the wire carries them.
        self.raw = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
        self.raw.setsockopt(SOL_CAN_RAW, CAN_RAW_FILTER, struct.pack("=II", rsp, 0x7FF))
        self.raw.bind((iface,))
        self.raw.settimeout(0)
        self.fcs: Dict[Tuple[int, int, int], int] = {}
        self.pending = 0

    def drain_fc(self) -> None:
        while True:
            try:
                f = self.raw.recv(16)
            except (BlockingIOError, socket.timeout):
                return
            can_id, dlc = struct.unpack_from("=IB", f)
            data = f[8:8 + dlc]
            if dlc >= 3 and (data[0] & 0xF0) == 0x30:
                k = (data[0] & 0x0F, data[1], data[2])
                self.fcs[k] = self.fcs.get(k, 0) + 1

    def request(self, pdu: bytes, what: str) -> bytes:
        sid = pdu[0]
        try:
            self.s.send(pdu)
        except OSError as e:
            raise UdsError("%s: send failed, %s (no Flow Control from the ECU?)"
                           % (what, errno.errorcode.get(e.errno, e.errno)))
        n = 0
        while True:
            try:
                r = self.s.recv(4096)
            except socket.timeout:
                raise UdsError("%s: no answer within %.0f s" % (what, P2STAR_S))
            if len(r) >= 3 and r[0] == 0x7F and r[1] == sid and r[2] == 0x78:
                n += 1
                self.pending += 1
                if n > MAX_PENDING:
                    raise UdsError("%s: more than %d x 0x78" % (what, MAX_PENDING))
                continue
            if len(r) >= 3 and r[0] == 0x7F and r[1] == sid:
                raise UdsError("%s: NRC 0x%02X" % (what, r[2]))
            if r[0] != sid + 0x40:
                raise UdsError("%s: unexpected answer %s" % (what, r[:8].hex()))
            return r


def download(a: argparse.Namespace) -> int:
    image = open(a.image, "rb").read()
    if not image or len(image) > MAX_IMAGE:
        print("FAIL: image is %d bytes; the largest installable image is %d (README, Flash layout)"
              % (len(image), MAX_IMAGE))
        return 1
    if image[:4] != bytes.fromhex("3db8f396"):
        print("FAIL: %s does not start with the MCUboot magic 3D B8 F3 96 (an unsigned image?)" % a.image)
        return 1
    key = bytes.fromhex(a.key)
    con_start = 0
    if a.console_log:
        import os
        con_start = os.path.getsize(a.console_log)
    t = Tester(a.iface, int(a.req, 16), int(a.rsp, 16))
    addr = int(a.addr, 0)
    try:
        t.request(bytes([0x10, 0x02]), "10 02 programming session")
        seed = t.request(bytes([0x27, 0x01]), "27 01 seed")[2:]
        sk = aes_cmac(key, seed)[:4]
        t.request(bytes([0x27, 0x02]) + sk, "27 02 key (wrong --key?)")
        try:                                  # optional, not enforced by 0x34 (README, step 4)
            t.request(bytes([0x31, 0x01, 0xFF, 0x00]), "31 01 FF00 preconditions")
        except UdsError as e:
            print("note: %s (continuing, 0x34 does not need it)" % e)

        t0 = time.monotonic()
        r = t.request(bytes([0x34, 0x00, 0x44]) + struct.pack(">II", addr, len(image)), "34 RequestDownload")
        t34 = time.monotonic()
        # 74 <LFI> <maxNumberOfBlockLength>: LFI high nibble = number of length bytes
        nlen = r[1] >> 4
        maxlen = int.from_bytes(r[2:2 + nlen], "big")
        blk = maxlen - 2                      # SID and block counter are inside maxNumberOfBlockLength
        if blk < 1:
            raise UdsError("34: maxNumberOfBlockLength %d is too small" % maxlen)
        pending34 = t.pending

        crc = 0
        nblocks = 0
        seq = 1
        for off in range(0, len(image), blk):
            chunk = image[off:off + blk]
            t.request(bytes([0x36, seq & 0xFF]) + chunk, "36 TransferData block %d" % (nblocks + 1))
            crc = zlib.crc32(chunk, crc)
            nblocks += 1
            seq = (seq + 1) & 0xFF
            if nblocks % 64 == 0:
                t.drain_fc()
        t36 = time.monotonic()
        t.request(bytes([0x37]) + struct.pack(">I", crc & 0xFFFFFFFF), "37 RequestTransferExit")
        t37 = time.monotonic()
        t.drain_fc()
        if a.reset:
            t.s.send(bytes([0x11, 0x01]))
    except UdsError as e:
        t.drain_fc()
        print("FAIL: %s" % e)
        return 1

    n = len(image)
    xfer = t36 - t34
    print("image        %s, %d bytes" % (a.image, n))
    print("block        maxNumberOfBlockLength %d, %d payload bytes, %d blocks" % (maxlen, blk, nblocks))
    print("0x34         %.2f s (erase of the secondary slot; %d x 0x78)" % (t34 - t0, pending34))
    print("0x36         %.2f s, %.2f KiB/s (%.0f B/s)" % (xfer, n / 1024.0 / xfer, n / xfer))
    print("0x37         %.2f s" % (t37 - t36))
    print("total        %.2f s, %.2f KiB/s (0x34 to 0x37)" % (t37 - t0, n / 1024.0 / (t37 - t0)))
    fcs = ", ".join("BS %d STmin %d (x%d)" % (bs, st, c) for (_, bs, st), c in sorted(t.fcs.items())) or "none seen"
    print("ECU FC       %s" % fcs)

    ok = True
    if a.expect_fc:
        want_bs, want_st = (int(x) for x in a.expect_fc.split(","))
        seen = {(bs, st) for (_, bs, st) in t.fcs}
        if seen != {(want_bs, want_st)}:
            print("FAIL: the ECU advertised %s, this run expects BS %d STmin %d (wrong firmware flashed?)"
                  % (sorted(seen) or "no FC", want_bs, want_st))
            ok = False
    if a.console_log:
        ok = check_console(a.console_log, con_start) and ok
    if ok:
        print("PASS: download complete, %.2f KiB/s over 0x36, %.2f KiB/s total" % (n / 1024.0 / xfer, n / 1024.0 / (t37 - t0)))
    return 0 if ok else 1


def check_console(path: str, since: int) -> bool:
    with open(path, "rb") as f:
        f.seek(since)
        text = f.read().decode("utf-8", "replace")
    lines = [ln for ln in text.splitlines() if "overflowed" in ln]
    print("console      %d bytes read, %d 'overflowed' line(s)" % (len(text), len(lines)))
    for ln in lines[:3]:
        print("             " + ln.strip()[:100])
    return not lines


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("download")
    d.add_argument("iface")
    d.add_argument("image")
    d.add_argument("--req", default="7E0")
    d.add_argument("--rsp", default="7E8")
    d.add_argument("--addr", default="0x100000")
    d.add_argument("--key", default=DEFAULT_KEY.hex())
    d.add_argument("--expect-fc", help="BS,STMIN the ECU must advertise, e.g. 16,0 or 4,1")
    d.add_argument("--console-log")
    d.add_argument("--reset", action="store_true", help="send 11 01 after 0x37 (MCUboot swaps on the reset)")
    c = sub.add_parser("console")
    c.add_argument("logfile")
    c.add_argument("--since", type=int, default=0)
    sub.add_parser("selftest")
    a = ap.parse_args()
    if a.cmd == "selftest":
        return selftest()
    if a.cmd == "console":
        ok = check_console(a.logfile, a.since)
        print("PASS: no 'Msgq overflowed'" if ok else "FAIL: the CAN RX queue overflowed")
        return 0 if ok else 1
    return download(a)


if __name__ == "__main__":
    sys.exit(main())
