#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Xaloqi
"""[EDS#557][EDS#546] examples/canfd_ecu on native_sim over an MTU-72 vcan, tested
by the Linux kernel's own ISO-TP stack (net/can/isotp.c), not by EDS code.

The tester is a CAN_ISOTP socket set to CAN FD (mtu 72, TX_DL 64, BRS). Such a
socket only accepts CAN FD frames, so every answer here also proves the ECU sent
its SF / FF / CF / FC as FD frames. A raw CAN FD socket checks the frames
themselves: FD, BRS, the data lengths and the padding.

  1. 22 F190      -> 20-byte answer: one escape Single Frame (00 14 ...), 24 bytes
  2. 22 0100      -> 67-byte answer: FF of 64 bytes + one CF
  3. 22 + 32 DIDs -> a 65-byte REQUEST (FF + CF from Linux; the ECU's FC is FD,
                     30 10 00) and a 2112-byte answer (FF + 33 CFs of 64 bytes,
                     the last padded to a valid FD length; Linux's FC is FD)
  4. 10 03, 2A 03 01 -> periodic messages on 0x5E8: [01, 20 bytes], one FD frame
                     of 24 bytes, BRS, padded; uptime advancing; 2A 04 01 stops them

Build (from a west workspace):

    W=<EDS checkout>; E=$W/examples/canfd_ecu
    ZEPHYR_TOOLCHAIN_VARIANT=host west build -p always -b native_sim $E -- -DDIAG_SKIP_CODEGEN=ON \\
      -DEXTRA_CONF_FILE=$E/boards/native_sim/native_sim_socketcan.conf \\
      -DDTC_OVERLAY_FILE=$E/boards/native_sim/native_sim_socketcan.overlay

Run (no sudo: a user + network namespace with its own vcan; needs the can-isotp
module loaded on the host):

    unshare -rn sh -c 'ip link add zcan0 type vcan && ip link set zcan0 mtu 72 up &&
                       python3 scripts/smoke_canfd_ecu.py build/zephyr/zephyr.exe zcan0'

Exit 0 when every check passes.
"""

from __future__ import annotations

import socket
import struct
import subprocess
import sys
import threading
import time
from typing import List

import can

RX_ID, TX_ID, PERIODIC_ID = 0x7E0, 0x7E8, 0x5E8
FD_DLS = (8, 12, 16, 20, 24, 32, 48, 64)
PAD = 0xCC

SOL_CAN_ISOTP = 106              # SOL_CAN_BASE + CAN_ISOTP
CAN_ISOTP_OPTS, CAN_ISOTP_LL_OPTS = 1, 5
CAN_ISOTP_TX_PADDING = 0x004
CANFD_BRS = 0x01

RECORDS = {
    0x0100: bytes(range(0x00, 0x40)),
    0x0101: bytes(range(0x40, 0x80)),
    0x0102: bytes(range(0x80, 0xC0)),
}
IDENT = b"XALOQI-CANFD-0001"

failures: List[str] = []


def check(cond: bool, what: str) -> None:
    print(("PASS  " if cond else "FAIL  ") + what)
    if not cond:
        failures.append(what)


class Sniffer:
    """Every frame on the bus, with FD/BRS flags (a raw CAN FD socket)."""

    def __init__(self, channel: str) -> None:
        self.bus = can.Bus(interface="socketcan", channel=channel, fd=True,
                           receive_own_messages=False)
        self.frames: List[can.Message] = []
        self._stop = threading.Event()
        self._t = threading.Thread(target=self._run, daemon=True)
        self._t.start()

    def _run(self) -> None:
        while not self._stop.is_set():
            m = self.bus.recv(0.05)
            if m is not None:
                self.frames.append(m)

    def mark(self) -> int:
        return len(self.frames)

    def since(self, mark: int, can_id: int) -> List[can.Message]:
        time.sleep(0.1)
        return [m for m in self.frames[mark:] if m.arbitration_id == can_id]

    def close(self) -> None:
        self._stop.set()
        self._t.join()
        self.bus.shutdown()


def isotp_socket(channel: str) -> socket.socket:
    s = socket.socket(socket.AF_CAN, socket.SOCK_DGRAM, socket.CAN_ISOTP)
    # struct can_isotp_options: flags, frame_txtime, ext_address, txpad, rxpad, rx_ext_address
    s.setsockopt(SOL_CAN_ISOTP, CAN_ISOTP_OPTS,
                 struct.pack("=IIBBBB", CAN_ISOTP_TX_PADDING, 0, 0, PAD, 0, 0))
    # struct can_isotp_ll_options: mtu (CANFD_MTU), tx_dl, tx_flags
    s.setsockopt(SOL_CAN_ISOTP, CAN_ISOTP_LL_OPTS, struct.pack("=BBB", 72, 64, CANFD_BRS))
    s.bind((channel, TX_ID, RX_ID))   # (interface, rx_addr, tx_addr)
    s.settimeout(3.0)
    return s


def request(s: socket.socket, req: bytes) -> bytes:
    s.send(req)
    try:
        return s.recv(4096)
    except socket.timeout:
        return b""


def fd_frames_ok(frames: List[can.Message], what: str) -> None:
    check(bool(frames) and all(m.is_fd for m in frames), f"{what}: every frame is CAN FD")
    check(bool(frames) and all(m.bitrate_switch for m in frames), f"{what}: BRS on every frame")
    check(all(len(m.data) in FD_DLS or len(m.data) <= 8 for m in frames),
          f"{what}: every length is a valid CAN FD data length")


def main() -> int:
    exe, channel = sys.argv[1], sys.argv[2]
    ecu = subprocess.Popen([exe], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        time.sleep(1.5)
        sn = Sniffer(channel)
        s = isotp_socket(channel)

        # 1. Escape Single Frame.
        mk = sn.mark()
        rsp = request(s, b"\x22\xF1\x90")
        check(rsp == b"\x62\xF1\x90" + IDENT, f"22 F190 -> 62 F190 + 17 bytes ({rsp[:3].hex()} ..)")
        out = sn.since(mk, TX_ID)
        fd_frames_ok(out, "22 F190 answer")
        check(len(out) == 1 and out[0].data[0] == 0x00 and out[0].data[1] == 20
              and len(out[0].data) == 24 and all(b == PAD for b in out[0].data[22:]),
              "one escape SF: 00 14, 24 bytes, padded")

        # 2. Multi-frame answer.
        mk = sn.mark()
        rsp = request(s, b"\x22\x01\x00")
        check(rsp == b"\x62\x01\x00" + RECORDS[0x0100], f"22 0100 -> 67 bytes ({len(rsp)})")
        out = sn.since(mk, TX_ID)
        fd_frames_ok(out, "22 0100 answer")
        check(len(out) == 2 and len(out[0].data) == 64 and out[0].data[:2] == b"\x10\x43"
              and len(out[1].data) <= 8,
              "FF 10 43 of 64 bytes, then one CF of at most 8 bytes")

        # 3. Multi-frame request and a long multi-frame answer.
        dids = [0x0100, 0x0101, 0x0102] * 10 + [0x0100, 0x0101]
        req = b"\x22" + b"".join(struct.pack(">H", d) for d in dids)
        want = b"\x62" + b"".join(struct.pack(">H", d) + RECORDS[d] for d in dids)
        mk = sn.mark()
        t0 = time.monotonic()
        rsp = request(s, req)
        dt = (time.monotonic() - t0) * 1000.0
        check(len(req) == 65 and rsp == want,
              f"65-byte request, {len(want)}-byte answer, byte-exact ({len(rsp)} bytes, {dt:.0f} ms)")
        up = sn.since(mk, RX_ID)
        down = sn.since(mk, TX_ID)
        fd_frames_ok(up, "Linux request (FF + CF) and its FC")
        fd_frames_ok(down, "ECU FC and answer")
        fcs = [m for m in down if (m.data[0] >> 4) == 3]
        check(bool(fcs) and fcs[0].data[:3] == b"\x30\x10\x00",
              f"ECU FC 30 10 00 (BS 16, STmin 0) as an FD frame ({fcs[0].data[:3].hex() if fcs else '-'})")
        cfs = [m for m in down if (m.data[0] >> 4) == 2]
        # 2112 = 62 + 32 * 63 + 34: 33 CFs, the last carries 34 bytes -> 35, padded to 48.
        check(len(cfs) == 33 and all(len(m.data) == 64 for m in cfs[:-1])
              and len(cfs[-1].data) == 48,
              f"answer: 33 CFs of 64 bytes, the last 48 ({len(cfs)} CFs)")
        linux_fc = [m for m in up if (m.data[0] >> 4) == 3]
        check(len(linux_fc) >= 1, f"Linux sent {len(linux_fc)} FD Flow Control frame(s); the ECU honoured them")

        # 4. Periodic messages, 20-byte record, one FD frame each.
        check(request(s, b"\x10\x03")[:2] == b"\x50\x03", "10 03 -> 50 03")
        mk = sn.mark()
        rsp = request(s, b"\x2A\x03\x01")
        check(rsp == b"\x6A", f"2A 03 01 -> 6A ({rsp.hex()})")
        time.sleep(0.5)
        per = sn.since(mk, PERIODIC_ID)
        check(len(per) >= 10, f"periodic messages on 0x5E8 ({len(per)} in 0.5 s at the fast rate)")
        fd_frames_ok(per, "periodic messages")
        check(all(len(m.data) == 24 and m.data[0] == 0x01
                  and bytes(m.data[5:21]) == bytes(range(0xA0, 0xB0))
                  and all(b == PAD for b in m.data[21:]) for m in per),
              "each: 24 bytes = [01, uptime(4), A0..AF, pad]: no PCI, no 6A")
        ups = [struct.unpack(">I", bytes(m.data[1:5]))[0] for m in per]
        check(len(ups) >= 2 and all(b > a for a, b in zip(ups, ups[1:])), "uptime advances")
        check(not any(m.data[:1] == b"\x6A" for m in sn.since(mk, TX_ID)),
              "nothing periodic on the response ID 0x7E8")
        check(request(s, b"\x2A\x04\x01") == b"\x6A", "2A 04 01 -> 6A (stop)")
        time.sleep(0.2)
        mk = sn.mark()
        check(len(sn.since(mk, PERIODIC_ID)) == 0, "no periodic message after stopSending")

        s.close()
        sn.close()
    finally:
        ecu.terminate()
        try:
            ecu.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            ecu.kill()

    if failures:
        print(f"\n{len(failures)} check(s) failed")
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
