#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Runtime smoke for examples/bms_ecu on native_sim (EDS#577).

Starts the SocketCAN build of the example (host interface zcan0) and reads the
"source: app" DIDs and the identity DIDs over raw CAN frames (standard library
only). Each must answer the boot value src/bms_sim.c sets (identity DIDs: the
YAML default_value), not the zeros the generated stubs return: that is what
EDS#577 was (the handlers were never registered).

The extended-session DIDs (0xD901, 0xD910, 0xD911, 0xDE01, 0xDE02) are read
after `10 03`.

The interface must exist and be up before this runs:
    ip link add dev zcan0 type vcan && ip link set zcan0 up
(`unshare -rn` gives a rootless network namespace for it.)

Usage: smoke_bms_ecu.py <path/to/zephyr.exe> [interface]
"""

import socket
import struct
import subprocess
import sys
import time

REQ_ID = 0x7E0
RSP_ID = 0x7E8
CAN_FMT = "=IB3x8s"

# did: (name, expected payload as hex, needs extended session)
EXPECTED = {
    0xF190: ("VehicleIdentificationNumber", "BMSEXAMPLE0000001".encode().hex(), False),
    0xF18C: ("ECUSerialNumber",             "BMS00001".encode().hex(), False),
    0xF189: ("ECUSoftwareVersionNumber",    "01000000", False),
    0xDB00: ("PackVoltage_mV",              "00057300", False),   # 357120
    0xDB01: ("PackCurrent_100mA",           "0000",     False),
    0xDB02: ("PackPower_10W",               "0000",     False),
    0xDB03: ("ContactorState",              "00",       False),
    0xDB04: ("InsulationResistance_kOhm",   "ffff",     False),
    0xDC01: ("CellGroup1_Voltage_mV",       "ae60",     False),   # 44640
    0xDC02: ("CellGroup2_Voltage_mV",       "ae60",     False),
    0xDC03: ("CellGroup3_Voltage_mV",       "ae60",     False),
    0xDC04: ("CellGroup4_Voltage_mV",       "ae60",     False),
    0xDD00: ("MaxCellTemperature_degC",     "41",       False),   # 25 degC + 40
    0xDD01: ("MinCellTemperature_degC",     "41",       False),
    0xDD02: ("CoolantInletTemperature",     "41",       False),
    0xDD03: ("BMSBoardTemperature",         "41",       False),
    0xDE00: ("StateOfCharge_04pct",         "7d",       False),   # 125 -> 50 %
    0xDE01: ("StateOfHealth_04pct",         "fa",       True),
    0xDE02: ("BalancingStateBitmask",       "00",       True),
    0xD900: ("FaultFlagsBitmask",           "0000",     False),
    0xD901: ("FullChargeCapacity_Wh",       "000124f8", True),    # 75000
    0xD910: ("OVThreshold_mV",              "1068",     True),    # 4200
    0xD911: ("UVThreshold_mV",              "0af0",     True),    # 2800
}


def _send(sock, payload):
    sock.send(struct.pack(CAN_FMT, REQ_ID, len(payload),
                          bytes(payload).ljust(8, b"\0")))


def uds(sock, req, timeout=2.0):
    """Send one single-frame request, return the response payload.

    Answers a First Frame with Flow Control (BS 0, STmin 0) and joins the
    Consecutive Frames, so the 17-byte VIN reads too."""
    _send(sock, bytes([len(req)]) + bytes(req))
    deadline = time.monotonic() + timeout
    total, buf = 0, b""
    while time.monotonic() < deadline:
        sock.settimeout(max(deadline - time.monotonic(), 0.01))
        try:
            raw = sock.recv(16)
        except socket.timeout:
            break
        can_id, dlc, data = struct.unpack(CAN_FMT, raw)
        if (can_id & 0x1FFFFFFF) != RSP_ID:
            continue
        pci = data[0] >> 4
        if pci == 0 and not total:
            return data[1:1 + (data[0] & 0x0F)]
        if pci == 1:                                  # First Frame
            total = ((data[0] & 0x0F) << 8) | data[1]
            buf = data[2:8]
            _send(sock, [0x30, 0x00, 0x00])
        elif pci == 2 and total:                      # Consecutive Frame
            buf += data[1:8]
            if len(buf) >= total:
                return buf[:total]
    raise TimeoutError("no response to %s" % bytes(req).hex())


def read_did(sock, did):
    r = uds(sock, [0x22, did >> 8, did & 0xFF])
    assert r[:3] == bytes([0x62, did >> 8, did & 0xFF]), \
        "0x%04X: %s" % (did, r.hex())
    return r[3:]


def main():
    exe = sys.argv[1]
    iface = sys.argv[2] if len(sys.argv) > 2 else "zcan0"

    sock = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    sock.bind((iface,))
    ecu = subprocess.Popen([exe], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    failed = 0
    try:
        for _ in range(50):  # ECU boot
            try:
                uds(sock, [0x3E, 0x00], timeout=0.2)
                break
            except TimeoutError:
                time.sleep(0.1)
        else:
            raise SystemExit("FAIL: ECU did not answer on %s" % iface)

        extended = False
        for did, (name, want, needs_ext) in sorted(EXPECTED.items(),
                                                   key=lambda kv: kv[1][2]):
            if needs_ext and not extended:
                r = uds(sock, [0x10, 0x03])
                assert r[:2] == b"\x50\x03", r.hex()
                extended = True
            got = read_did(sock, did).hex()
            ok = got == want
            failed += not ok
            print("%s 0x%04X %-26s %s%s" % ("PASS" if ok else "FAIL", did, name,
                                            got, "" if ok else "  (want %s)" % want))
    finally:
        ecu.terminate()
    if failed:
        raise SystemExit("FAIL: %d DID(s) do not answer their configured value" % failed)
    print("PASS %d DIDs answer their configured values" % len(EXPECTED))


if __name__ == "__main__":
    main()
