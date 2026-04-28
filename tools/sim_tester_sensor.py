#!/usr/bin/env python3
# =============================================================================
# Xaloqi EDS
# FILE: tools/sim_tester_sensor.py
#
# PURPOSE: Live demo script for the SensorECU example.
#
#          Runs a scripted 6-phase sequence against the built-in ECU simulator
#          (zero dependencies — no CAN hardware, no vcan0, no west build):
#
#            Phase 1 — Normal operation
#                      Read AmbientTemperature (DID 0xD001): 25 °C
#                      Read SupplyVoltage (DID 0xD002): 12,340 mV
#                      Read SensorStatusBitmask (DID 0xD003): 0x03 (all OK)
#                      ReadDTCInformation (0x19 02 FF): no active DTCs
#
#            Phase 2 — Over-temperature fault injection
#                      Inject temp = 95 °C (above 85 °C threshold)
#                      DTC 0xD00101 becomes TEST_FAILED
#                      SensorStatusBitmask bit 2 set (TEMP_FAULT_HIGH)
#
#            Phase 3 — Under-voltage fault injection
#                      Inject voltage = 7,500 mV (below 9,000 mV threshold)
#                      DTC 0xD00202 becomes TEST_FAILED
#
#            Phase 4 — Read both active DTCs
#                      0x19 02 FF → two DTCs returned with status bytes
#
#            Phase 5 — Threshold adjustment (write DID 0xD010)
#                      Enter extended session, unlock security level 1
#                      Write TemperatureThresholdHigh = 100 °C
#                      Temp fault clears (95 °C now within new threshold)
#
#            Phase 6 — Recovery
#                      Inject voltage back to 12,500 mV
#                      DTC 0xD00202 clears
#                      ReadDTCInformation: no active DTCs
#
# USAGE:
#     # No hardware, no build required — runs in under 5 seconds:
#     python3 tools/sim_tester_sensor.py
#
#     # Verbose ISO-TP frame trace:
#     python3 tools/sim_tester_sensor.py --verbose
#
#     # Against a real native_sim build on vcan0:
#     python3 tools/sim_tester_sensor.py --interface socketcan --channel vcan0
#
# DEPENDENCIES:
#     None for simulator mode (default).
#     python-can >= 4.0.0 for socketcan / virtual modes:
#         pip install python-can
#
# SPDX-License-Identifier: Apache-2.0
# =============================================================================

from __future__ import annotations

import argparse
import os
import queue
import struct
import sys
import time
import threading
import logging
from typing import List, Optional

log = logging.getLogger("sim_tester_sensor")

# ---------------------------------------------------------------------------
# Protocol constants — must match examples/sensor_ecu/diagnostics_config.yaml
# ---------------------------------------------------------------------------

TESTER_TX_ID = 0x7DF
ECU_TX_ID    = 0x7E8
TIMEOUT_S    = 0.5

# SID bytes
SID_DSC   = 0x10   # DiagnosticSessionControl
SID_SA    = 0x27   # SecurityAccess
SID_RDBI  = 0x22   # ReadDataByIdentifier
SID_WDBI  = 0x2E   # WriteDataByIdentifier
SID_RDTC  = 0x19   # ReadDTCInformation
SID_NR    = 0x7F   # Negative Response

SESSION_DEFAULT     = 0x01
SESSION_EXTENDED    = 0x03

# DID IDs (matches diagnostics_config.yaml)
DID_AMBIENT_TEMP        = 0xD001
DID_SUPPLY_VOLTAGE      = 0xD002
DID_SENSOR_STATUS       = 0xD003
DID_TEMP_THRESHOLD_HIGH = 0xD010
DID_TEMP_THRESHOLD_LOW  = 0xD011

# DTC codes (3-byte)
DTC_TEMP_HIGH    = 0xD00101
DTC_TEMP_LOW     = 0xD00102
DTC_VOLTAGE_HIGH = 0xD00201
DTC_VOLTAGE_LOW  = 0xD00202

# Status bitmask bits (sensor_ecu.h)
STATUS_TEMP_OK           = 0x01
STATUS_VOLTAGE_OK        = 0x02
STATUS_TEMP_FAULT_HIGH   = 0x04
STATUS_TEMP_FAULT_LOW    = 0x08
STATUS_VOLTAGE_FAULT_HIGH = 0x10
STATUS_VOLTAGE_FAULT_LOW  = 0x20

# Temperature encoding: raw = T_°C + 40
TEMP_OFFSET = 40

# AES-128 placeholder key (matches uds_security_algo.c dev keys)
_LEVEL1_KEY = bytes([
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
])


# ---------------------------------------------------------------------------
# Minimal AES-128-CMAC (identical to the implementation in conftest.py)
# No external dependencies.
# ---------------------------------------------------------------------------

def _aes_enc(key: bytes, block: bytes) -> bytes:
    """Pure-Python AES-128 single block encryption (ECB)."""
    assert len(key) == 16 and len(block) == 16

    def xtime(b: int) -> int:
        return ((b << 1) ^ 0x1B) & 0xFF if b & 0x80 else (b << 1) & 0xFF

    def mul(a: int, b: int) -> int:
        r = 0
        for _ in range(8):
            if b & 1:
                r ^= a
            a = xtime(a)
            b >>= 1
        return r

    SBOX = [
        0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
        0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
        0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
        0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
        0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
        0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
        0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
        0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
        0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
        0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
        0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
        0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
        0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
        0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
        0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
        0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
    ]
    RCON = [0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36]

    def key_expand(k: bytes):
        w = [k[i*4:(i+1)*4] for i in range(4)]
        for i in range(4, 44):
            t = list(w[i-1])
            if i % 4 == 0:
                t = [SBOX[t[1]]^RCON[i//4-1], SBOX[t[2]], SBOX[t[3]], SBOX[t[0]]]
            w.append(bytes(a^b for a,b in zip(w[i-4], t)))
        return [bytes(w[i])+bytes(w[i+1])+bytes(w[i+2])+bytes(w[i+3]) for i in range(0,44,4)]

    def add_round_key(s, rk):
        return bytes(a^b for a,b in zip(s, rk))

    def sub_bytes(s):
        return bytes(SBOX[b] for b in s)

    def shift_rows(s):
        return bytes([s[0],s[5],s[10],s[15],s[4],s[9],s[14],s[3],
                      s[8],s[13],s[2],s[7],s[12],s[1],s[6],s[11]])

    def mix_col(col):
        return bytes([mul(2,col[0])^mul(3,col[1])^col[2]^col[3],
                      col[0]^mul(2,col[1])^mul(3,col[2])^col[3],
                      col[0]^col[1]^mul(2,col[2])^mul(3,col[3]),
                      mul(3,col[0])^col[1]^col[2]^mul(2,col[3])])

    def mix_columns(s):
        r = b""
        for i in range(4):
            r += mix_col(s[i::4])
        return bytes([r[j*4+i] for j in range(4) for i in range(4)])

    rks = key_expand(key)
    state = add_round_key(block, rks[0])
    for rd in range(1, 10):
        state = add_round_key(mix_columns(shift_rows(sub_bytes(state))), rks[rd])
    return add_round_key(shift_rows(sub_bytes(state)), rks[10])


def _cmac(key: bytes, msg: bytes) -> bytes:
    """AES-128-CMAC, returns 16-byte tag."""
    def _pad16(b):
        r = len(b) % 16
        return b + b'\x80' + b'\x00' * (15 - r) if r else b

    def _xor16(a, b):
        return bytes(x ^ y for x, y in zip(a, b))

    def _gen_subkeys(k):
        L = _aes_enc(k, b'\x00' * 16)
        K1 = (int.from_bytes(L, 'big') << 1) & 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF
        if L[0] & 0x80:
            K1 ^= 0x87
        K1 = K1.to_bytes(16, 'big')
        K2 = (int.from_bytes(K1, 'big') << 1) & 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF
        if K1[0] & 0x80:
            K2 ^= 0x87
        K2 = K2.to_bytes(16, 'big')
        return K1, K2

    K1, K2 = _gen_subkeys(key)
    blocks = [msg[i:i+16] for i in range(0, max(len(msg), 1), 16)]
    if not blocks:
        blocks = [b'']
    last = blocks[-1]
    if len(last) == 16:
        last = _xor16(last, K1)
    else:
        last = _xor16(_pad16(last), K2)
    blocks[-1] = last
    X = b'\x00' * 16
    for blk in blocks:
        X = _aes_enc(key, _xor16(X, blk))
    return X


def _derive_key(seed: bytes, level: int) -> bytes:
    """Derive UDS SecurityAccess key from seed using AES-128-CMAC."""
    key = _LEVEL1_KEY if level == 1 else bytes([
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    ])
    mac = _cmac(key, seed)
    return mac[:4]   # ECU expects 4-byte key (UDS_SECURITY_KEY_LEN)


# ---------------------------------------------------------------------------
# ISO-TP framing helpers (single-frame only for requests ≤7 bytes)
# ---------------------------------------------------------------------------

def _isotp_sf(payload: bytes) -> bytes:
    n = len(payload)
    assert 1 <= n <= 7
    return bytes([n]) + payload + b'\xCC' * (7 - n)


def _isotp_decode_sf(data: bytes) -> Optional[bytes]:
    if len(data) < 1:
        return None
    pci = data[0]
    if (pci >> 4) == 0:          # Single Frame
        length = pci & 0x0F
        if length == 0 or length > len(data) - 1:
            return None
        return bytes(data[1:1 + length])
    return None


def _isotp_decode_mf(frames: List[bytes]) -> Optional[bytes]:
    """Decode a multi-frame ISO-TP sequence (FF + one or more CF)."""
    if not frames:
        return None
    ff = frames[0]
    if (ff[0] >> 4) != 1:
        return None
    total = ((ff[0] & 0x0F) << 8) | ff[1]
    payload = bytearray(ff[2:8])
    for cf in frames[1:]:
        if (cf[0] >> 4) != 2:
            return None
        payload.extend(cf[1:8])
    return bytes(payload[:total])


# ---------------------------------------------------------------------------
# Built-in ECU simulator
#
# Mirrors _EcuSimulator in examples/sensor_ecu/generated/tests/conftest.py
# but adds mutable sensor state so the demo can inject faults.
# ---------------------------------------------------------------------------

class SensorEcuSimulator:
    """
    Stateful UDS simulator for the SensorECU.

    Sensor state is mutable via inject_temp() and inject_voltage() so the
    demo script can drive faults and recovery without real hardware.
    Threshold DIDs (0xD010 / 0xD011) are writable and affect fault detection.
    DTC state updates automatically when inject_*() is called.
    """

    _LEVEL_KEYS = {
        1: _LEVEL1_KEY,
        2: bytes([0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
                  0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F]),
    }

    def __init__(self) -> None:
        self._session      = SESSION_DEFAULT
        self._sec_unlocked: dict = {}
        self._pending_seed: Optional[bytes] = None
        self._pending_level: Optional[int] = None
        self._seq = 1

        # Sensor state (mutable via inject_* methods)
        self._temp_deg_c   = 25        # °C
        self._voltage_mv   = 12000     # mV

        # NVM-backed thresholds (writable via DID 0xD010 / 0xD011)
        self._temp_high_threshold = 85   # °C
        self._temp_low_threshold  = -40  # °C

        # DTC status bytes: 0x00 = clear, 0x08 = testFailed
        self._dtcs = {
            DTC_TEMP_HIGH:    0x00,
            DTC_TEMP_LOW:     0x00,
            DTC_VOLTAGE_HIGH: 0x00,
            DTC_VOLTAGE_LOW:  0x00,
        }

    # ── Sensor injection ────────────────────────────────────────────────────

    def inject_temp(self, deg_c: int) -> None:
        """Set simulated temperature and update DTC state immediately."""
        self._temp_deg_c = deg_c
        self._update_dtcs()

    def inject_voltage(self, mv: int) -> None:
        """Set simulated supply voltage and update DTC state immediately."""
        self._voltage_mv = mv
        self._update_dtcs()

    def _update_dtcs(self) -> None:
        """Recompute DTC fault status from current sensor values and thresholds."""
        if self._temp_deg_c > self._temp_high_threshold:
            self._dtcs[DTC_TEMP_HIGH] = 0x08
            self._dtcs[DTC_TEMP_LOW]  = 0x00
        elif self._temp_deg_c < self._temp_low_threshold:
            self._dtcs[DTC_TEMP_LOW]  = 0x08
            self._dtcs[DTC_TEMP_HIGH] = 0x00
        else:
            self._dtcs[DTC_TEMP_HIGH] = 0x00
            self._dtcs[DTC_TEMP_LOW]  = 0x00

        if self._voltage_mv > 16000:
            self._dtcs[DTC_VOLTAGE_HIGH] = 0x08
            self._dtcs[DTC_VOLTAGE_LOW]  = 0x00
        elif self._voltage_mv < 9000:
            self._dtcs[DTC_VOLTAGE_LOW]  = 0x08
            self._dtcs[DTC_VOLTAGE_HIGH] = 0x00
        else:
            self._dtcs[DTC_VOLTAGE_HIGH] = 0x00
            self._dtcs[DTC_VOLTAGE_LOW]  = 0x00

    def _sensor_status_bitmask(self) -> int:
        status = STATUS_TEMP_OK | STATUS_VOLTAGE_OK
        if self._dtcs[DTC_TEMP_HIGH]:
            status |= STATUS_TEMP_FAULT_HIGH
        if self._dtcs[DTC_TEMP_LOW]:
            status |= STATUS_TEMP_FAULT_LOW
        if self._dtcs[DTC_VOLTAGE_HIGH]:
            status |= STATUS_VOLTAGE_FAULT_HIGH
        if self._dtcs[DTC_VOLTAGE_LOW]:
            status |= STATUS_VOLTAGE_FAULT_LOW
        return status

    # ── UDS PDU handler ─────────────────────────────────────────────────────

    def handle(self, pdu: bytes) -> Optional[bytes]:
        if not pdu:
            return None
        sid = pdu[0]
        dispatch = {
            SID_DSC:  self._dsc,
            SID_SA:   self._sa,
            SID_RDBI: self._rdbi,
            SID_WDBI: self._wdbi,
            SID_RDTC: self._rdtc,
            0x3E:     self._tp,
            0x11:     self._reset,
        }
        fn = dispatch.get(sid)
        if fn is None:
            return self._nrc(sid, 0x11)   # serviceNotSupported
        return fn(pdu)

    def _nrc(self, sid: int, nrc: int) -> bytes:
        return bytes([SID_NR, sid, nrc])

    def _dsc(self, pdu: bytes) -> Optional[bytes]:
        if len(pdu) < 2:
            return self._nrc(SID_DSC, 0x13)
        sub = pdu[1]
        if sub not in (0x01, 0x02, 0x03):
            return self._nrc(SID_DSC, 0x22)
        self._session = sub
        if sub != SESSION_EXTENDED:
            self._sec_unlocked = {}
        return bytes([0x50, sub, 0x00, 0x19, 0x01, 0xF4])

    def _tp(self, pdu: bytes) -> Optional[bytes]:
        if len(pdu) >= 2 and pdu[1] & 0x80:
            return None   # suppress positive response
        return bytes([0x7E, 0x00])

    def _sa(self, pdu: bytes) -> bytes:
        if len(pdu) < 2:
            return self._nrc(SID_SA, 0x13)
        sub = pdu[1]
        level = (sub + 1) // 2
        if level not in self._LEVEL_KEYS:
            return self._nrc(SID_SA, 0x12)

        if sub % 2 == 1:   # Request seed (odd sub-function)
            if self._sec_unlocked.get(level):
                return bytes([0x67, sub]) + b'\x00' * 8
            seed = os.urandom(8)
            self._pending_seed  = seed
            self._pending_level = level
            return bytes([0x67, sub]) + seed

        else:              # Send key (even sub-function)
            if self._pending_seed is None or self._pending_level != level:
                return self._nrc(SID_SA, 0x24)   # requestSequenceError
            if len(pdu) < 6:
                return self._nrc(SID_SA, 0x13)
            expected = _derive_key(self._pending_seed, level)
            received = bytes(pdu[2:6])
            self._pending_seed  = None
            self._pending_level = None
            if received != expected:
                return self._nrc(SID_SA, 0x35)   # invalidKey
            self._sec_unlocked[level] = True
            return bytes([0x67, sub])

    def _rdbi(self, pdu: bytes) -> bytes:
        if len(pdu) < 3:
            return self._nrc(SID_RDBI, 0x13)
        did = (pdu[1] << 8) | pdu[2]
        session_ordinal = {0x01: 1, 0x03: 3, 0x02: 2}.get(self._session, 1)

        handlers = {
            0xF190: (1, 0, bytes(b'1HGBH41JXMN109186')),
            0xF18C: (1, 0, bytes(range(0x58, 0x60))),
            DID_AMBIENT_TEMP:        (1, 0, None),   # dynamic
            DID_SUPPLY_VOLTAGE:      (1, 0, None),   # dynamic
            DID_SENSOR_STATUS:       (1, 0, None),   # dynamic
            DID_TEMP_THRESHOLD_HIGH: (3, 0, None),   # dynamic writable
            DID_TEMP_THRESHOLD_LOW:  (3, 0, None),   # dynamic writable
        }

        if did not in handlers:
            return self._nrc(SID_RDBI, 0x31)   # requestOutOfRange

        min_sess, sec_needed, static_data = handlers[did]
        if session_ordinal < min_sess:
            return self._nrc(SID_RDBI, 0x7F)   # not in session
        if sec_needed and not self._sec_unlocked.get(sec_needed):
            return self._nrc(SID_RDBI, 0x33)   # securityAccessDenied

        if did == DID_AMBIENT_TEMP:
            raw = self._temp_deg_c + TEMP_OFFSET
            data = bytes([raw & 0xFF])
        elif did == DID_SUPPLY_VOLTAGE:
            data = struct.pack('>H', self._voltage_mv)
        elif did == DID_SENSOR_STATUS:
            data = bytes([self._sensor_status_bitmask()])
        elif did == DID_TEMP_THRESHOLD_HIGH:
            data = bytes([(self._temp_high_threshold + TEMP_OFFSET) & 0xFF])
        elif did == DID_TEMP_THRESHOLD_LOW:
            data = bytes([(self._temp_low_threshold + TEMP_OFFSET) & 0xFF])
        else:
            data = static_data

        return bytes([0x62, pdu[1], pdu[2]]) + data

    def _wdbi(self, pdu: bytes) -> bytes:
        if len(pdu) < 3:
            return self._nrc(0x2E, 0x13)
        did = (pdu[1] << 8) | pdu[2]
        data = pdu[3:]
        session_ordinal = {0x01: 1, 0x03: 3, 0x02: 2}.get(self._session, 1)

        writable = {
            DID_TEMP_THRESHOLD_HIGH: (3, 1, 1),   # min_session=extended, write_sec=1, length=1
            DID_TEMP_THRESHOLD_LOW:  (3, 1, 1),
        }
        if did not in writable:
            return self._nrc(0x2E, 0x31)
        min_sess, write_sec, expected_len = writable[did]
        if session_ordinal < min_sess:
            return self._nrc(0x2E, 0x7F)
        if not self._sec_unlocked.get(write_sec):
            return self._nrc(0x2E, 0x33)
        if len(data) != expected_len:
            return self._nrc(0x2E, 0x13)

        raw_val = data[0]
        deg_c = raw_val - TEMP_OFFSET
        if did == DID_TEMP_THRESHOLD_HIGH:
            self._temp_high_threshold = deg_c
        else:
            self._temp_low_threshold = deg_c
        self._update_dtcs()
        return bytes([0x6E, pdu[1], pdu[2]])

    def _rdtc(self, pdu: bytes) -> bytes:
        if len(pdu) < 2:
            return self._nrc(SID_RDTC, 0x13)
        sub = pdu[1]

        if sub == 0x01:   # reportNumberOfDTCByStatusMask
            mask = pdu[2] if len(pdu) >= 3 else 0xFF
            count = sum(1 for s in self._dtcs.values() if s & mask)
            return bytes([0x59, 0x01, 0xFF, 0x09, 0x00, count])

        if sub == 0x02:   # reportDTCByStatusMask
            mask = pdu[2] if len(pdu) >= 3 else 0xFF
            resp = bytearray([0x59, 0x02, 0xFF])
            for code, status in self._dtcs.items():
                if status & mask:
                    resp += bytes([(code >> 16) & 0xFF,
                                   (code >>  8) & 0xFF,
                                    code        & 0xFF,
                                    status])
            return bytes(resp)

        return self._nrc(SID_RDTC, 0x12)   # subFunctionNotSupported

    def _reset(self, pdu: bytes) -> Optional[bytes]:
        sub = pdu[1] if len(pdu) >= 2 else 0x01
        self._session = SESSION_DEFAULT
        self._sec_unlocked = {}
        return bytes([0x51, sub])


# ---------------------------------------------------------------------------
# Minimal ISO-TP transport over the simulator (no CAN bus needed)
# ---------------------------------------------------------------------------

class SimTransport:
    """Wraps SensorEcuSimulator with an ISO-TP framing layer."""

    FC_BLOCK_SIZE = 0
    FC_STMIN      = 0

    def __init__(self, sim: SensorEcuSimulator, verbose: bool = False) -> None:
        self._sim     = sim
        self._verbose = verbose
        self.pass_count = 0
        self.fail_count = 0

    def request(self, payload: List[int]) -> Optional[List[int]]:
        data = bytes(payload)
        if self._verbose:
            log.debug(f"  PDU TX: {data.hex(' ').upper()}")
        resp = self._sim.handle(data)
        if resp is None:
            return None
        if self._verbose:
            log.debug(f"  PDU RX: {resp.hex(' ').upper()}")
        return list(resp)

    def check(self, label: str, condition: bool, detail: str = "") -> None:
        if condition:
            self.pass_count += 1
            log.info(f"  ✓  {label}")
        else:
            self.fail_count += 1
            log.error(f"  ✗  {label}  {detail}")

    def summary(self) -> bool:
        total = self.pass_count + self.fail_count
        log.info("")
        log.info(f"{'─' * 60}")
        log.info(f"  {self.pass_count}/{total} checks passed")
        if self.fail_count:
            log.error(f"  {self.fail_count} FAILURES")
        return self.fail_count == 0


# ---------------------------------------------------------------------------
# Real CAN transport (socketcan / virtual) — only used when --interface != simulator
# ---------------------------------------------------------------------------

class CanTransport:
    """Thin ISO-TP wrapper over python-can for real hardware / vcan0."""

    def __init__(self, channel: str, bustype: str, verbose: bool = False) -> None:
        try:
            import can as can_mod
        except ImportError:
            log.error("python-can is required for non-simulator mode: pip install python-can")
            sys.exit(1)
        self._bus     = can_mod.interface.Bus(channel=channel,
                                              bustype=bustype,
                                              bitrate=500000)
        self._verbose = verbose
        self.pass_count = 0
        self.fail_count = 0

    def request(self, payload: List[int]) -> Optional[List[int]]:
        import can as can_mod
        data = bytes(payload)
        frame = _isotp_sf(data)
        msg = can_mod.Message(arbitration_id=TESTER_TX_ID,
                              data=frame, is_extended_id=False)
        if self._verbose:
            log.debug(f"  TX: {frame.hex(' ').upper()}")
        self._bus.send(msg)

        # Collect response frames
        deadline = time.monotonic() + TIMEOUT_S
        frames: List[bytes] = []
        while time.monotonic() < deadline:
            rx = self._bus.recv(timeout=0.01)
            if rx is None or rx.arbitration_id != ECU_TX_ID:
                continue
            if self._verbose:
                log.debug(f"  RX: {bytes(rx.data).hex(' ').upper()}")
            pci = rx.data[0]
            ftype = (pci >> 4) & 0x0F
            if ftype == 0:    # SF — complete
                return list(_isotp_decode_sf(bytes(rx.data)) or [])
            if ftype == 1:    # FF — send FC and collect CFs
                frames.append(bytes(rx.data))
                import can as can_mod2
                fc = can_mod2.Message(arbitration_id=TESTER_TX_ID,
                                      data=bytes([0x30, 0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC]),
                                      is_extended_id=False)
                self._bus.send(fc)
            elif ftype == 2:  # CF
                frames.append(bytes(rx.data))
                decoded = _isotp_decode_mf(frames)
                if decoded is not None:
                    return list(decoded)
        return None

    def check(self, label: str, condition: bool, detail: str = "") -> None:
        if condition:
            self.pass_count += 1
            log.info(f"  ✓  {label}")
        else:
            self.fail_count += 1
            log.error(f"  ✗  {label}  {detail}")

    def summary(self) -> bool:
        total = self.pass_count + self.fail_count
        log.info("")
        log.info(f"{'─' * 60}")
        log.info(f"  {self.pass_count}/{total} checks passed")
        if self.fail_count:
            log.error(f"  {self.fail_count} FAILURES")
        return self.fail_count == 0

    def shutdown(self) -> None:
        self._bus.shutdown()


# ---------------------------------------------------------------------------
# Demo phases
# ---------------------------------------------------------------------------

def _decode_temp(raw: int) -> int:
    return raw - TEMP_OFFSET

def _dtc_name(code: int) -> str:
    return {
        DTC_TEMP_HIGH:    "0xD00101 (AmbientTemp over-range)",
        DTC_TEMP_LOW:     "0xD00102 (AmbientTemp under-range)",
        DTC_VOLTAGE_HIGH: "0xD00201 (SupplyVoltage over-range)",
        DTC_VOLTAGE_LOW:  "0xD00202 (SupplyVoltage under-range)",
    }.get(code, f"0x{code:06X}")


def phase1_normal(t, sim: Optional[SensorEcuSimulator]) -> None:
    log.info("")
    log.info("━━━━  Phase 1 — Normal operation  ━━━━")

    resp = t.request([0x10, SESSION_DEFAULT])
    t.check("Session: default", resp and resp[0] == 0x50)

    resp = t.request([0x22, 0xD0, 0x01])
    ok = resp and resp[0] == 0x62 and len(resp) >= 4
    t.check("DID 0xD001 AmbientTemperature readable",
            ok, f"got {resp}")
    if ok:
        raw = resp[3]
        log.info(f"      Temperature: {_decode_temp(raw)} °C  (raw 0x{raw:02X})")

    resp = t.request([0x22, 0xD0, 0x02])
    ok = resp and resp[0] == 0x62 and len(resp) >= 5
    t.check("DID 0xD002 SupplyVoltage readable", ok, f"got {resp}")
    if ok:
        mv = struct.unpack('>H', bytes(resp[3:5]))[0]
        log.info(f"      Voltage: {mv} mV  ({mv/1000:.2f} V)")

    resp = t.request([0x22, 0xD0, 0x03])
    ok = resp and resp[0] == 0x62 and len(resp) >= 4
    t.check("DID 0xD003 SensorStatusBitmask readable", ok)
    if ok:
        s = resp[3]
        log.info(f"      Status: 0x{s:02X}  "
                 f"(temp_ok={bool(s & STATUS_TEMP_OK)}, "
                 f"volt_ok={bool(s & STATUS_VOLTAGE_OK)})")

    resp = t.request([0x19, 0x02, 0xFF])
    t.check("0x19 02 FF: no active DTCs in normal state",
            resp and resp[0] == 0x59 and len(resp) == 3,
            f"got {resp}")
    log.info("      No active DTCs ✓")


def phase2_over_temp(t, sim: Optional[SensorEcuSimulator]) -> None:
    log.info("")
    log.info("━━━━  Phase 2 — Over-temperature fault injection  ━━━━")

    if sim:
        sim.inject_temp(95)   # 95 °C > 85 °C threshold
        log.info("      [SIM] Injected temperature: 95 °C (threshold: 85 °C)")
    else:
        log.info("      [HW]  Waiting for ECU to report over-temp fault…")
        time.sleep(0.5)

    resp = t.request([0x22, 0xD0, 0x01])
    ok = resp and resp[0] == 0x62 and len(resp) >= 4
    t.check("DID 0xD001: reads injected temperature", ok)
    if ok:
        raw = resp[3]
        log.info(f"      Temperature: {_decode_temp(raw)} °C  (raw 0x{raw:02X})")

    resp = t.request([0x22, 0xD0, 0x03])
    ok = resp and resp[0] == 0x62 and len(resp) >= 4
    t.check("DID 0xD003: TEMP_FAULT_HIGH bit set",
            ok and bool(resp[3] & STATUS_TEMP_FAULT_HIGH),
            f"status=0x{resp[3]:02X}" if ok else f"got {resp}")
    if ok:
        log.info(f"      Status: 0x{resp[3]:02X}  (TEMP_FAULT_HIGH=1)")

    resp = t.request([0x19, 0x02, 0xFF])
    ok = resp and resp[0] == 0x59 and len(resp) >= 7
    t.check("0x19 02 FF: DTC_TEMP_HIGH (0xD00101) active", ok and (
        resp[3:6] == [0xD0, 0x01, 0x01] and resp[6] == 0x08
    ), f"got {resp}")
    if ok and len(resp) >= 7:
        code = (resp[3] << 16) | (resp[4] << 8) | resp[5]
        log.info(f"      Active DTC: {_dtc_name(code)}  status=0x{resp[6]:02X}")


def phase3_under_voltage(t, sim: Optional[SensorEcuSimulator]) -> None:
    log.info("")
    log.info("━━━━  Phase 3 — Under-voltage fault injection  ━━━━")

    if sim:
        sim.inject_voltage(7500)   # 7.5 V < 9.0 V threshold
        log.info("      [SIM] Injected voltage: 7,500 mV (threshold: 9,000 mV)")
    else:
        log.info("      [HW]  Waiting for ECU to report under-voltage fault…")
        time.sleep(0.5)

    resp = t.request([0x22, 0xD0, 0x02])
    ok = resp and resp[0] == 0x62 and len(resp) >= 5
    t.check("DID 0xD002: reads injected voltage", ok)
    if ok:
        mv = struct.unpack('>H', bytes(resp[3:5]))[0]
        log.info(f"      Voltage: {mv} mV  ({mv/1000:.2f} V)")

    resp = t.request([0x22, 0xD0, 0x03])
    ok = resp and resp[0] == 0x62 and len(resp) >= 4
    t.check("DID 0xD003: VOLTAGE_FAULT_LOW bit set",
            ok and bool(resp[3] & STATUS_VOLTAGE_FAULT_LOW),
            f"status=0x{resp[3]:02X}" if ok else f"got {resp}")


def phase4_read_both_dtcs(t, sim: Optional[SensorEcuSimulator]) -> None:
    log.info("")
    log.info("━━━━  Phase 4 — Read all active DTCs  ━━━━")

    resp = t.request([0x19, 0x01, 0xFF])
    ok = resp and resp[0] == 0x59
    t.check("0x19 01 FF: DTC count = 2", ok and resp[5] == 2,
            f"count={resp[5] if ok and len(resp)>=6 else '?'}")
    if ok and len(resp) >= 6:
        log.info(f"      Active DTC count: {resp[5]}")

    resp = t.request([0x19, 0x02, 0xFF])
    ok = resp and resp[0] == 0x59 and len(resp) >= 11
    t.check("0x19 02 FF: returns 2 DTCs", ok, f"got {resp}")
    if ok:
        i = 3
        dtc_count = 0
        while i + 3 < len(resp):
            code   = (resp[i] << 16) | (resp[i+1] << 8) | resp[i+2]
            status = resp[i+3]
            log.info(f"      DTC: {_dtc_name(code)}  status=0x{status:02X}  "
                     f"({'TEST_FAILED' if status == 0x08 else f'0x{status:02X}'})")
            i += 4
            dtc_count += 1
        t.check("Both DTCs have status TEST_FAILED (0x08)", dtc_count == 2)


def phase5_adjust_threshold(t, sim: Optional[SensorEcuSimulator]) -> None:
    log.info("")
    log.info("━━━━  Phase 5 — Adjust temperature threshold (DID 0xD010 write)  ━━━━")

    resp = t.request([0x10, SESSION_EXTENDED])
    t.check("Session: extended", resp and resp[0] == 0x50 and resp[1] == 0x03)

    resp = t.request([0x27, 0x01])
    ok = resp and resp[0] == 0x67 and resp[1] == 0x01 and len(resp) >= 10
    t.check("SecurityAccess: seed received", ok, f"got {resp}")
    if not ok:
        log.error("      Cannot continue phase 5 — seed request failed.")
        return

    seed = bytes(resp[2:10])
    log.info(f"      Seed: {seed.hex(' ').upper()}")
    key = _derive_key(seed, 1)
    log.info(f"      Key (AES-CMAC[:4]): {key.hex(' ').upper()}")

    resp = t.request([0x27, 0x02] + list(key))
    t.check("SecurityAccess: Level 1 granted",
            resp and resp[0] == 0x67 and resp[1] == 0x02, f"got {resp}")

    # Write new threshold: 100 °C (raw = 100 + 40 = 0x8C)
    new_threshold_raw = (100 + TEMP_OFFSET) & 0xFF
    log.info(f"      Writing TemperatureThresholdHigh = 100 °C (raw 0x{new_threshold_raw:02X})")
    resp = t.request([0x2E, 0xD0, 0x10, new_threshold_raw])
    t.check("DID 0xD010 write accepted (0x6E positive response)",
            resp and resp[0] == 0x6E, f"got {resp}")

    # Verify temp fault cleared (95 °C < new 100 °C threshold)
    resp = t.request([0x22, 0xD0, 0x03])
    ok = resp and resp[0] == 0x62 and len(resp) >= 4
    t.check("DID 0xD003: TEMP_FAULT_HIGH cleared after threshold raise",
            ok and not bool(resp[3] & STATUS_TEMP_FAULT_HIGH),
            f"status=0x{resp[3]:02X}" if ok else f"got {resp}")
    if ok:
        log.info(f"      Status: 0x{resp[3]:02X}  (TEMP_FAULT_HIGH=0 ✓)")

    resp = t.request([0x19, 0x01, 0xFF])
    ok = resp and resp[0] == 0x59 and len(resp) >= 6
    t.check("0x19 01 FF: DTC count reduced to 1",
            ok and resp[5] == 1, f"count={resp[5] if ok else '?'}")


def phase6_recovery(t, sim: Optional[SensorEcuSimulator]) -> None:
    log.info("")
    log.info("━━━━  Phase 6 — Voltage recovery  ━━━━")

    if sim:
        sim.inject_voltage(12500)   # Back to nominal
        log.info("      [SIM] Restored voltage: 12,500 mV")
    else:
        log.info("      [HW]  Waiting for ECU to recover…")
        time.sleep(0.5)

    resp = t.request([0x22, 0xD0, 0x02])
    ok = resp and resp[0] == 0x62 and len(resp) >= 5
    t.check("DID 0xD002: voltage back in nominal range", ok)
    if ok:
        mv = struct.unpack('>H', bytes(resp[3:5]))[0]
        log.info(f"      Voltage: {mv} mV  ({mv/1000:.2f} V)")

    resp = t.request([0x22, 0xD0, 0x03])
    ok = resp and resp[0] == 0x62 and len(resp) >= 4
    t.check("DID 0xD003: VOLTAGE_FAULT_LOW cleared",
            ok and not bool(resp[3] & STATUS_VOLTAGE_FAULT_LOW),
            f"status=0x{resp[3]:02X}" if ok else f"got {resp}")

    resp = t.request([0x19, 0x02, 0xFF])
    t.check("0x19 02 FF: no active DTCs after full recovery",
            resp and resp[0] == 0x59 and len(resp) == 3,
            f"got {resp}")
    if resp and resp[0] == 0x59 and len(resp) == 3:
        log.info("      No active DTCs ✓  — all faults cleared.")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="SensorECU DTC demo script — Xaloqi EDS",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Zero-dependency simulator mode (default):
  python3 tools/sim_tester_sensor.py

  # Against a running native_sim build on vcan0:
  python3 tools/sim_tester_sensor.py --interface socketcan --channel vcan0

  # Verbose ISO-TP frame trace:
  python3 tools/sim_tester_sensor.py --verbose
""")
    parser.add_argument("--interface", default="simulator",
                        choices=["simulator", "virtual", "socketcan", "pcan", "kvaser"],
                        help="CAN interface (default: simulator — no hardware required)")
    parser.add_argument("--channel", default="vcan0",
                        help="CAN channel for socketcan/pcan/kvaser (default: vcan0)")
    parser.add_argument("--phase", type=int, choices=range(1, 7), default=0,
                        help="Run a single phase only (default: run all 6)")
    parser.add_argument("--verbose", action="store_true",
                        help="Print raw UDS PDUs")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(message)s",
    )

    log.info("╔══════════════════════════════════════════════════════════╗")
    log.info("║  Xaloqi EDS — SensorECU DTC Demo                        ║")
    log.info("║  6-phase sequence: normal → fault → recovery             ║")
    log.info("╚══════════════════════════════════════════════════════════╝")
    log.info(f"  Interface: {args.interface}"
             + (f" · channel: {args.channel}" if args.interface != "simulator" else ""))

    sim: Optional[SensorEcuSimulator] = None
    if args.interface == "simulator":
        sim = SensorEcuSimulator()
        t = SimTransport(sim, verbose=args.verbose)
    else:
        t = CanTransport(args.channel, args.interface, verbose=args.verbose)
        sim = None

    phases = {
        1: phase1_normal,
        2: phase2_over_temp,
        3: phase3_under_voltage,
        4: phase4_read_both_dtcs,
        5: phase5_adjust_threshold,
        6: phase6_recovery,
    }

    try:
        if args.phase:
            phases[args.phase](t, sim)
        else:
            for fn in phases.values():
                fn(t, sim)
    finally:
        if hasattr(t, 'shutdown'):
            t.shutdown()

    ok = t.summary()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
