#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Runtime check of SID 0x2A (ReadDataByPeriodicIdentifier) against the compiled
stack: examples/robot_joint_controller_ecu on native_sim, DID 0xF200 (pDID 0x00).

Over raw CAN frames (standard library only) it checks:
  - default session refuses 0x2A (7F 2A 7F); an unknown pDID is 7F 2A 31
  - 2A <mode> 00 answers 6A on the response ID 0x7E8, then pushes messages at the
    slow / medium / fast rate (1000 / 100 / 10 ms, median interval within a
    tolerance); the first one follows at the next tick, not a period later
  - the periodic message is UUDT (ISO 14229-3:2022 §8.5, Table 3, Table 4): on
    the periodic CAN ID 0x5E8 (can.periodic_tx_can_id), NOT on 0x7E8, with no
    PCI byte and no 0x6A: [pDID, <int32 BE position>], DLC 5
  - it does not go through ISO-TP: periodic messages keep their rate while a
    multi-frame response waits for Flow Control
  - 2A 04 00 and 2A 04 (stop all) stop it; a re-subscribe changes the rate in
    place; returning to the default session cancels every subscription

Usage: smoke_robot_periodic_2a.py <path/to/zephyr.exe> [interface]
The interface must exist and be up (see scripts/smoke_robot_joint_ecu.py).
"""

import socket
import statistics
import struct
import subprocess
import sys
import time

REQ_ID = 0x7E0
RSP_ID = 0x7E8
PERIODIC_ID = 0x5E8                         # can.periodic_tx_can_id of the example
CAN_FMT = "=IB3x8s"
SWEEP_AMPL_MDEG = 90000                     # joint_sim.h


def send(sock, req, can_id=REQ_ID):
    frame = bytes([len(req)]) + bytes(req)
    sock.send(struct.pack(CAN_FMT, can_id, len(frame), frame.ljust(8, b"\0")))


def send_raw(sock, data):
    sock.send(struct.pack(CAN_FMT, REQ_ID, len(data), bytes(data).ljust(8, b"\0")))


def collect(sock, seconds):
    """Frames from the ECU for `seconds`: [(t, can_id, data)]."""
    out, t0 = [], time.monotonic()
    while True:
        left = seconds - (time.monotonic() - t0)
        if left <= 0:
            return out
        sock.settimeout(left)
        try:
            raw = sock.recv(16)
        except socket.timeout:
            return out
        cid, dlc, data = struct.unpack(CAN_FMT, raw)
        cid &= 0x1FFFFFFF
        if cid in (RSP_ID, PERIODIC_ID):
            out.append((time.monotonic() - t0, cid, data[:dlc]))


def periodic(frames):
    return [f for f in frames if f[1] == PERIODIC_ID]


def responses(frames):
    return [f for f in frames if f[1] == RSP_ID]


def uds(sock, req, wait=0.25):
    """Send a request; return the SF payload of the response on 0x7E8."""
    send(sock, req)
    rs = responses(collect(sock, wait))
    assert rs, "no response to %s" % bytes(req).hex()
    first = rs[0][2]
    assert first[0] >> 4 == 0, "not an SF: %s" % first.hex()
    return first[1:1 + (first[0] & 0x0F)]


def check_message(cid, d):
    assert cid == PERIODIC_ID, "periodic message on 0x%X" % cid
    assert len(d) == 5 and d[0] == 0x00, "not [pDID 00, int32] (no PCI, no 0x6A): %s" % d.hex()
    pos = int.from_bytes(d[1:5], "big", signed=True)
    assert abs(pos) <= SWEEP_AMPL_MDEG, "position out of sweep: %d" % pos


def main():
    exe = sys.argv[1]
    iface = sys.argv[2] if len(sys.argv) > 2 else "zcan0"
    sock = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    sock.bind((iface,))
    ecu = subprocess.Popen([exe], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(50):
            try:
                uds(sock, [0x3E, 0x00], wait=0.2)
                break
            except AssertionError:
                time.sleep(0.1)
        else:
            raise SystemExit("FAIL: ECU did not answer on %s" % iface)

        assert uds(sock, [0x2A, 0x02, 0x00]) == b"\x7F\x2A\x7F"
        print("PASS default session: 7F 2A 7F")

        assert uds(sock, [0x10, 0x03])[:2] == b"\x50\x03"
        assert uds(sock, [0x2A, 0x02, 0x55]) == b"\x7F\x2A\x31"
        print("PASS unknown pDID 0x55: 7F 2A 31")

        for mode, name, period, secs in ((0x01, "slow", 1.0, 3.6),
                                         (0x02, "medium", 0.1, 1.0),
                                         (0x03, "fast", 0.01, 0.4)):
            send(sock, [0x2A, mode, 0x00])
            fr = collect(sock, secs)
            rs = responses(fr)
            assert rs and rs[0][2] == b"\x01\x6A" and len(rs) == 1, \
                "%s: want exactly the positive response on 0x7E8, got %r" % (name, rs)
            msgs = periodic(fr)
            assert msgs, "%s: no periodic message on 0x%X" % (name, PERIODIC_ID)
            assert fr[0] == rs[0], "%s: a periodic message came before the positive response" % name
            for t, cid, d in msgs:
                check_message(cid, d)
            gaps = [b[0] - a[0] for a, b in zip(msgs, msgs[1:])]
            assert len(msgs) >= int(secs / period * 0.8) - 1, \
                "%s: only %d messages in %.1f s" % (name, len(msgs), secs)
            first = msgs[0][0] - rs[0][0]
            assert first < 0.05, \
                "%s: first message %.0f ms after the response, want the next tick" % (name, first * 1e3)
            med = statistics.median(gaps)
            assert 0.8 * period <= med <= 1.3 * period, \
                "%s: median interval %.1f ms, want ~%.0f" % (name, med * 1e3, period * 1e3)
            print("PASS %s: %d messages, median %.1f ms, 0x%X %s" %
                  (name, len(msgs), med * 1e3, PERIODIC_ID, msgs[0][2].hex()))
            assert uds(sock, [0x2A, 0x04, 0x00]) == b"\x6A"
            assert not periodic(collect(sock, 0.35)), "%s: messages after stopSending" % name

        # Not through ISO-TP: a multi-frame response waiting for Flow Control
        # does not hold the periodic stream back.
        send(sock, [0x2A, 0x03, 0x00]); collect(sock, 0.05)
        send(sock, [0x22, 0xF1, 0x90])                      # 17-byte VIN: First Frame
        fr = collect(sock, 0.25)                            # no Flow Control sent yet
        ff = [f for f in responses(fr) if f[2][0] >> 4 == 1]
        assert ff, "no First Frame for 22 F1 90: %r" % responses(fr)
        assert len(periodic(fr)) >= 15, \
            "periodic stream stalled behind a multi-frame response: %d messages" % len(periodic(fr))
        send_raw(sock, [0x30, 0x00, 0x00])                  # Flow Control CTS
        fr = collect(sock, 0.2)
        assert [f for f in responses(fr) if f[2][0] >> 4 == 2], "no Consecutive Frames after FC"
        assert periodic(fr), "periodic stream stopped during the transfer"
        assert uds(sock, [0x2A, 0x04]) == b"\x6A"
        print("PASS periodic messages keep their rate while a multi-frame response waits for FC")

        # Re-subscribe changes the rate in place: one stream, not two.
        send(sock, [0x2A, 0x01, 0x00]); collect(sock, 0.05)
        send(sock, [0x2A, 0x02, 0x00]); collect(sock, 0.05)
        msgs = periodic(collect(sock, 0.6))
        gaps = [b[0] - a[0] for a, b in zip(msgs, msgs[1:])]
        assert gaps and statistics.median(gaps) < 0.15 and len(msgs) <= 7, \
            "re-subscribe: %d messages, gaps %r" % (len(msgs), gaps[:4])
        print("PASS re-subscribe slow -> medium: one stream, %d messages/0.6 s" % len(msgs))

        assert uds(sock, [0x2A, 0x04]) == b"\x6A"
        assert not periodic(collect(sock, 0.35))
        print("PASS 2A 04 (no pDID) stops all")

        send(sock, [0x2A, 0x03, 0x00]); collect(sock, 0.05)
        uds(sock, [0x10, 0x01], wait=0.1)
        assert not periodic(collect(sock, 0.4)), "messages after return to default session"
        print("PASS default session cancels the subscription")
    finally:
        ecu.terminate()
        ecu.wait(timeout=5)
    print("PERIODIC 0x2A OK")


if __name__ == "__main__":
    main()
