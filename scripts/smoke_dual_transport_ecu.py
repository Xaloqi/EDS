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
  3. ECUReset over CAN: 11 01 is answered 51 01, then the ECU resets (on
     native_sim the process exits: "sys_arch_reboot called").

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
SOL_CAN_ISOTP, CAN_ISOTP_LL_OPTS, CANFD_BRS = 106, 5, 0x01
FD = "--fd" in sys.argv

FAILS: list[str] = []


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
    s.send(req)
    return s.recv(4096)


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
    s.sendall(doip_frame(0x8001, TESTER_ADDR.to_bytes(2, "big") + ECU_ADDR.to_bytes(2, "big") + req))
    while True:
        ptype, body = doip_read(s)
        if ptype == 0x8001:          # the diagnostic message, after its 0x8002 ack
            return body[4:]


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
    ds = doip_connect()
    tally = {"can": [0, 0, 0], "doip": [0, 0, 0]}   # positive, busy, other

    def run(name: str, fn) -> None:
        for _ in range(ROUNDS):
            r = fn(b"\x3E\x00")
            k = 0 if r == b"\x7E\x00" else (1 if busy(r, 0x3E) else 2)
            tally[name][k] += 1

    tc = threading.Thread(target=run, args=("can", lambda q: can_req(cs, q)))
    td = threading.Thread(target=run, args=("doip", lambda q: doip_req(ds, q)))
    tc.start(); td.start(); tc.join(); td.join()
    ds.close()
    for name in ("can", "doip"):
        pos, bsy, other = tally[name]
        check(pos + bsy == ROUNDS and other == 0,
              f"{name}: {ROUNDS} x 3E 00 at once with the other -> {pos} x 7E 00, {bsy} x 7F 3E 21")


def ecu_reset(cs: socket.socket, ecu: subprocess.Popen) -> None:
    check(can_req(cs, b"\x11\x01")[:2] == b"\x51\x01", "CAN 11 01 -> 51 01")
    try:
        out, _ = ecu.communicate(timeout=5.0)
    except subprocess.TimeoutExpired:
        ecu.kill()
        out, _ = ecu.communicate()
    check(b"sys_arch_reboot called" in out, "then the executor resets the ECU")


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
        ecu_reset(cs, ecu)
    finally:
        if ecu.poll() is None:
            ecu.kill()
            ecu.wait()
    print(f"smoke_dual_transport_ecu{' (CAN FD)' if FD else ''}: {'FAILED: ' + '; '.join(FAILS) if FAILS else 'all checks passed'}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
