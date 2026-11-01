#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Xaloqi
"""[EDS#282] End to end: the server's response-pending path over SocketCAN.

Drives examples/basic_ecu on native_sim, built with the bench-only slow
routine (RID 0xF0F0, -DDIAG_BENCH_SLOW_ROUTINE=ON) and the real-CAN overlay,
with the free tester (xaloqi-tester >= 1.5.10, which waits through 0x78)
plus raw frames for the behaviour checks the tester cannot express.

Build (from a west workspace):

    W=<EDS checkout>
    ZEPHYR_TOOLCHAIN_VARIANT=host west build -p always -b native_sim $W/examples/basic_ecu -- \\
      -DDIAG_SKIP_CODEGEN=ON -DDIAG_BENCH_SLOW_ROUTINE=ON \\
      "-DEXTRA_CONF_FILE=$W/boards/native_sim/native_sim.conf;$W/boards/native_sim/native_sim_realcan.conf" \\
      "-DDTC_OVERLAY_FILE=$W/boards/native_sim/native_sim.overlay;$W/boards/native_sim/native_sim_realcan.overlay"

Run (no sudo: a user + network namespace with its own vcan):

    unshare -rn sh -c 'ip link add zcan0 type vcan && ip link set zcan0 up &&
                       python3 scripts/e2e_response_pending.py build/zephyr/zephyr.exe zcan0'

Exit 0 when every check passes. Timing here is only loosely bounded (host
scheduling); the Pi 5 CAN bench is where the 0x78 cadence is measured.
"""

from __future__ import annotations

import asyncio
import subprocess
import sys
import time
from typing import List, Optional, Tuple

import can
from xaloqi.tester import UdsTester, Session
from xaloqi.tester.transport.base import CanBus

RX_ID = 0x7DF            # basic_ecu physical request ID (GEN_CAN_RX_ID)
TX_ID = 0x7E8
RID = 0xF0F0
P2_STAR_MS = 5000.0

failures: List[str] = []


def check(cond: bool, what: str) -> None:
    print(("PASS  " if cond else "FAIL  ") + what)
    if not cond:
        failures.append(what)


class SocketCanBus(CanBus):
    """Minimal python-can adapter for the free tester's CanBus interface."""

    def __init__(self, channel: str) -> None:
        self._bus = can.Bus(interface="socketcan", channel=channel,
                            receive_own_messages=False)

    async def send(self, arbitration_id: int, data: bytes) -> None:
        msg = can.Message(arbitration_id=arbitration_id, data=data, is_extended_id=False)
        await asyncio.get_running_loop().run_in_executor(None, self._bus.send, msg)

    async def recv(self, timeout: float) -> Optional[Tuple[int, bytes]]:
        msg = await asyncio.get_running_loop().run_in_executor(None, self._bus.recv, timeout)
        if msg is None:
            return None
        return msg.arbitration_id, bytes(msg.data)

    async def close(self) -> None:
        self._bus.shutdown()


def sf(payload: bytes) -> bytes:
    """Single frame, 0xCC-padded."""
    return (bytes([len(payload)]) + payload).ljust(8, b"\xCC")


def collect(bus: can.BusABC, until_s: float, stop_on=None) -> List[Tuple[float, bytes]]:
    """Responses on TX_ID as (time, UDS payload) until a deadline."""
    out: List[Tuple[float, bytes]] = []
    deadline = time.monotonic() + until_s
    while time.monotonic() < deadline:
        msg = bus.recv(0.05)
        if msg is None or msg.arbitration_id != TX_ID:
            continue
        pci = msg.data[0]
        if (pci >> 4) != 0:
            continue            # single frames only in this test
        uds = bytes(msg.data[1:1 + (pci & 0x0F)])
        out.append((time.monotonic(), uds))
        if stop_on is not None and stop_on(uds):
            break
    return out


async def tester_run(channel: str) -> None:
    bus = SocketCanBus(channel)
    async with UdsTester(bus, rx_id=TX_ID, tx_id=RX_ID, keepalive=True,
                         p2_star_timeout=P2_STAR_MS / 1000.0 + 0.5) as t:
        await t.session(Session.EXTENDED)

        # 3 steps of 1 s: one 0x78 at once, then a keepalive every ~2.5 s.
        r = await t.routine_control(RID, option_record=bytes([3, 100]))
        check(r.status_record == bytes([3]), "free tester: 71 01 F0 F0 03 after the job")
        check(r.pending.count >= 1, f"free tester: saw {r.pending.count} x 0x78")
        gaps = r.pending.gaps_ms[1:]
        check(all(g < P2_STAR_MS for g in gaps),
              f"free tester: every gap after the first 0x78 < P2* ({[round(g) for g in gaps]})")
        check(r.pending.gaps_ms[0] < 50.0,
              f"free tester: first 0x78 within P2 ({r.pending.gaps_ms[0]:.1f} ms)")

        # 8 steps of 1.5 s = 12 s, with the tester's own 3E 80 every 2 s
        # running through it: accepted silently, the job carries on.
        r = await t.routine_control(RID, option_record=bytes([8, 150]))
        check(r.status_record == bytes([8]), "free tester: 12 s job through 3E 80 keepalives")
        check(r.pending.count >= 4, f"free tester: {r.pending.count} x 0x78 over 12 s")
    await bus.close()


def raw_run(channel: str) -> None:
    bus = can.Bus(interface="socketcan", channel=channel)
    try:
        bus.send(can.Message(arbitration_id=RX_ID, data=sf(b"\x10\x03"), is_extended_id=False))
        collect(bus, 0.3)

        # Start a 6 x 1 s job, then a 22 F1 90 and a 10 01 in the middle.
        t0 = time.monotonic()
        bus.send(can.Message(arbitration_id=RX_ID, data=sf(bytes([0x31, 0x01, 0xF0, 0xF0, 6, 100])),
                             is_extended_id=False))
        first = collect(bus, 0.5, stop_on=lambda u: True)
        check(bool(first) and first[0][1] == b"\x7F\x31\x78", "raw: first answer 7F 31 78")
        time.sleep(0.3)
        bus.send(can.Message(arbitration_id=RX_ID, data=sf(b"\x22\xF1\x90"), is_extended_id=False))
        # The ECU reads CAN between steps: allow one 1 s step plus margin.
        busy = collect(bus, 2.5, stop_on=lambda u: u[:2] == b"\x7F\x22")
        check(any(u == b"\x7F\x22\x21" for _, u in busy),
              f"raw: 22 F1 90 mid-job -> 7F 22 21 (got {[u.hex(' ') for _, u in busy]})")
        bus.send(can.Message(arbitration_id=RX_ID, data=sf(b"\x10\x01"), is_extended_id=False))
        busy = collect(bus, 2.5, stop_on=lambda u: u[:2] == b"\x7F\x10" or u[:1] == b"\x50")
        check(any(u == b"\x7F\x10\x21" for _, u in busy),
              f"raw: 10 01 mid-job -> 7F 10 21 (got {[u.hex(' ') for _, u in busy]})")
        bus.send(can.Message(arbitration_id=RX_ID, data=sf(b"\x3E\x80"), is_extended_id=False))
        rest = collect(bus, 8.0, stop_on=lambda u: u[0] == 0x71)
        check(not any(u[:1] == b"\x7E" or u[:2] == b"\x7F\x3E" for _, u in rest),
              "raw: 3E 80 mid-job -> no answer")
        final = [u for _, u in rest if u[0] == 0x71]
        check(final == [bytes([0x71, 0x01, 0xF0, 0xF0, 6])], "raw: job unaffected, final 71 01 F0 F0 06")
        check(time.monotonic() - t0 >= 6.0, "raw: final after the 6 s of work")

        # Out-of-range option record: decided before deferral, no 0x78.
        bus.send(can.Message(arbitration_id=RX_ID, data=sf(bytes([0x31, 0x01, 0xF0, 0xF0, 0, 100])),
                             is_extended_id=False))
        nrc = collect(bus, 0.5, stop_on=lambda u: True)
        check(bool(nrc) and nrc[0][1] == b"\x7F\x31\x31", "raw: bad option record -> 7F 31 31, no 0x78")

        # Served normally once the job is over.
        bus.send(can.Message(arbitration_id=RX_ID, data=sf(b"\x3E\x00"), is_extended_id=False))
        tp = collect(bus, 0.5, stop_on=lambda u: True)
        check(bool(tp) and tp[0][1][:1] == b"\x7E", "raw: 3E 00 answered after the job")
    finally:
        bus.shutdown()


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    exe = sys.argv[1]
    channel = sys.argv[2] if len(sys.argv) > 2 else "zcan0"
    ecu = subprocess.Popen([exe], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.5)
        asyncio.run(tester_run(channel))
        raw_run(channel)
    finally:
        ecu.terminate()
        ecu.wait(timeout=5)
    print(f"\n{'FAIL' if failures else 'PASS'}: {len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
