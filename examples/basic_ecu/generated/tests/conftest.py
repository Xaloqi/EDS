# =============================================================================
# GENERATED — do NOT edit manually.
# Regenerate: python3 tools/codegen.py --config <yaml> --out generated/ --test-gen
#
# ECU       : BasicECU
# Version   : 0.1.0
# Generated : 2026-04-21T07:31:56Z
#
# PURPOSE: pytest conftest — shared fixtures and ISO-TP transport layer.
#
# The IsoTpTransport class provides a clean UDS PDU interface over full
# ISO 15765-2 framing (SF / FF / CF / FC).  Tests receive and send raw
# UDS PDUs; ISO-TP segmentation is handled transparently.
#
# INTERFACE MODES (set via CLI or env-vars):
#   virtual    — python-can in-process bus  (default, no hardware needed)
#   socketcan  — Linux SocketCAN / vcan0
#   pcan       — PEAK PCAN USB
#   kvaser     — Kvaser USB
#   simulator  — built-in loopback echo simulator (zero dependencies)
#
# RUNNING:
#   pytest generated/tests/ -v                        # simulator mode
#   CAN_INTERFACE=socketcan CAN_CHANNEL=vcan0 \
#       pytest generated/tests/ -v                    # vcan0
#   pytest generated/tests/ -v --hil                  # hardware-in-loop
# =============================================================================

from __future__ import annotations

import os
import queue
import struct
import threading
import time
import logging
from typing import Optional, List, Tuple

import pytest

log = logging.getLogger("conftest.basicecu")

# ---------------------------------------------------------------------------
# Protocol constants (derived from YAML — must match ECU firmware)
# ---------------------------------------------------------------------------

TESTER_TX_ID:   int   = 2015   # Tester → ECU
ECU_TX_ID:      int   = 2024   # ECU    → Tester
CAN_EXTENDED:   bool  = False

P2_SERVER_MAX_MS:      int   = 25
P2_STAR_SERVER_MAX_MS: int   = 5000
S3_SERVER_TIMEOUT_MS:  int   = 5000

RESPONSE_TIMEOUT_S:    float = (P2_SERVER_MAX_MS + 50) / 1000.0
ISOTP_FC_TIMEOUT_S:    float = 0.250   # Flow-control wait timeout
ISOTP_CF_TIMEOUT_S:    float = 0.150   # Consecutive-frame separation timeout

ISO_TP_PADDING:        int   = 0xCC    # ISO 15765-2 §9.8.1 padding

SID_DIAGNOSTIC_SESSION_CONTROL: int = 0x10
SID_ROUTINE_CONTROL:            int = 0x31
SID_SECURITY_ACCESS:            int = 0x27
SID_TESTER_PRESENT:             int = 0x3E
SID_NEGATIVE_RESPONSE:          int = 0x7F
POSITIVE_RESPONSE_OFFSET:       int = 0x40
SESSION_DEFAULT:                int = 0x01
SESSION_PROGRAMMING:            int = 0x02
SESSION_EXTENDED:               int = 0x03

# ---------------------------------------------------------------------------
# UDS SID constants (used by test_services.py, test_did_*.py, and per-routine tests)
# ---------------------------------------------------------------------------

SID_ECU_RESET:          int = 0x11
SID_READ_DTC_INFO:      int = 0x19
SID_READ_DATA_BY_ID:    int = 0x22
SID_WRITE_DATA_BY_ID:   int = 0x2E
POSITIVE_RESPONSE_OFFSET: int = 0x40

# ---------------------------------------------------------------------------
# NRC codes (ISO 14229-1 §7.4)
# ---------------------------------------------------------------------------

NRC_INCORRECT_MSG_LEN:             int = 0x13
NRC_CONDITIONS_NOT_CORRECT:        int = 0x22
NRC_REQUEST_OUT_OF_RANGE:          int = 0x31
NRC_SECURITY_ACCESS_DENIED:        int = 0x33
NRC_INVALID_KEY:                   int = 0x35
NRC_SUB_FUNCTION_NOT_SUPPORTED:    int = 0x12
NRC_SERVICE_NOT_SUPPORTED_IN_SESSION: int = 0x7F

# ---------------------------------------------------------------------------
# SecurityAccess algorithm sizes (AES-128-CMAC — must match uds_security_algo.c)
# ---------------------------------------------------------------------------

ALGO_SEED_LEN: int = 8   # UDS_SECURITY_SEED_LEN
ALGO_KEY_LEN:  int = 4   # UDS_SECURITY_KEY_LEN



# ===========================================================================
# ISO 15765-2 framing helpers
# ===========================================================================

def _sf(payload: bytes) -> bytes:
    """Build Single Frame (payload 1-7 bytes)."""
    n = len(payload)
    assert 1 <= n <= 7, f"SF payload {n} bytes (max 7)"
    return bytes([n & 0x0F]) + payload + bytes([ISO_TP_PADDING] * (7 - n))


def _ff(total: int, first6: bytes) -> bytes:
    """Build First Frame header (2-byte PCI + 6 payload bytes)."""
    assert len(first6) == 6
    return bytes([0x10 | ((total >> 8) & 0x0F), total & 0xFF]) + first6


def _cf(sn: int, chunk: bytes) -> bytes:
    """Build Consecutive Frame (SN 1-15, chunk up to 7 bytes)."""
    n = len(chunk)
    assert 1 <= n <= 7
    return bytes([0x20 | (sn & 0x0F)]) + chunk + bytes([ISO_TP_PADDING] * (7 - n))


def _fc(bs: int = 0, stmin: int = 0) -> bytes:
    """Build Flow Control — CTS."""
    return bytes([0x30, bs & 0xFF, stmin & 0xFF]) + bytes([ISO_TP_PADDING] * 5)


def isotp_encode(payload: bytes) -> List[bytes]:
    """Segment a UDS PDU into a list of 8-byte CAN frames."""
    if len(payload) <= 7:
        return [_sf(payload)]
    frames: List[bytes] = [_ff(len(payload), payload[:6])]
    rest, sn = payload[6:], 1
    while rest:
        chunk, rest = rest[:7], rest[7:]
        frames.append(_cf(sn, chunk))
        sn = (sn % 15) + 1   # wraps 1-15
    return frames


def isotp_decode_sf(data: bytes) -> Optional[bytes]:
    """Decode a Single Frame. Returns payload bytes or None."""
    if not data:
        return None
    pci_type = (data[0] & 0xF0) >> 4
    if pci_type != 0:
        return None
    n = data[0] & 0x0F
    if n == 0 or n > 7 or n > len(data) - 1:
        return None
    return bytes(data[1:1 + n])


def isotp_decode_mf(frames: List[bytes]) -> Optional[bytes]:
    """
    Reassemble a multi-frame sequence (FF + CFs).
    Returns the complete payload or None on framing error.
    """
    if not frames:
        return None
    ff = frames[0]
    if (ff[0] & 0xF0) >> 4 != 1:
        return None
    total = ((ff[0] & 0x0F) << 8) | ff[1]
    buf = bytearray(ff[2:8])
    expected_sn = 1
    for cf in frames[1:]:
        if (cf[0] & 0xF0) >> 4 != 2:
            return None
        sn = cf[0] & 0x0F
        if sn != expected_sn:
            log.warning("CF SN mismatch: expected %d got %d", expected_sn, sn)
        buf.extend(cf[1:8])
        expected_sn = (expected_sn % 15) + 1
    return bytes(buf[:total])


# ===========================================================================
# Built-in ECU simulator
# Handles all UDS services needed by the test suite without real firmware.
# Used when --can-interface=simulator (default in CI).
# ===========================================================================

class _EcuSimulator:
    """
    Minimal UDS ECU simulator for offline test execution.

    Implements correct protocol responses for:
      0x10  DiagnosticSessionControl
      0x27  SecurityAccess (AES-CMAC key derivation matching firmware)
      0x3E  TesterPresent
      0x22  ReadDataByIdentifier  (all configured DIDs)
      0x2E  WriteDataByIdentifier (all write-capable DIDs)
      0x19  ReadDTCInformation    (sub-functions 0x01 and 0x02)
      0x11  ECUReset
      0x31  RoutineControl        (all configured routines, session+security gated)
      0x7F  Negative responses for all constraint violations
    """

    _SESSION_NAMES = {0x01: "default", 0x02: "programming", 0x03: "extended"}
    _SESSION_ORDINALS = {0x01: 1, 0x02: 2, 0x03: 3}

    # Placeholder AES-128 keys — must match s_level_keys[] in uds_security_algo.c
    _LEVEL_KEYS = {
        1: bytes([0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                  0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F]),
        2: bytes([0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
                  0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F]),
    }

    # DID catalogue mirroring diagnostics_config.yaml
    _DIDS = {
        61840: {
            "name":         "Vehicle Identification Number",
            "data_length":  17,
            "access_read":  True,
            "access_write": False,
            "min_session":  1,
            "read_sec":     0,
            "write_sec":    1,
            "data":         bytes([0xAA] * 17),  # default value
        },
        61836: {
            "name":         "ECU Serial Number",
            "data_length":  4,
            "access_read":  True,
            "access_write": False,
            "min_session":  1,
            "read_sec":     0,
            "write_sec":    1,
            "data":         bytes([0xAA] * 4),  # default value
        },
        61831: {
            "name":         "Vehicle Manufacturer Spare Part Number",
            "data_length":  11,
            "access_read":  True,
            "access_write": True,
            "min_session":  3,
            "read_sec":     0,
            "write_sec":    1,
            "data":         bytes([0xAA] * 11),  # default value
        },
        3072: {
            "name":         "Engine Speed",
            "data_length":  2,
            "access_read":  True,
            "access_write": False,
            "min_session":  1,
            "read_sec":     0,
            "write_sec":    0,
            "data":         bytes([0xAA] * 2),  # default value
        },
        1280: {
            "name":         "Coolant Temperature",
            "data_length":  1,
            "access_read":  True,
            "access_write": False,
            "min_session":  1,
            "read_sec":     0,
            "write_sec":    0,
            "data":         bytes([0xAA] * 1),  # default value
        },
    }


    # Routine catalogue mirroring diagnostics_config.yaml — used by _rc() simulator.
    # session ordinals: default=1, programming=2, extended=3
    # uds_safety.c: active_ordinal >= required_ordinal (extended satisfies programming)
    _ROUTINES = {
        65280: {
            "name":        "ECU_SelfTest",
            "min_session": 3,
            "sec_level":   0,
            "support": {"start","results",            },
        },
        65281: {
            "name":        "ResetToFactoryDefaults",
            "min_session": 3,
            "sec_level":   1,
            "support": {"start",            },
        },
        65296: {
            "name":        "EraseNVM",
            "min_session": 2,
            "sec_level":   1,
            "support": {"start","results",            },
        },
    }

    # DTC catalogue mirroring diagnostics_config.yaml
    _DTCS = [
        {"code": 12583168, "status": 0x00},
        {"code": 12583424, "status": 0x00},
    ]

    def __init__(self) -> None:
        self._session:      int             = 0x01   # default
        self._sec_unlocked: dict            = {}     # level → bool
        self._pending_seed: Optional[bytes] = None
        self._pending_level: Optional[int]  = None
        self._seq:          int             = 1
        self._did_store:    dict            = {
            k: bytearray(v["data"]) for k, v in self._DIDS.items()
        }
        self._reset_pending: bool = False

    # ── AES-128-CMAC (pure Python, no external deps) ─────────────────────
    @staticmethod
    def _aes_enc(key: bytes, block: bytes) -> bytes:
        """AES-128 ECB encryption — pure Python, matches uds_aes_cmac.c."""
        # S-box
        S = [
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
        def xtime(b: int) -> int:
            return ((b << 1) ^ 0x1B) & 0xFF if b & 0x80 else (b << 1) & 0xFF
        def mul(a: int, b: int) -> int:
            r = 0
            for _ in range(8):
                if b & 1: r ^= a
                hi = a & 0x80; a = (a << 1) & 0xFF
                if hi: a ^= 0x1B
                b >>= 1
            return r

        # Key expansion
        def key_expand(k: bytes):
            RCON = [0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1B,0x36]
            w = [list(k[i*4:(i+1)*4]) for i in range(4)]
            for i in range(4, 44):
                t = list(w[i-1])
                if i % 4 == 0:
                    t = [S[t[1]]^RCON[i//4-1], S[t[2]], S[t[3]], S[t[0]]]
                w.append([w[i-4][j]^t[j] for j in range(4)])
            return [[w[r*4+c][b] for c in range(4) for r_  in [0] for b in range(1)] for r in range(11)], w

        rk_words = []
        t = [list(key[i*4:(i+1)*4]) for i in range(4)]
        RCON = [0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1B,0x36]
        rk_words.extend(t)
        for i in range(4, 44):
            prev = list(rk_words[i-1])
            if i % 4 == 0:
                prev = [S[prev[1]]^RCON[i//4-1], S[prev[2]], S[prev[3]], S[prev[0]]]
            rk_words.append([rk_words[i-4][j] ^ prev[j] for j in range(4)])
        round_keys = [
            [[rk_words[r*4+c][b] for b in range(4)] for c in range(4)]
            for r in range(11)
        ]

        # State as 4×4 column-major (matches AES spec)
        state = [[block[r + 4*c] for c in range(4)] for r in range(4)]

        def add_round_key(s, rk):
            return [[s[r][c] ^ rk[c][r] for c in range(4)] for r in range(4)]

        def sub_bytes(s):
            return [[S[s[r][c]] for c in range(4)] for r in range(4)]

        def shift_rows(s):
            return [
                [s[0][0],s[0][1],s[0][2],s[0][3]],
                [s[1][1],s[1][2],s[1][3],s[1][0]],
                [s[2][2],s[2][3],s[2][0],s[2][1]],
                [s[3][3],s[3][0],s[3][1],s[3][2]],
            ]

        def mix_col(col):
            return [
                mul(0x02,col[0])^mul(0x03,col[1])^col[2]^col[3],
                col[0]^mul(0x02,col[1])^mul(0x03,col[2])^col[3],
                col[0]^col[1]^mul(0x02,col[2])^mul(0x03,col[3]),
                mul(0x03,col[0])^col[1]^col[2]^mul(0x02,col[3]),
            ]

        def mix_columns(s):
            cols = [[s[r][c] for r in range(4)] for c in range(4)]
            mixed = [mix_col(c) for c in cols]
            return [[mixed[c][r] for c in range(4)] for r in range(4)]

        state = add_round_key(state, round_keys[0])
        for rnd in range(1, 10):
            state = sub_bytes(state)
            state = shift_rows(state)
            state = mix_columns(state)
            state = add_round_key(state, round_keys[rnd])
        state = sub_bytes(state)
        state = shift_rows(state)
        state = add_round_key(state, round_keys[10])

        return bytes(state[r][c] for c in range(4) for r in range(4))

    @classmethod
    def _cmac(cls, key: bytes, msg: bytes) -> bytes:
        """AES-128-CMAC per RFC 4493."""
        def _pad16(b: bytes) -> bytes:
            n = len(b) % 16
            if n == 0:
                return b
            return b + bytes([0x80] + [0x00] * (15 - n))

        def _xor16(a: bytes, b: bytes) -> bytes:
            return bytes(x ^ y for x, y in zip(a, b))

        def _gen_subkeys(k: bytes):
            R128 = bytes([0]*15 + [0x87])
            L = cls._aes_enc(k, bytes(16))
            K1 = bytes([(L[i] << 1) & 0xFF | (L[i+1] >> 7) for i in range(15)] + [(L[15] << 1) & 0xFF])
            if L[0] & 0x80:
                K1 = _xor16(K1, R128)
            K2 = bytes([(K1[i] << 1) & 0xFF | (K1[i+1] >> 7) for i in range(15)] + [(K1[15] << 1) & 0xFF])
            if K1[0] & 0x80:
                K2 = _xor16(K2, R128)
            return K1, K2

        K1, K2 = _gen_subkeys(key)
        # Split into 16-byte blocks
        if len(msg) == 0:
            msg = bytes([0])
        blocks = [msg[i:i+16] for i in range(0, len(msg), 16)]
        # Last block padding
        if len(blocks[-1]) == 16:
            last = _xor16(blocks[-1], K1)
        else:
            last = _xor16(_pad16(blocks[-1]), K2)
        blocks[-1] = last
        X = bytes(16)
        for blk in blocks:
            X = cls._aes_enc(key, _xor16(X, blk))
        return X

    def _derive_key(self, seed: bytes, level: int) -> bytes:
        """Derive 4-byte key from 8-byte seed using AES-128-CMAC."""
        key = self._LEVEL_KEYS.get(level, self._LEVEL_KEYS[1])
        mac = self._cmac(key, seed)
        return mac[:4]

    def _make_seed(self, level: int) -> bytes:
        """Generate an 8-byte seed (6 random-ish nonce + 2 sequence bytes)."""
        import os as _os
        nonce = _os.urandom(6)
        seq_hi = (self._seq >> 8) & 0xFF
        seq_lo = self._seq & 0xFF
        self._seq = (self._seq % 0xFFFE) + 1
        return nonce + bytes([seq_hi, seq_lo])

    # ── Request dispatcher ────────────────────────────────────────────────
    def handle(self, pdu: bytes) -> Optional[bytes]:
        """Process a UDS PDU and return the response PDU (or None = suppress)."""
        if not pdu:
            return bytes([0x7F, 0x00, 0x13])

        sid = pdu[0]

        if sid == 0x10:   return self._dsc(pdu)
        if sid == 0x3E:   return self._tp(pdu)
        if sid == 0x27:   return self._sa(pdu)
        if sid == 0x22:   return self._rdbi(pdu)
        if sid == 0x2E:   return self._wdbi(pdu)
        if sid == 0x19:   return self._rdtc(pdu)
        if sid == 0x11:   return self._reset(pdu)
        if sid == 0x31:   return self._rc(pdu)

        return bytes([0x7F, sid, 0x11])   # serviceNotSupported

    def _nrc(self, sid: int, nrc: int) -> bytes:
        return bytes([0x7F, sid, nrc])

    def _dsc(self, pdu: bytes) -> Optional[bytes]:
        if len(pdu) < 2:
            return self._nrc(0x10, 0x13)
        sub = pdu[1] & 0x7F
        suppress = bool(pdu[1] & 0x80)
        valid = {0x01, 0x02, 0x03}
        if sub not in valid:
            return self._nrc(0x10, 0x12)
        self._session = sub
        self._sec_unlocked.clear()
        self._pending_seed = None
        self._pending_level = None
        if suppress:
            return None
        p2_hi = 0
        p2_lo = 25
        p2s_hi = 19
        p2s_lo = 136
        return bytes([0x50, sub, p2_hi, p2_lo, p2s_hi, p2s_lo])

    def _tp(self, pdu: bytes) -> Optional[bytes]:
        if len(pdu) < 2:
            return self._nrc(0x3E, 0x13)
        sub = pdu[1] & 0x7F
        suppress = bool(pdu[1] & 0x80)
        if sub != 0x00:
            return self._nrc(0x3E, 0x12)
        if suppress:
            return None
        return bytes([0x7E, 0x00])

    def _sa(self, pdu: bytes) -> bytes:
        if len(pdu) < 2:
            return self._nrc(0x27, 0x13)
        if self._session == 0x01:
            return self._nrc(0x27, 0x7F)
        sub = pdu[1]
        # Odd sub = requestSeed, even sub = sendKey
        if sub % 2 == 1:   # requestSeed
            level = (sub + 1) // 2
            if self._sec_unlocked.get(level):
                return bytes([0x67, sub] + [0x00] * 8)
            seed = self._make_seed(level)
            self._pending_seed = seed
            self._pending_level = level
            return bytes([0x67, sub]) + seed
        else:               # sendKey
            level = sub // 2
            if self._pending_seed is None or self._pending_level != level:
                return self._nrc(0x27, 0x24)
            expected_key_len = 4
            if len(pdu) != 2 + expected_key_len:
                self._pending_seed = None
                return self._nrc(0x27, 0x13)
            received = pdu[2:]
            expected = self._derive_key(self._pending_seed, level)
            self._pending_seed = None
            self._pending_level = None
            if bytes(received) != bytes(expected):
                return self._nrc(0x27, 0x35)
            self._sec_unlocked[level] = True
            return bytes([0x67, sub])

    def _rdbi(self, pdu: bytes) -> bytes:
        if len(pdu) < 3:
            return self._nrc(0x22, 0x13)
        # Parse one or more DID IDs
        resp = bytearray([0x62])
        data_part = pdu[1:]
        if len(data_part) % 2 != 0:
            return self._nrc(0x22, 0x13)
        for i in range(0, len(data_part), 2):
            did_id = (data_part[i] << 8) | data_part[i+1]
            entry = self._DIDS.get(did_id)
            if entry is None:
                return self._nrc(0x22, 0x31)
            sess_ok = self._SESSION_ORDINALS.get(self._session, 1) >= entry["min_session"]
            if not sess_ok:
                return self._nrc(0x22, 0x31)
            if entry["read_sec"] > 0 and not self._sec_unlocked.get(entry["read_sec"]):
                return self._nrc(0x22, 0x33)
            resp.extend([data_part[i], data_part[i+1]])
            resp.extend(self._did_store.get(did_id, bytearray(entry["data_length"])))
        return bytes(resp)

    def _wdbi(self, pdu: bytes) -> bytes:
        if len(pdu) < 4:
            return self._nrc(0x2E, 0x13)
        did_id = (pdu[1] << 8) | pdu[2]
        entry = self._DIDS.get(did_id)
        if entry is None:
            return self._nrc(0x2E, 0x31)
        if not entry["access_write"]:
            return self._nrc(0x2E, 0x31)
        sess_ord = self._SESSION_ORDINALS.get(self._session, 1)
        if sess_ord < entry["min_session"]:
            return self._nrc(0x2E, 0x31)
        if entry["write_sec"] > 0 and not self._sec_unlocked.get(entry["write_sec"]):
            return self._nrc(0x2E, 0x33)
        data = pdu[3:]
        if len(data) != entry["data_length"]:
            return self._nrc(0x2E, 0x13)
        self._did_store[did_id] = bytearray(data)
        return bytes([0x6E, pdu[1], pdu[2]])

    def _rdtc(self, pdu: bytes) -> bytes:
        if len(pdu) < 2:
            return self._nrc(0x19, 0x13)
        sub = pdu[1]
        if sub == 0x01:   # reportNumberOfDTCByStatusMask
            if len(pdu) < 3:
                return self._nrc(0x19, 0x13)
            n = len(self._DTCS)
            return bytes([0x59, 0x01, 0xFF, 0x01, (n >> 8) & 0xFF, n & 0xFF])
        if sub == 0x02:   # reportDTCByStatusMask
            if len(pdu) < 3:
                return self._nrc(0x19, 0x13)
            resp = bytearray([0x59, 0x02, 0xFF])
            for dtc in self._DTCS:
                code = dtc["code"]
                resp.extend([(code >> 16) & 0xFF, (code >> 8) & 0xFF, code & 0xFF, dtc["status"]])
            return bytes(resp)
        return self._nrc(0x19, 0x12)

    def _rc(self, pdu: bytes) -> bytes:
        """
        SID 0x31 — RoutineControl simulator handler.

        Implements the same session, security, and support-flag gating as
        service_0x31.c so simulator tests exercise identical NRC codes to
        firmware tests without requiring a compiled binary.

        Request:  [0x31, subFn, RID_hi, RID_lo {, routineOptionRecord}]
        Response: [0x71, subFn, RID_hi, RID_lo {, routineStatusRecord}]

        NRC 0x13  request too short (< 4 bytes)
        NRC 0x12  subFn not in {0x01,0x02,0x03}, or stop/results not in support
        NRC 0x31  RID not in _ROUTINES catalogue
        NRC 0x7F  active session ordinal < routine min_session ordinal
        NRC 0x33  security level not unlocked (sec_level > 0)
        """
        if len(pdu) < 4:
            return self._nrc(0x31, 0x13)
        sub_fn = pdu[1]
        if sub_fn < 0x01 or sub_fn > 0x03:
            return self._nrc(0x31, 0x12)
        rid = (pdu[2] << 8) | pdu[3]
        entry = self._ROUTINES.get(rid)
        if entry is None:
            return self._nrc(0x31, 0x31)   # requestOutOfRange — RID not registered
        sess_ord = self._SESSION_ORDINALS.get(self._session, 1)
        if sess_ord < entry["min_session"]:
            return self._nrc(0x31, 0x7F)   # serviceNotSupportedInActiveSession
        if entry["sec_level"] > 0 and not self._sec_unlocked.get(entry["sec_level"]):
            return self._nrc(0x31, 0x33)   # securityAccessDenied
        if sub_fn == 0x01 and "start"   not in entry["support"]:
            return self._nrc(0x31, 0x12)
        if sub_fn == 0x02 and "stop"    not in entry["support"]:
            return self._nrc(0x31, 0x12)
        if sub_fn == 0x03 and "results" not in entry["support"]:
            return self._nrc(0x31, 0x12)
        # Positive response — stub returns no routineStatusRecord (zero result bytes)
        return bytes([0x71, sub_fn, pdu[2], pdu[3]])

    def _reset(self, pdu: bytes) -> bytes:
        if len(pdu) < 2:
            return self._nrc(0x11, 0x13)
        sub = pdu[1] & 0x7F
        if sub not in (0x01, 0x02, 0x03):
            return self._nrc(0x11, 0x12)
        self._session = 0x01
        self._sec_unlocked.clear()
        return bytes([0x51, sub])


# ===========================================================================
# IsoTpTransport
# Wraps CAN bus access with full ISO-TP SF/FF/CF/FC framing.
# ===========================================================================

class IsoTpTransport:
    """
    Full ISO 15765-2 transport over python-can (or built-in simulator).

    Call request(pdu) → get back the reassembled UDS response PDU.
    All ISO-TP framing (segmentation, flow control, reassembly) is handled
    internally.  Tests only see UDS PDU bytes.
    """

    def __init__(
        self,
        bus,
        tester_tx_id: int = TESTER_TX_ID,
        ecu_tx_id:    int = ECU_TX_ID,
        extended_id:  bool = CAN_EXTENDED,
        simulator:    Optional[_EcuSimulator] = None,
    ) -> None:
        self._bus         = bus
        self._tx_id       = tester_tx_id
        self._rx_id       = ecu_tx_id
        self._extended    = extended_id
        self._sim         = simulator
        self._rx_q: queue.Queue = queue.Queue()
        self._rx_thread: Optional[threading.Thread] = None
        if bus is not None and simulator is None:
            self._start_rx()

    def _start_rx(self) -> None:
        self._running = True
        self._rx_thread = threading.Thread(target=self._rx_loop, daemon=True)
        self._rx_thread.start()

    def _rx_loop(self) -> None:
        while self._running:
            try:
                msg = self._bus.recv(timeout=0.05)
                if msg and msg.arbitration_id == self._rx_id:
                    self._rx_q.put(bytes(msg.data), block=False)
            except Exception:
                pass

    def _send_frame(self, data: bytes) -> None:
        if self._sim is not None:
            return   # simulator handles internally
        try:
            import can as _can
            msg = _can.Message(
                arbitration_id=self._tx_id,
                data=data,
                is_extended_id=self._extended,
            )
            self._bus.send(msg)
        except Exception as exc:
            log.error("CAN send error: %s", exc)

    def _recv_frame(self, timeout: float) -> Optional[bytes]:
        """Receive one CAN frame, respecting timeout."""
        try:
            return self._rx_q.get(timeout=timeout)
        except queue.Empty:
            return None

    def request(self, pdu: bytes, timeout: float = RESPONSE_TIMEOUT_S) -> Optional[bytes]:
        """
        Send a UDS PDU over ISO-TP and return the reassembled response PDU.

        Args:
            pdu:     Raw UDS PDU bytes (SID + data).
            timeout: Maximum seconds to wait for a complete response.

        Returns:
            Reassembled UDS PDU bytes, or None if timeout or suppress.
        """
        if self._sim is not None:
            return self._sim.handle(pdu)

        frames = isotp_encode(pdu)
        deadline = time.monotonic() + timeout

        if len(frames) == 1:
            # Single Frame — send directly
            self._send_frame(frames[0])
        else:
            # Multi-frame TX: send FF, wait for FC, send CFs
            self._send_frame(frames[0])
            fc_raw = self._recv_frame(ISOTP_FC_TIMEOUT_S)
            if fc_raw is None or (fc_raw[0] & 0xF0) >> 4 != 3:
                log.warning("No valid Flow Control received (got %s)", fc_raw)
                return None
            bs   = fc_raw[1]         # block size (0 = unlimited)
            stmin_raw = fc_raw[2]
            stmin_s = stmin_raw / 1000.0 if stmin_raw <= 0x7F else 0.001
            sent = 0
            for cf in frames[1:]:
                time.sleep(stmin_s)
                self._send_frame(cf)
                sent += 1
                if bs > 0 and sent >= bs:
                    fc2 = self._recv_frame(ISOTP_FC_TIMEOUT_S)
                    if fc2 is None:
                        return None
                    bs = fc2[1]
                    sent = 0

        # Receive response
        remaining = deadline - time.monotonic()
        first = self._recv_frame(max(remaining, 0.0))
        if first is None:
            return None

        pci_type = (first[0] & 0xF0) >> 4

        if pci_type == 0:   # Single Frame response
            return isotp_decode_sf(first)

        if pci_type == 1:   # First Frame — need to send FC and collect CFs
            total = ((first[0] & 0x0F) << 8) | first[1]
            self._send_frame(_fc(bs=0, stmin=0))
            mf_frames = [first]
            collected = 6
            while collected < total:
                remaining = deadline - time.monotonic()
                cf = self._recv_frame(max(remaining, 0.05))
                if cf is None:
                    log.warning("Timeout waiting for CF (collected %d / %d)", collected, total)
                    return None
                mf_frames.append(cf)
                collected += 7
            return isotp_decode_mf(mf_frames)

        return None

    def close(self) -> None:
        """Stop the RX thread and release the CAN bus."""
        if hasattr(self, '_running'):
            self._running = False
        if self._rx_thread:
            self._rx_thread.join(timeout=0.5)
        if self._bus and self._sim is None:
            try:
                self._bus.shutdown()
            except Exception:
                pass


# ===========================================================================
# pytest CLI options and fixtures
# ===========================================================================

def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption(
        "--can-interface", default=os.environ.get("CAN_INTERFACE", "simulator"),
        help="CAN interface: simulator|virtual|socketcan|pcan|kvaser (default: simulator)"
    )
    parser.addoption(
        "--can-channel", default=os.environ.get("CAN_CHANNEL", "vcan0"),
        help="CAN channel name (default: vcan0)"
    )
    parser.addoption(
        "--can-bitrate", type=int, default=int(os.environ.get("CAN_BITRATE", "500000")),
        help="CAN bus bitrate in bps (default: 500000)"
    )
    parser.addoption(
        "--hil", action="store_true", default=False,
        help="Enable hardware-in-loop tests (requires real ECU)"
    )
    parser.addoption(
        "--sec-key-l1", default=None,
        help="Hex AES-128 key for security level 1 (overrides placeholder default)"
    )
    parser.addoption(
        "--sec-key-l2", default=None,
        help="Hex AES-128 key for security level 2 (overrides placeholder default)"
    )


@pytest.fixture(scope="session")
def can_interface(request: pytest.FixtureRequest) -> str:
    return request.config.getoption("--can-interface")


@pytest.fixture(scope="session")
def is_hil(request: pytest.FixtureRequest) -> bool:
    return bool(request.config.getoption("--hil"))


@pytest.fixture(scope="session")
def aes_keys(request: pytest.FixtureRequest) -> dict:
    """Return the AES-128 keys to use for SecurityAccess key derivation."""
    keys = {
        1: bytes([0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                  0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F]),
        2: bytes([0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
                  0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F]),
    }
    k1 = request.config.getoption("--sec-key-l1")
    k2 = request.config.getoption("--sec-key-l2")
    if k1:
        keys[1] = bytes.fromhex(k1.replace("0x", "").replace(" ", ""))
    if k2:
        keys[2] = bytes.fromhex(k2.replace("0x", "").replace(" ", ""))
    return keys


@pytest.fixture(scope="function")
def uds_bus(
    can_interface: str,
    request: pytest.FixtureRequest,
    aes_keys: dict,
) -> IsoTpTransport:
    """
    Per-test IsoTpTransport fixture.

    Provides a fresh transport for each test.  After each test, the ECU is
    returned to Default Session automatically.

    Modes:
      simulator  — built-in ECU simulator, no hardware required
      virtual    — python-can VirtualBus (in-process)
      socketcan  — Linux SocketCAN (requires vcan0)
      pcan/kvaser — physical adapters
    """
    transport: IsoTpTransport

    if can_interface == "simulator":
        sim = _EcuSimulator()
        # Inject custom AES keys if provided
        for lvl, key in aes_keys.items():
            if lvl in sim._LEVEL_KEYS:
                sim._LEVEL_KEYS[lvl] = key
        transport = IsoTpTransport(bus=None, simulator=sim)
    else:
        try:
            import can as _can
        except ImportError:
            pytest.skip("python-can not installed — run: pip install python-can")

        iface   = can_interface
        channel = request.config.getoption("--can-channel")
        bitrate = request.config.getoption("--can-bitrate")

        try:
            if iface == "virtual":
                bus = _can.Bus(interface="virtual", channel=channel)
            elif iface == "socketcan":
                bus = _can.Bus(interface="socketcan", channel=channel)
            elif iface in ("pcan", "kvaser"):
                bus = _can.Bus(interface=iface, channel=channel, bitrate=bitrate)
            else:
                pytest.skip(f"Unknown CAN interface: {iface!r}")
        except Exception as exc:
            pytest.skip(f"Cannot open CAN bus ({iface}/{channel}): {exc}")

        transport = IsoTpTransport(bus=bus)

    yield transport

    # Teardown: restore default session
    try:
        transport.request(
            bytes([SID_DIAGNOSTIC_SESSION_CONTROL, SESSION_DEFAULT]),
            timeout=0.1,
        )
    except Exception:
        pass
    transport.close()
