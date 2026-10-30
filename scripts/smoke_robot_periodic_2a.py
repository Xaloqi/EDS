#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Runtime check of SID 0x2A (ReadDataByPeriodicIdentifier) against the compiled
stack: examples/robot_joint_controller_ecu on native_sim, DID 0xF200 (pDID 0x00).

Before this, the 0x2A positive path was only exercised by unit tests with mock
DIDs and by the simulator model; no example defined a 0xF2xx DID.

Over raw CAN frames (standard library only) it checks:
  - default session refuses 0x2A (7F 2A 7F); unknown pDID is 7F 2A 31
  - 2A <mode> 00 answers 6A, then pushes frames at the slow / medium / fast
    rate (1000 / 100 / 10 ms, median interval within a tolerance)
  - the periodic message format: [periodicDataIdentifier, <int32 BE position>]
    without the 0x6A SID (ISO 14229-1:2026 Table 221), today still carried in an
    ISO-TP single frame [05, 00, <int32>] (DLC 6) on the physical response ID 0x7E8
  - the first message follows the positive response at the next tick, not a full
    period later (§10.5.5, Figures 25/26)
  - 2A 04 00 and 2A 04 (stop all) stop it; a re-subscribe changes the rate in
    place; returning to the default session cancels every subscription

LINK LAYER (open, Xaloqi/EDS#546): ISO 14229-1 gives the periodic messages their
own allocated response IDs and leaves the CAN mapping to ISO 14229-3. The stack
still sends them through ISO-TP on the diagnostic response ID. FRAME_* below pin
that, so the change is visible when it is fixed.

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
CAN_FMT = "=IB3x8s"

# Link layer as sent today (Xaloqi/EDS#546): ISO-TP SF on the response ID.
FRAME_ID = RSP_ID
FRAME_PREFIX = bytes([0x05, 0x00])         # ISO-TP SF len 5, pDID 0x00 (no 0x6A)
FRAME_LEN = 6                               # 2 + int32, DLC 6, no padding

SWEEP_AMPL_MDEG = 90000                     # joint_sim.h


def send(sock, req):
    frame = bytes([len(req)]) + bytes(req)
    sock.send(struct.pack(CAN_FMT, REQ_ID, len(frame), frame.ljust(8, b"\0")))


def collect(sock, seconds):
    """All frames from the ECU for `seconds`: [(t, can_id, data)]."""
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
        if (cid & 0x1FFFFFFF) in (RSP_ID,):
            out.append((time.monotonic() - t0, cid & 0x1FFFFFFF, data[:dlc]))


def uds(sock, req, wait=0.25):
    """Send a request; return the SF payload of the response (periodic frames
    that arrive meanwhile are skipped) and any later non-periodic frames."""
    send(sock, req)
    frames = [f for f in collect(sock, wait) if not is_periodic(f[2])]
    assert frames, "no response to %s" % bytes(req).hex()
    first = frames[0][2]
    assert first[0] >> 4 == 0, "not an SF: %s" % first.hex()
    return first[1:1 + (first[0] & 0x0F)], frames[1:]


def is_periodic(data):
    return len(data) == FRAME_LEN and data.startswith(FRAME_PREFIX)


def stream(sock, seconds):
    fr = [f for f in collect(sock, seconds) if is_periodic(f[2])]
    return fr, [b[0] - a[0] for a, b in zip(fr, fr[1:])]


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

        rsp, _ = uds(sock, [0x2A, 0x02, 0x00])
        assert rsp == b"\x7F\x2A\x7F", "default session: %s" % rsp.hex()
        print("PASS default session: 7F 2A 7F")

        assert uds(sock, [0x10, 0x03])[0][:2] == b"\x50\x03"
        rsp, _ = uds(sock, [0x2A, 0x02, 0x55])
        assert rsp == b"\x7F\x2A\x31", "unknown pDID: %s" % rsp.hex()
        print("PASS unknown pDID 0x55: 7F 2A 31")

        for mode, name, period, secs in ((0x01, "slow", 1.0, 3.6),
                                         (0x02, "medium", 0.1, 1.0),
                                         (0x03, "fast", 0.01, 0.4)):
            send(sock, [0x2A, mode, 0x00])
            fr = collect(sock, secs)
            assert fr and fr[0][2] == b"\x01\x6A", \
                "%s: positive response missing/first: %r" % (name, fr[:1])
            frames = [f for f in fr[1:] if is_periodic(f[2])]
            assert len(frames) == len(fr) - 1, "%s: a frame is not %s..." % (name, FRAME_PREFIX.hex())
            gaps = [b[0] - a[0] for a, b in zip(frames, frames[1:])]
            assert len(frames) >= int(secs / period * 0.8) - 1, \
                "%s: only %d frames in %.1f s" % (name, len(frames), secs)
            first = frames[0][0] - fr[0][0]
            assert first < 0.05, \
                "%s: first message %.0f ms after the response, want the next tick" % (name, first * 1e3)
            med = statistics.median(gaps)
            assert 0.8 * period <= med <= 1.3 * period, \
                "%s: median interval %.1f ms, want ~%.0f" % (name, med * 1e3, period * 1e3)
            for t, cid, d in frames:
                assert cid == FRAME_ID
                pos = int.from_bytes(d[2:6], "big", signed=True)
                assert abs(pos) <= SWEEP_AMPL_MDEG, "position out of sweep: %d" % pos
            print("PASS %s: %d frames, median %.1f ms, frame %s" %
                  (name, len(frames), med * 1e3, frames[0][2].hex()))
            rsp, _ = uds(sock, [0x2A, 0x04, 0x00])
            assert rsp == b"\x6A", "stop: %s" % rsp.hex()
            assert not stream(sock, 0.35)[0], "%s: frames after stopSending" % name

        # Re-subscribe changes the rate in place: one stream, not two.
        send(sock, [0x2A, 0x01, 0x00]); collect(sock, 0.05)
        send(sock, [0x2A, 0x02, 0x00]); collect(sock, 0.05)
        fr, gaps = stream(sock, 0.6)
        assert gaps and statistics.median(gaps) < 0.15 and len(fr) <= 7, \
            "re-subscribe: %d frames, gaps %r" % (len(fr), gaps[:4])
        print("PASS re-subscribe slow -> medium: one stream, %d frames/0.6 s" % len(fr))

        # stopSending with no pDID stops everything.
        rsp, _ = uds(sock, [0x2A, 0x04])
        assert rsp == b"\x6A", "stop all: %s" % rsp.hex()
        assert not stream(sock, 0.35)[0]
        print("PASS 2A 04 (no pDID) stops all")

        # Back to the default session cancels every subscription.
        send(sock, [0x2A, 0x03, 0x00]); collect(sock, 0.05)
        uds(sock, [0x10, 0x01], wait=0.1)
        assert not stream(sock, 0.4)[0], "frames after return to default session"
        print("PASS default session cancels the subscription")
    finally:
        ecu.terminate()
        ecu.wait(timeout=5)
    print("PERIODIC 0x2A OK")


if __name__ == "__main__":
    main()
