#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Bench smoke for EDS#580: a permanent DTC survives 14 FF FF FF and a reboot.

Runs examples/robot_joint_controller_ecu built with -DDIAG_BENCH_PERMANENT_DTC=ON
and native_sim_bench_nvs.conf (NVS on native_sim's flash simulator), over raw
CAN single frames (standard library only):

  1. wait for the simulated over-temperature DTC 0xA00101 to be confirmed
     (0x19 0x02 0x08 lists it, about 56 s after boot)
  2. 0x19 0x15 (permanent DTCs) lists it
  3. (extended session, 14 needs it) 14 FF FF FF answers 54, and the DTC is still confirmed and permanent
  4. ECUReset (11 01), then the ECU is started again in the same
     working directory, so ./flash.bin carries the DTC mirror across the
     "reboot"; the simulated motor is cold again (about 30 degC)
  5. 0x19 0x02 0x08 still lists 0xA00101 with its status byte, 0x19 0x15 lists
     it, and 14 FF FF FF still leaves it standing

The permanent flag is not persisted (by decision): the bench hook marks the
restored DTC permanent again at boot, which is what an application does.

The interface must exist and be up before this runs:
    ip link add dev zcan0 type vcan && ip link set zcan0 up
(`unshare -rn` gives a rootless network namespace for it.)

Control run: with --control the ECU gets EDS_BENCH_NO_PERMANENT=1, the DTC is not
permanent, and the script expects the opposite: 0x19 0x15 is empty and
14 FF FF FF clears 0xA00101 (also after the reboot, where it was restored).

Usage: smoke_permanent_dtc.py [--control] <path/to/zephyr.exe> [interface] [ramp_timeout_s]
"""

import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

REQ_ID = 0x7E0
RSP_ID = 0x7E8
CAN_FMT = "=IB3x8s"
DTC = 0xA00101
TEMP_FAULT_C = 85


def uds(sock, req, timeout=2.0):
    """Send one single-frame request, return the response payload (SF only)."""
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
        if (data[0] >> 4) != 0:
            raise RuntimeError("not a single frame: %s" % data.hex())
        return data[1:1 + (data[0] & 0x0F)]
    raise TimeoutError("no response to %s" % bytes(req).hex())


def dtcs(sock, sub):
    """0x19 <sub> [mask]: {dtc: status}."""
    r = uds(sock, [0x19, *sub])
    assert r[:2] == bytes([0x59, sub[0]]), r.hex()
    body = r[3:]
    assert len(body) % 4 == 0, r.hex()
    return {int.from_bytes(body[i:i + 3], "big"): body[i + 3]
            for i in range(0, len(body), 4)}


def confirmed(sock):
    return dtcs(sock, [0x02, 0x08])


def permanent(sock):
    return dtcs(sock, [0x15])


def temp_c(sock):
    r = uds(sock, [0x22, 0xA0, 0x03])
    return r[3] - 40


CONTROL = False
PROCS = []   # every ECU started, so none outlives the script


def start(exe, cwd):
    env = dict(os.environ, **({"EDS_BENCH_NO_PERMANENT": "1"} if CONTROL else {}))
    p = subprocess.Popen([os.path.abspath(exe)], cwd=cwd, env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    PROCS.append(p)
    return p


def wait_boot(sock):
    """Wait for the ECU, then enter the extended session (0x14 needs it)."""
    for _ in range(50):
        try:
            uds(sock, [0x3E, 0x00], timeout=0.2)
            assert uds(sock, [0x10, 0x03])[:2] == b"\x50\x03"
            return
        except TimeoutError:
            time.sleep(0.1)
    raise SystemExit("FAIL: ECU did not answer")


def main_control(sock, exe, work, ecu, ramp_timeout):
    """Same sequence, DTC not permanent: 14 FF FF FF must clear it."""
    wait_boot(sock)
    t0 = time.monotonic()
    while DTC not in confirmed(sock):
        if time.monotonic() - t0 > ramp_timeout:
            raise SystemExit("FAIL: 0x%06X not confirmed" % DTC)
        time.sleep(0.5)
    assert DTC not in permanent(sock), "permanent despite EDS_BENCH_NO_PERMANENT"
    print("CONTROL %.0f s: 0x%06X confirmed, 0x19 0x15 empty" % (time.monotonic() - t0, DTC))
    # The simulated motor is still hot and re-asserts the DTC every 100 ms, so
    # reboot first (cold again), then clear.
    assert uds(sock, [0x11, 0x01]) == b"\x51\x01"
    try:
        ecu.wait(timeout=5)
    except subprocess.TimeoutExpired:
        ecu.terminate()
        ecu.wait()
    time.sleep(0.5)
    ecu = start(exe, work)
    wait_boot(sock)
    assert DTC in confirmed(sock), "DTC not restored from NVM"
    assert uds(sock, [0x14, 0xFF, 0xFF, 0xFF]) == b"\x54"
    assert DTC not in confirmed(sock), "non-permanent DTC survived 14 FF FF FF"
    print("CONTROL after reboot: 14 FF FF FF -> 54; 0x%06X no longer confirmed" % DTC)
    return ecu


def main():
    global CONTROL
    if "--control" in sys.argv:
        CONTROL = True
        sys.argv.remove("--control")
    exe = sys.argv[1]
    iface = sys.argv[2] if len(sys.argv) > 2 else "zcan0"
    ramp_timeout = float(sys.argv[3]) if len(sys.argv) > 3 else 90.0

    sock = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    sock.bind((iface,))
    work = tempfile.mkdtemp(prefix="eds580-")   # ./flash.bin lives here
    ecu = start(exe, work)
    try:
        if CONTROL:
            ecu = main_control(sock, exe, work, ecu, ramp_timeout)
            return
        wait_boot(sock)
        assert DTC not in confirmed(sock), "DTC set at boot"
        print("PASS boot 1: %d degC, no confirmed DTC" % temp_c(sock))

        t0 = time.monotonic()
        while DTC not in confirmed(sock):
            if time.monotonic() - t0 > ramp_timeout:
                raise SystemExit("FAIL: 0x%06X not confirmed in %.0f s" % (DTC, ramp_timeout))
            time.sleep(0.5)
        time.sleep(0.3)
        st = confirmed(sock)[DTC]
        assert DTC in permanent(sock), "not in 0x19 0x15 once confirmed"
        print("PASS %.0f s: 0x%06X confirmed, status 0x%02X, listed by 0x19 0x15"
              % (time.monotonic() - t0, DTC, st))

        assert uds(sock, [0x14, 0xFF, 0xFF, 0xFF]) == b"\x54"
        assert DTC in confirmed(sock) and DTC in permanent(sock), \
            "14 FF FF FF cleared a permanent DTC"
        print("PASS 14 FF FF FF -> 54; 0x%06X still confirmed (0x%02X) and permanent"
              % (DTC, confirmed(sock)[DTC]))

        assert uds(sock, [0x11, 0x01]) == b"\x51\x01"
        try:
            ecu.wait(timeout=5)
        except subprocess.TimeoutExpired:
            ecu.terminate()
            ecu.wait()
        print("reboot: ECU exited (native_sim sys_reboot); flash.bin kept: %d bytes"
              % os.path.getsize(os.path.join(work, "flash.bin")))
        time.sleep(0.5)

        ecu = start(exe, work)
        wait_boot(sock)
        cold = temp_c(sock)
        assert cold <= TEMP_FAULT_C, "not cold after reboot: %d degC" % cold
        after = confirmed(sock)
        assert DTC in after, "0x%06X lost over the reboot: %r" % (DTC, after)
        time.sleep(0.3)
        assert DTC in permanent(sock), "not permanent again after boot"
        print("PASS boot 2: %d degC, 0x%06X restored from NVM, status 0x%02X, "
              "listed by 0x19 0x15" % (cold, DTC, after[DTC]))

        assert uds(sock, [0x14, 0xFF, 0xFF, 0xFF]) == b"\x54"
        assert DTC in confirmed(sock), "14 FF FF FF cleared the DTC after reboot"
        print("PASS 14 FF FF FF -> 54 after the reboot; 0x%06X still confirmed (0x%02X)"
              % (DTC, confirmed(sock)[DTC]))
    finally:
        for p in PROCS:
            p.terminate()
        subprocess.run(["rm", "-rf", work])


if __name__ == "__main__":
    main()
