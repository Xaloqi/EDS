#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Xaloqi
"""[EDS#197] examples/dual_transport_ecu on native_sim: one ECU, a CAN tester
and a DoIP tester at once.

CAN: the Linux kernel's ISO-TP stack on the vcan zcan0 (0x7E0 -> 0x7E8). DoIP:
TCP to 192.0.2.1:13400 over the TAP zeth. Both talk to the same UDS server,
through the diagnostic executor.

  1. Session ownership (decision A): CAN opens 10 03; DoIP's 10 03 and 3E 00
     get 7F xx 21. CAN returns to default; DoIP opens 10 03 and owns it; CAN's
     3E 00 gets 7F 3E 21. DoIP disconnects: its session ends, and CAN's 10 03
     is accepted again.
  2. Both at once: a CAN thread and a DoIP thread each send ROUNDS 3E 00 in the
     default session. Every request gets exactly one answer, 7E 00 or 7F 3E 21.
  3. The January "done when" (EDS#197 J8): the same two threads send a mix that
     competes for the session (3E 00, 22 F190, 22 F187 which needs an extended
     session, 10 03, 10 01), MIX_ROUNDS each, one request outstanding per
     transport. A transport that got 50 03 owns the session until its own
     10 01: every request it sends meanwhile must be answered positively. A
     transport that does not own it is answered positively or 7F sid 21, and
     22 F187 is never positive for it (7F 22 31 or 7F 22 21). So every physical
     request is answered or refused 0x21, and no transport's session is ended
     by the other.
  4. DoIP churn: while CAN owns an extended session, DoIP connects, sends a
     request (7F sid 21), disconnects, and reconnects, ten times. Each
     reconnection proves the server handled the previous disconnect; CAN's
     22 F187 must still be positive after it (a DoIP disconnect never ends
     CAN's session).
  5. Latency: every request of phases 2 and 3, timed by the tester, p99 below
     the example's P2server_max (diagnostics_config.yaml, timing).
  6. ECUReset over CAN: 11 01 is answered 51 01, then the ECU resets (on
     native_sim the process exits: "sys_arch_reboot called"). Its log carries
     the server's own request_count, which must equal the number of requests
     the testers sent (none of the mix reaches a job step, so the producers
     answer none locally): read from the ECU, not inferred.

The pending-job case of EDS#664 (a DoIP disconnect in an unowned default
session leaves a job CAN started) needs a routine that defers in the default
session; this example has none, so tests/unit_runnable/test_uds_exec.c covers
it (test_foreign_disconnect_in_unowned_session_leaves_job).

Build (from a west workspace):

    W=<EDS checkout>; E=$W/examples/dual_transport_ecu
    ZEPHYR_TOOLCHAIN_VARIANT=host west build -p always -b native_sim $E -- \\
      -DDIAG_SKIP_CODEGEN=ON -DEXTRA_CONF_FILE=$E/boards/native_sim/native_sim.conf \\
      -DDTC_OVERLAY_FILE=$E/boards/native_sim/native_sim.overlay

Run (no sudo: a user + network namespace with its own vcan and TAP; needs the
can-isotp module loaded on the host):

    unshare -rn sh -c 'ip link add zcan0 type vcan && ip link set zcan0 up &&
                       ip tuntap add zeth mode tap && ip link set zeth up &&
                       ip addr add 192.0.2.2/24 dev zeth &&
                       python3 scripts/smoke_dual_transport_ecu.py build/zephyr/zephyr.exe'

CAN FD build (fd.conf in EXTRA_CONF_FILE): an MTU-72 vcan and --fd, which puts
the kernel's ISO-TP socket in FD mode (TX_DL 64, BRS): it then accepts only FD
frames, so every CAN answer also shows the ECU sent FD.

    unshare -rn sh -c 'ip link add zcan0 type vcan && ip link set zcan0 mtu 72 up &&
                       ip tuntap add zeth mode tap && ip link set zeth up &&
                       ip addr add 192.0.2.2/24 dev zeth &&
                       python3 scripts/smoke_dual_transport_ecu.py build/zephyr/zephyr.exe --fd'

Exit 0 when every check passes.
"""

from __future__ import annotations

import os
import random
import re
import socket
import struct
import subprocess
import sys
import threading
import time

CAN_IF, RX_ID, TX_ID = "zcan0", 0x7E0, 0x7E8
ECU_IP, DOIP_PORT = "192.0.2.1", 13400
ECU_ADDR, TESTER_ADDR = 0xE400, 0x0E00
ROUNDS = 200
MIX_ROUNDS = 1000
CHURN_CYCLES = 10
EXAMPLE_YAML = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                            "examples", "dual_transport_ecu", "diagnostics_config.yaml")
SOL_CAN_ISOTP, CAN_ISOTP_LL_OPTS, CANFD_BRS = 106, 5, 0x01
FD = "--fd" in sys.argv

FAILS: list[str] = []

# Every request a tester sent, by transport (each reaches the server: no job
# runs, one request is outstanding per transport), and the round-trip times
# collected while LATENCY_ON.
SENT = {"can": 0, "doip": 0}
LATENCY_MS: list[float] = []
LATENCY_ON = False
COUNT_LOCK = threading.Lock()


def note(name: str, t0: float) -> None:
    dt = (time.perf_counter() - t0) * 1000.0
    with COUNT_LOCK:
        SENT[name] += 1
        if LATENCY_ON:
            LATENCY_MS.append(dt)


def p2_server_max_ms() -> float:
    with open(EXAMPLE_YAML, encoding="utf-8") as f:
        m = re.search(r"^\s*p2_server_max_ms:\s*(\d+)", f.read(), re.M)
    if m is None:
        raise RuntimeError("p2_server_max_ms not found in " + EXAMPLE_YAML)
    return float(m.group(1))


def check(cond: bool, what: str) -> None:
    print(("PASS " if cond else "FAIL ") + what)
    if not cond:
        FAILS.append(what)


# ---- CAN: kernel ISO-TP ------------------------------------------------------

def can_socket() -> socket.socket:
    s = socket.socket(socket.AF_CAN, socket.SOCK_DGRAM, socket.CAN_ISOTP)
    if FD:   # struct can_isotp_ll_options: mtu, tx_dl, tx_flags
        s.setsockopt(SOL_CAN_ISOTP, CAN_ISOTP_LL_OPTS, struct.pack("BBB", 72, 64, CANFD_BRS))
    s.bind((CAN_IF, TX_ID, RX_ID))   # (interface, rx id, tx id)
    s.settimeout(2.0)
    return s


def can_req(s: socket.socket, req: bytes) -> bytes:
    t0 = time.perf_counter()
    s.send(req)
    r = s.recv(4096)
    note("can", t0)
    return r


# ---- DoIP: TCP ---------------------------------------------------------------

def doip_frame(ptype: int, payload: bytes) -> bytes:
    return bytes([0x02, 0xFD]) + ptype.to_bytes(2, "big") + len(payload).to_bytes(4, "big") + payload


def doip_read(s: socket.socket) -> tuple[int, bytes]:
    hdr = b""
    while len(hdr) < 8:
        hdr += s.recv(8 - len(hdr))
    n = int.from_bytes(hdr[4:8], "big")
    body = b""
    while len(body) < n:
        body += s.recv(n - len(body))
    return int.from_bytes(hdr[2:4], "big"), body


def doip_connect() -> socket.socket:
    s = socket.create_connection((ECU_IP, DOIP_PORT), timeout=5.0)
    s.sendall(doip_frame(0x0005, TESTER_ADDR.to_bytes(2, "big") + bytes(5)))
    ptype, body = doip_read(s)
    if ptype != 0x0006 or body[4] != 0x10:
        raise RuntimeError(f"routing activation refused: {ptype:#06x} {body.hex()}")
    return s


def doip_req(s: socket.socket, req: bytes) -> bytes:
    t0 = time.perf_counter()
    s.sendall(doip_frame(0x8001, TESTER_ADDR.to_bytes(2, "big") + ECU_ADDR.to_bytes(2, "big") + req))
    while True:
        ptype, body = doip_read(s)
        if ptype == 0x8001:          # the diagnostic message, after its 0x8002 ack
            note("doip", t0)
            return body[4:]


def doip_connect_retry(seconds: float = 5.0) -> socket.socket:
    """The server takes one connection at a time: a new one is accepted
    once it has handled the previous disconnect."""
    deadline = time.monotonic() + seconds
    while True:
        try:
            return doip_connect()
        except (OSError, RuntimeError):
            if time.monotonic() > deadline:
                raise
            time.sleep(0.05)


def busy(resp: bytes, sid: int) -> bool:
    return resp == bytes([0x7F, sid, 0x21])


# ---- the checks --------------------------------------------------------------

def ownership(cs: socket.socket) -> None:
    ds = doip_connect()
    check(can_req(cs, b"\x10\x03")[:2] == b"\x50\x03", "CAN 10 03 -> 50 03: CAN owns the session")
    check(busy(doip_req(ds, b"\x10\x03"), 0x10), "DoIP 10 03 -> 7F 10 21 while CAN owns it")
    check(busy(doip_req(ds, b"\x3E\x00"), 0x3E), "DoIP 3E 00 -> 7F 3E 21 while CAN owns it")
    check(can_req(cs, b"\x10\x01")[:2] == b"\x50\x01", "CAN 10 01 -> 50 01: back to default")
    check(doip_req(ds, b"\x10\x03")[:2] == b"\x50\x03", "DoIP 10 03 -> 50 03: DoIP owns it")
    check(busy(can_req(cs, b"\x3E\x00"), 0x3E), "CAN 3E 00 -> 7F 3E 21 while DoIP owns it")
    ds.close()
    time.sleep(0.5)                  # the server notices the close on its next read
    check(can_req(cs, b"\x10\x03")[:2] == b"\x50\x03",
          "after the DoIP disconnect, CAN 10 03 -> 50 03: DoIP's session ended")
    check(can_req(cs, b"\x10\x01")[:2] == b"\x50\x01", "CAN 10 01 -> 50 01")


def both_at_once(cs: socket.socket) -> None:
    global LATENCY_ON
    ds = doip_connect()
    LATENCY_ON = True
    tally = {"can": [0, 0, 0], "doip": [0, 0, 0]}   # positive, busy, other

    def run(name: str, fn) -> None:
        for _ in range(ROUNDS):
            r = fn(b"\x3E\x00")
            k = 0 if r == b"\x7E\x00" else (1 if busy(r, 0x3E) else 2)
            tally[name][k] += 1

    tc = threading.Thread(target=run, args=("can", lambda q: can_req(cs, q)))
    td = threading.Thread(target=run, args=("doip", lambda q: doip_req(ds, q)))
    tc.start(); td.start(); tc.join(); td.join()
    LATENCY_ON = False
    ds.close()
    for name in ("can", "doip"):
        pos, bsy, other = tally[name]
        check(pos + bsy == ROUNDS and other == 0,
              f"{name}: {ROUNDS} x 3E 00 at once with the other -> {pos} x 7E 00, {bsy} x 7F 3E 21")


# One entry per request of the mix: (request, kind). Weights favour reads.
MIX = ([(b"\x3E\x00", "tp")] * 3 + [(b"\x22\xF1\x90", "vin")] * 3 + [(b"\x22\xF1\x87", "ext")] * 2 +
       [(b"\x10\x03", "to_ext")] * 2 + [(b"\x10\x01", "to_def")])


def judge(owns: bool, kind: str, resp: bytes, req: bytes) -> tuple[bool, str | None]:
    """(still owns, None) or (owns, why the answer is wrong)."""
    sid = req[0]
    pos = resp[:1] == bytes([sid + 0x40])
    nrc = resp[:2] == bytes([0x7F, sid])
    b21 = busy(resp, sid)
    if owns:
        # The session is ours until our own 10 01: nothing may refuse us.
        ok = pos and (kind != "to_ext" or resp[:2] == b"\x50\x03") \
            and (kind != "to_def" or resp[:2] == b"\x50\x01")
        if ok and kind == "to_def":
            owns = False
        return owns, None if ok else "owner refused or answered wrongly"
    if kind == "to_ext":
        if resp[:2] == b"\x50\x03":
            return True, None
        return False, None if b21 else "10 03 answered neither 50 03 nor 7F 10 21"
    if kind == "ext":        # needs an extended session, which this side does not hold
        return False, None if (b21 or resp == b"\x7F\x22\x31") else "22 F187 outside its session"
    if kind == "to_def":
        return False, None if (b21 or resp[:2] == b"\x50\x01") else "10 01 answered neither 50 01 nor 7F 10 21"
    return False, None if (pos or b21) else "answered neither positively nor 7F sid 21"


def mix_worker(name: str, fn, seed: int, out: dict) -> None:
    rng = random.Random(seed)
    owns = False
    counts = {"pos": 0, "busy": 0}
    bad: list[str] = []
    try:
        for _ in range(MIX_ROUNDS):
            req, kind = rng.choice(MIX)
            resp = fn(req)
            owns, why = judge(owns, kind, resp, req)
            if why is not None and len(bad) < 5:
                bad.append(f"{req.hex()} -> {resp.hex()}: {why}")
            counts["busy" if busy(resp, req[0]) else "pos"] += 1
        if owns:
            fn(b"\x10\x01")      # leave the session for the other side
    except (OSError, RuntimeError) as e:
        bad.append(f"no answer: {e!r}")
    out[name] = (counts, bad)


def stress_mix(cs: socket.socket) -> None:
    global LATENCY_ON
    ds = doip_connect()
    out: dict = {}
    LATENCY_ON = True
    tc = threading.Thread(target=mix_worker, args=("can", lambda q: can_req(cs, q), 1, out))
    td = threading.Thread(target=mix_worker, args=("doip", lambda q: doip_req(ds, q), 2, out))
    tc.start(); td.start(); tc.join(); td.join()
    LATENCY_ON = False
    ds.close()
    for name in ("can", "doip"):
        counts, bad = out.get(name, ({"pos": 0, "busy": 0}, ["worker died"]))
        check(not bad and counts["pos"] + counts["busy"] == MIX_ROUNDS,
              f"{name}: {MIX_ROUNDS} requests competing for the session -> {counts['pos']} answered, "
              f"{counts['busy']} x 7F sid 21" + ("" if not bad else "; " + "; ".join(bad)))
    time.sleep(0.5)          # the server handles the DoIP close on its next read
    check(can_req(cs, b"\x10\x01")[:2] in (b"\x50\x01",), "CAN 10 01 -> 50 01: nobody holds the session after the mix")


def doip_churn(cs: socket.socket) -> None:
    check(can_req(cs, b"\x10\x03")[:2] == b"\x50\x03", "CAN 10 03 -> 50 03: CAN owns the session")
    kept = 0
    for i in range(CHURN_CYCLES):
        ds = doip_connect_retry()
        a = busy(doip_req(ds, b"\x22\xF1\x87"), 0x22)
        b = busy(doip_req(ds, b"\x10\x01"), 0x10)
        ds.close()
        ds = doip_connect_retry()    # accepted only after the server handled the close
        ds.close()
        if a and b and can_req(cs, b"\x22\xF1\x87")[:1] == b"\x62":
            kept += 1
    check(kept == CHURN_CYCLES,
          f"{CHURN_CYCLES} DoIP connect/refused/disconnect cycles while CAN owns an extended session -> "
          f"CAN's 22 F187 positive after each ({kept})")
    check(can_req(cs, b"\x10\x01")[:2] == b"\x50\x01", "CAN 10 01 -> 50 01")
    time.sleep(0.5)


def latency() -> None:
    limit = p2_server_max_ms()
    xs = sorted(LATENCY_MS)
    if not xs:
        check(False, "latency: no sample")
        return
    p50, p99, mx = xs[len(xs) // 2], xs[min(len(xs) - 1, int(len(xs) * 0.99))], xs[-1]
    print(f"latency over {len(xs)} requests, both transports loaded: p50 {p50:.2f} ms, p99 {p99:.2f} ms, max {mx:.2f} ms")
    check(p99 < limit, f"latency p99 {p99:.2f} ms < P2server_max {limit:.0f} ms")


def ecu_reset(cs: socket.socket, ecu: subprocess.Popen) -> None:
    check(can_req(cs, b"\x11\x01")[:2] == b"\x51\x01", "CAN 11 01 -> 51 01")
    try:
        out, _ = ecu.communicate(timeout=5.0)
    except subprocess.TimeoutExpired:
        ecu.kill()
        out, _ = ecu.communicate()
    check(b"sys_arch_reboot called" in out, "then the executor resets the ECU")
    m = re.search(rb"request_count=(\d+) negative_response_count=(\d+)", out)
    sent = SENT["can"] + SENT["doip"]
    if m is None:
        check(False, "the ECU's reset log carries its counters")
        return
    got = int(m.group(1))
    print(f"ECU: request_count {got}, negative_response_count {int(m.group(2))}; testers sent "
          f"{sent} (CAN {SENT['can']}, DoIP {SENT['doip']})")
    check(got == sent, f"ECU request_count {got} == requests sent {sent}")


def main() -> int:
    exe = sys.argv[1]
    ecu = subprocess.Popen([exe], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + 10.0
        while True:
            try:
                socket.create_connection((ECU_IP, DOIP_PORT), timeout=1.0).close()
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.2)
        time.sleep(0.3)
        cs = can_socket()
        ownership(cs)
        both_at_once(cs)
        stress_mix(cs)
        doip_churn(cs)
        latency()
        ecu_reset(cs, ecu)
    finally:
        if ecu.poll() is None:
            ecu.kill()
            ecu.wait()
    print(f"smoke_dual_transport_ecu{' (CAN FD)' if FD else ''}: {'FAILED: ' + '; '.join(FAILS) if FAILS else 'all checks passed'}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
