#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Xaloqi
"""DoIP vehicle discovery probe (EDS#392), Python standard library only.

Starts a native_sim DoIP ECU built with native_sim_doip_realzeth.conf and
checks ISO 13400-2 discovery from the host side of its zeth TAP:

  1. 3 vehicle announcements to 255.255.255.255:13400 after boot, ~500 ms apart
  2. a broadcast vehicle identification request with version 0xFF from port
     13400 (what the Eclipse OpenSOVD CDA sends) is answered within 1 s
  3. by EID (match answered, foreign EID not), from an ephemeral port
  4. entity status and power mode
  5. generic header NACK 0x01 for an unknown type; a peer's 0x0004 ignored
  6. TCP still works: routing activation, then 22 F190 answered 62 F1 90

Network: the TAP "zeth" must exist and be up as 192.0.2.2/24 with a route
for 255.255.255.255 through it, before the ECU starts.

Usage: doip_discovery_probe.py <path/to/zephyr.exe> [--expect-off]
  --expect-off: the ECU was built with CONFIG_EDS_DOIP_DISCOVERY=n; pass
                only if nothing answers on UDP and TCP still works.
Exit status 0 if every check passed.
"""
import os
import socket
import struct
import subprocess
import sys
import time

ECU_IP = "192.0.2.1"
PORT = 13400
LA = 0xE400
TESTER = 0x0E00

results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}: {name} {detail}".rstrip(), flush=True)


def hdr(ver, ptype, plen):
    return struct.pack(">BBHI", ver, ver ^ 0xFF, ptype, plen)


def parse(data):
    ver, inv, ptype, plen = struct.unpack(">BBHI", data[:8])
    return ver, inv, ptype, plen, data[8:]


def recv_type(sock, ptype, timeout, src_ip=ECU_IP):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        sock.settimeout(max(0.01, end - time.monotonic()))
        try:
            data, src = sock.recvfrom(256)
        except socket.timeout:
            break
        if src[0] == src_ip and len(data) >= 8 and parse(data)[2] == ptype:
            return data
    return None


def tcp_read_frame(sock):
    head = b""
    while len(head) < 8:
        chunk = sock.recv(8 - len(head))
        if not chunk:
            return None
        head += chunk
    _, _, ptype, plen = struct.unpack(">BBHI", head)
    body = b""
    while len(body) < plen:
        chunk = sock.recv(plen - len(body))
        if not chunk:
            return None
        body += chunk
    return ptype, body


def tcp_check():
    s = socket.create_connection((ECU_IP, PORT), timeout=3)
    try:
        s.sendall(hdr(0x02, 0x0005, 7) + struct.pack(">HB4x", TESTER, 0x00))
        ptype, body = tcp_read_frame(s)
        ok = ptype == 0x0006 and body[4] == 0x10
        check("TCP routing activation", ok, f"type={ptype:#06x} code={body[4]:#04x}")
        uds = bytes([0x22, 0xF1, 0x90])
        s.sendall(hdr(0x02, 0x8001, 4 + len(uds)) + struct.pack(">HH", TESTER, LA) + uds)
        resp = None
        for _ in range(4):
            frame = tcp_read_frame(s)
            if frame is None:
                break
            if frame[0] == 0x8001:
                resp = frame[1][4:]
                break
        check("TCP 22 F190 answered", resp is not None and resp[:3] == b"\x62\xf1\x90",
              resp.hex() if resp else "None")
    finally:
        s.close()


def main():
    exe = sys.argv[1]
    expect_off = "--expect-off" in sys.argv[2:]

    listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    listener.bind(("0.0.0.0", PORT))

    log_path = os.path.abspath("doip_discovery_probe_ecu.log")
    log = open(log_path, "w")
    ecu = subprocess.Popen([os.path.abspath(exe)], stdout=log, stderr=subprocess.STDOUT)
    t0 = time.monotonic()
    try:
        vams = []
        listener.settimeout(0.2)
        while time.monotonic() - t0 < 4.0:
            try:
                data, src = listener.recvfrom(256)
            except socket.timeout:
                continue
            if src[0] == ECU_IP and len(data) >= 8 and parse(data)[2] == 0x0004:
                vams.append((time.monotonic() - t0, parse(data)))

        if expect_off:
            check("discovery off: no announcements", not vams, f"(got {len(vams)})")
            listener.sendto(hdr(0xFF, 0x0001, 0), ("255.255.255.255", PORT))
            check("discovery off: VIR not answered",
                  recv_type(listener, 0x0004, 1.0) is None)
            tcp_check()
            return

        check("3 announcements after boot", len(vams) == 3, f"(got {len(vams)})")
        gaps = [round(vams[i + 1][0] - vams[i][0], 3) for i in range(len(vams) - 1)]
        check("announcement interval ~500 ms", gaps and all(0.35 <= g <= 0.8 for g in gaps),
              str(gaps))
        eid = b"\x00" * 6
        if vams:
            plen, p = vams[0][1][3], vams[0][1][4]
            check("announcement logical address", struct.unpack(">H", p[17:19])[0] == LA)
            check("announcement payload 32 or 33 bytes", plen in (32, 33), str(plen))
            eid = p[19:25]
            print(f"      VIN={p[0:17].hex()} EID={eid.hex()} GID={p[25:31].hex()} "
                  f"FA={p[31]:#04x}" + (f" sync={p[32]:#04x}" if plen == 33 else ""))

        listener.sendto(hdr(0xFF, 0x0001, 0), ("255.255.255.255", PORT))
        got = recv_type(listener, 0x0004, 1.0)
        check("broadcast VIR v0xFF from :13400 answered within 1 s", got is not None)
        if got:
            check("reply carries version 0x02", got[0] == 0x02 and got[1] == 0xFD)

        eph = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        eph.bind(("192.0.2.2", 0))
        eph.sendto(hdr(0x02, 0x0002, 6) + eid, (ECU_IP, PORT))
        check("VIR by our EID answered to an ephemeral port",
              recv_type(eph, 0x0004, 1.0) is not None)
        eph.sendto(hdr(0x02, 0x0002, 6) + bytes(a ^ 0xFF for a in eid), (ECU_IP, PORT))
        check("VIR by a foreign EID not answered", recv_type(eph, 0x0004, 0.5) is None)

        eph.sendto(hdr(0x02, 0x4001, 0), (ECU_IP, PORT))
        st = recv_type(eph, 0x4002, 1.0)
        check("entity status", st is not None and parse(st)[4][:3] == b"\x01\x01\x00",
              parse(st)[4].hex() if st else "None")
        eph.sendto(hdr(0x02, 0x4003, 0), (ECU_IP, PORT))
        pm = recv_type(eph, 0x4004, 1.0)
        check("power mode ready", pm is not None and parse(pm)[4] == b"\x01")

        eph.sendto(hdr(0x02, 0x1234, 0), (ECU_IP, PORT))
        nack = recv_type(eph, 0x0000, 1.0)
        check("unknown type -> generic NACK 0x01", nack is not None and parse(nack)[4] == b"\x01")
        eph.sendto(hdr(0x02, 0x0004, 0), (ECU_IP, PORT))
        eph.settimeout(0.5)
        try:
            eph.recvfrom(256)
            check("a peer's 0x0004 is ignored", False)
        except socket.timeout:
            check("a peer's 0x0004 is ignored", True)

        tcp_check()
    finally:
        ecu.terminate()
        try:
            ecu.wait(timeout=5)
        except subprocess.TimeoutExpired:
            ecu.kill()
        log.close()
        if not all(results):
            print(f"--- ECU log ({log_path}) ---")
            print(open(log_path).read()[-4000:])


if __name__ == "__main__":
    main()
    sys.exit(0 if results and all(results) else 1)
