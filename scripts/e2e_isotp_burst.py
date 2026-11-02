#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Xaloqi
"""[EDS#558] End to end: a long ISO-TP request sent as fast as the ECU's Flow
Control allows is reassembled, on classic CAN and on CAN FD.

The sender here does exactly what ISO 15765-2 permits and nothing slower: it
waits for the FC after the First Frame, then sends Consecutive Frames back to
back (STmin 0 means no gap), and stops for a new FC only after BS frames when
the ECU advertised BS != 0. With BS 0 that is the whole 4000-byte message in
one burst, which an 8-frame RX queue drained one frame per tick cannot hold.

The request is a 4000-byte 0x2E. Its content does not matter: any response
(here a negative one) proves the ECU reassembled all of it. A lost CF shows
up as no response at all (reassembly waits for N_Cr, then drops the PDU).

Build basic_ecu on native_sim with the real-CAN overlay (add, for CAN FD,
-DCONFIG_CAN_FD_MODE=y -DEXTRA_CFLAGS=-DISOTP_ENABLE_CAN_FD=1):

    W=<EDS checkout>
    ZEPHYR_TOOLCHAIN_VARIANT=host west build -p always -b native_sim $W/examples/basic_ecu -- \\
      -DDIAG_SKIP_CODEGEN=ON \\
      "-DEXTRA_CONF_FILE=$W/boards/native_sim/native_sim.conf;$W/boards/native_sim/native_sim_realcan.conf" \\
      "-DDTC_OVERLAY_FILE=$W/boards/native_sim/native_sim.overlay;$W/boards/native_sim/native_sim_realcan.overlay"

Run (no sudo: a user + network namespace with its own vcan; mtu 72 for FD):

    unshare -rn sh -c 'ip link add zcan0 type vcan && ip link set zcan0 mtu 72 up &&
                       python3 scripts/e2e_isotp_burst.py build/zephyr/zephyr.exe zcan0 [--fd]'

Exit 0 when every request is answered.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from typing import List, Optional

import can

RX_ID = 0x7DF            # basic_ecu request ID (GEN_CAN_RX_ID)
TX_ID = 0x7E8            # basic_ecu response ID (GEN_CAN_TX_ID)
REQ_LEN = 4000           # fits the 12-bit FF_DL, so classic and FD use the same FF
FD_DLS = (8, 12, 16, 20, 24, 32, 48, 64)
FC_TIMEOUT_S = 1.0       # N_Bs
RESP_TIMEOUT_S = 3.0     # past the ECU's N_Cr (1000 ms), so a lost CF cannot pass

failures: List[str] = []


def check(cond: bool, what: str) -> None:
    print(("PASS  " if cond else "FAIL  ") + what)
    if not cond:
        failures.append(what)


def fd_len(n: int) -> int:
    return next(dl for dl in FD_DLS if dl >= n)


class IsoTpSender:
    def __init__(self, bus: can.BusABC, fd: bool) -> None:
        self.bus = bus
        self.fd = fd
        self.frame_len = 64 if fd else 8
        self.fcs: List[bytes] = []

    def _send(self, data: bytes) -> None:
        if self.fd:
            data = data + b"\xCC" * (fd_len(len(data)) - len(data))
        else:
            data = data + b"\xCC" * (8 - len(data))
        self.bus.send(can.Message(arbitration_id=RX_ID, data=data,
                                  is_extended_id=False, is_fd=self.fd))

    def _recv(self, deadline: float) -> Optional[bytes]:
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            msg = self.bus.recv(timeout=left)
            if msg is not None and msg.arbitration_id == TX_ID:
                return bytes(msg.data)

    def _wait_cts(self) -> Optional[bytes]:
        deadline = time.monotonic() + FC_TIMEOUT_S
        while True:
            fc = self._recv(deadline)
            if fc is None or (fc[0] & 0xF0) != 0x30:
                return None
            self.fcs.append(fc[:3])
            status = fc[0] & 0x0F
            if status == 0:
                return fc
            if status != 1:          # overflow / reserved: abort
                return None
            deadline = time.monotonic() + FC_TIMEOUT_S

    def request(self, payload: bytes) -> Optional[bytes]:
        n = len(payload)
        first = self.frame_len - 2
        self._send(bytes([0x10 | (n >> 8), n & 0xFF]) + payload[:first])
        pos, sn = first, 1
        while pos < n:
            fc = self._wait_cts()
            if fc is None:
                return None
            bs, stmin = fc[1], fc[2]
            gap = stmin / 1000.0 if stmin <= 0x7F else 0.0001
            sent = 0
            while pos < n and (bs == 0 or sent < bs):
                chunk = payload[pos:pos + self.frame_len - 1]
                self._send(bytes([0x20 | (sn & 0x0F)]) + chunk)
                pos, sn, sent = pos + len(chunk), sn + 1, sent + 1
                if gap:
                    time.sleep(gap)
        return self._read_response(time.monotonic() + RESP_TIMEOUT_S)

    def _read_response(self, deadline: float) -> Optional[bytes]:
        first = self._recv(deadline)
        if first is None:
            return None
        pci = first[0] >> 4
        if pci == 0:
            ln = first[0] & 0x0F
            return first[1:1 + ln] if ln else first[2:2 + first[1]]
        return first             # multi-frame answer: not expected for this request


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("channel")
    ap.add_argument("--fd", action="store_true", help="send the request as CAN FD frames")
    ap.add_argument("--rounds", type=int, default=5)
    args = ap.parse_args()

    ecu = subprocess.Popen([args.exe], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log: List[str] = []
    try:
        time.sleep(1.5)
        bus = can.Bus(interface="socketcan", channel=args.channel,
                      receive_own_messages=False, fd=args.fd)
        tx = IsoTpSender(bus, args.fd)
        kind = "CAN FD" if args.fd else "classic CAN"
        payload = bytes([0x2E, 0xF1, 0x90]) + bytes(i & 0xFF for i in range(REQ_LEN - 3))
        for r in range(args.rounds):
            t0 = time.monotonic()
            rsp = tx.request(payload)
            dt = (time.monotonic() - t0) * 1000.0
            check(rsp is not None and len(rsp) >= 2 and rsp[0] in (0x7F, 0x6E),
                  f"{kind} round {r + 1}: {REQ_LEN}-byte 0x2E answered "
                  f"({rsp.hex() if rsp else 'no response'}, {dt:.0f} ms)")
            bus.recv(timeout=0.2)
        if tx.fcs:
            print("FC seen (PCI BS STmin): " + ", ".join(sorted({f.hex() for f in tx.fcs})))
        bus.shutdown()
    finally:
        ecu.terminate()
        try:
            out, _ = ecu.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            ecu.kill()
            out, _ = ecu.communicate()
        log = out.decode(errors="replace").splitlines()

    overflows = [ln for ln in log if "overflowed" in ln]
    check(not overflows, f"no CAN RX queue overflow logged ({len(overflows)} lines)")
    if overflows:
        print("  " + overflows[0])
    if failures:
        print(f"\n{len(failures)} check(s) failed")
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
