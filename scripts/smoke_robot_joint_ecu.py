#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Runtime smoke for examples/robot_joint_controller_ecu on native_sim.

Starts the SocketCAN build of the example (native_sim_socketcan.conf/.overlay,
host interface zcan0), then over raw CAN single frames (standard library only,
no ISO-TP kernel module):

  1. reads DID 0xA000 (position): must be non-zero
  2. reads DID 0xA003 (motor temperature) at boot: must be below the 85 degC
     threshold, and 0x19 0x02 0x01 must not list DTC 0xA00101
  3. waits for the temperature ramp (DID 0xA003 above 85 degC)
  4. 0x19 0x02 0x01 must now list DTC 0xA00101 with testFailed set, and
     DID 0xA004 must have the OVERTEMP bit (bit 4) set

The interface must exist and be up before this runs:
    ip link add dev zcan0 type vcan && ip link set zcan0 up
(`unshare -rn` gives a rootless network namespace for it, see the example's
README.) The ECU is started and stopped by this script.

Usage: smoke_robot_joint_ecu.py <path/to/zephyr.exe> [interface] [ramp_timeout_s]
"""

import socket
import struct
import subprocess
import sys
import time

REQ_ID = 0x7E0   # physical request (0x7DF is the functional ID)
RSP_ID = 0x7E8
TEMP_FAULT_C = 85
CAN_FMT = "=IB3x8s"


def uds(sock, req, timeout=2.0):
    """Send one single-frame request, return the response payload (SF only)."""
    assert len(req) <= 7
    frame = bytes([len(req)]) + bytes(req)
    sock.send(struct.pack(CAN_FMT, REQ_ID, len(frame), frame.ljust(8, b"\0")))
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        sock.settimeout(max(deadline - time.monotonic(), 0.01))
        try:
            raw = sock.recv(16)
        except socket.timeout:
            break
        can_id, dlc, data = struct.unpack(CAN_FMT, raw)
        if (can_id & 0x1FFFFFFF) != RSP_ID:
            continue
        pci = data[0]
        if (pci >> 4) != 0:
            raise RuntimeError("response is not a single frame: %s" % data.hex())
        return data[1:1 + (pci & 0x0F)]
    raise TimeoutError("no response to %s" % bytes(req).hex())


def temp_c(sock):
    r = uds(sock, [0x22, 0xA0, 0x03])
    assert r[:3] == b"\x62\xA0\x03" and len(r) == 4, r.hex()
    return r[3] - 40


def dtc_list(sock):
    """0x19 0x02 with status mask 0x01 (testFailed): {dtc: status}."""
    r = uds(sock, [0x19, 0x02, 0x01])
    assert r[:2] == b"\x59\x02", r.hex()
    body = r[3:]
    assert len(body) % 4 == 0, r.hex()
    return {int.from_bytes(body[i:i + 3], "big"): body[i + 3]
            for i in range(0, len(body), 4)}


def main():
    exe = sys.argv[1]
    iface = sys.argv[2] if len(sys.argv) > 2 else "zcan0"
    ramp_timeout = float(sys.argv[3]) if len(sys.argv) > 3 else 90.0

    sock = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    sock.bind((iface,))
    ecu = subprocess.Popen([exe], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    try:
        for _ in range(50):  # ECU boot
            try:
                uds(sock, [0x22, 0xA0, 0x03], timeout=0.2)
                break
            except TimeoutError:
                time.sleep(0.1)
        else:
            raise SystemExit("FAIL: ECU did not answer on %s" % iface)

        pos = uds(sock, [0x22, 0xA0, 0x00])
        assert pos[:3] == b"\x62\xA0\x00" and len(pos) == 7, pos.hex()
        assert pos[3:] != b"\0\0\0\0", "0xA000 is zero (stub?): %s" % pos.hex()
        print("PASS 0xA000 = %s (%d mdeg)"
              % (pos[3:].hex(), int.from_bytes(pos[3:], "big", signed=True)))

        t0 = temp_c(sock)
        assert t0 <= TEMP_FAULT_C, "already hot at boot: %d degC" % t0
        assert 0xA00101 not in dtc_list(sock), "DTC set before the ramp"
        print("PASS boot: %d degC, no 0xA00101" % t0)

        start = time.monotonic()
        while temp_c(sock) <= TEMP_FAULT_C:
            if time.monotonic() - start > ramp_timeout:
                raise SystemExit("FAIL: 85 degC not exceeded in %.0f s"
                                 % ramp_timeout)
            time.sleep(0.5)
        time.sleep(0.5)  # DTC is set by the next 100 ms re-assert at the latest
        dtcs = dtc_list(sock)
        assert 0xA00101 in dtcs, "0xA00101 missing: %r" % dtcs
        assert dtcs[0xA00101] & 0x01, "testFailed not set: 0x%02X" % dtcs[0xA00101]
        st = uds(sock, [0x22, 0xA0, 0x04])
        assert st[3] & 0x10, "OVERTEMP bit not set in 0xA004: %s" % st.hex()
        print("PASS %.0f s after first read: 0x19 lists 0xA00101 status 0x%02X, "
              "0xA004 = 0x%02X" % (time.monotonic() - start, dtcs[0xA00101], st[3]))
    finally:
        ecu.terminate()
        ecu.wait(timeout=5)
    print("SMOKE OK")


if __name__ == "__main__":
    main()
