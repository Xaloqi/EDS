# =============================================================================
# GENERATED — do NOT edit manually.
# Regenerate: python3 tools/testgen.py --config <yaml> --out generated/
#
# ECU       : ARDEP_IOController
# Version   : 1.0.0
# Generated : 2026-05-20T07:21:48Z
#
# PURPOSE: pytest conftest — shared fixtures backed by xaloqi-tester.
#
# Replaces 870 lines of inline ISO-TP / AES-128 / ECU simulator with
# xaloqi-tester imports.  Public API is identical — all test files
# (test_did_*.py, test_services.py, test_routine_*.py) work unchanged:
#
#   IsoTpTransport.request(pdu)  ->  raw UDS PDU response bytes
#   _EcuSimulator                ->  importable for key derivation
#   All SID/NRC/SESSION constants
#   uds_bus fixture (function scope)
#
# INTERFACE MODES (CAN_INTERFACE env var or --can-interface CLI flag):
#   simulator  — built-in ECU simulator via xaloqi-tester (default)
#   virtual    — xaloqi-tester VirtualBus (in-process)
#   socketcan  — Linux SocketCAN / vcan0
#   pcan       — PEAK PCAN USB
#   kvaser     — Kvaser USB
#
# DEPENDENCIES: see requirements_testgen.txt
#   xaloqi-tester >= 1.0.0
#   pytest >= 7.0
# =============================================================================

from __future__ import annotations
import asyncio, os, logging
from typing import Optional
import pytest

try:
    from xaloqi.tester import (
        UdsTester, NrcError,
        TimeoutError as UdsTimeoutError, TransportError,
    )
    from xaloqi.tester.transport.virtual import VirtualBus
    from xaloqi.tester._security import aes_cmac, derive_key
    _XALOQI_AVAILABLE = True
except ImportError:
    _XALOQI_AVAILABLE = False

log = logging.getLogger("conftest.ardep_iocontroller")

# ---------------------------------------------------------------------------
# Protocol constants
# ---------------------------------------------------------------------------

TESTER_TX_ID:   int   = 2015
ECU_TX_ID:      int   = 2024
CAN_EXTENDED:   bool  = False
P2_SERVER_MAX_MS:      int = 25
P2_STAR_SERVER_MAX_MS: int = 5000
S3_SERVER_TIMEOUT_MS:  int = 5000
RESPONSE_TIMEOUT_S:    float = (P2_SERVER_MAX_MS + 50) / 1000.0

SID_DIAGNOSTIC_SESSION_CONTROL: int = 0x10
SID_ECU_RESET:                  int = 0x11
SID_READ_DTC_INFO:              int = 0x19
SID_READ_DATA_BY_ID:            int = 0x22
SID_SECURITY_ACCESS:            int = 0x27
SID_WRITE_DATA_BY_ID:           int = 0x2E
SID_ROUTINE_CONTROL:            int = 0x31
SID_TESTER_PRESENT:             int = 0x3E
SID_NEGATIVE_RESPONSE:          int = 0x7F
POSITIVE_RESPONSE_OFFSET:       int = 0x40
SESSION_DEFAULT:                int = 0x01
SESSION_PROGRAMMING:            int = 0x02
SESSION_EXTENDED:               int = 0x03
NRC_INCORRECT_MSG_LEN:                int = 0x13
NRC_CONDITIONS_NOT_CORRECT:           int = 0x22
NRC_REQUEST_OUT_OF_RANGE:             int = 0x31
NRC_SECURITY_ACCESS_DENIED:           int = 0x33
NRC_INVALID_KEY:                      int = 0x35
NRC_SUB_FUNCTION_NOT_SUPPORTED:       int = 0x12
NRC_SERVICE_NOT_SUPPORTED_IN_SESSION: int = 0x7F
ALGO_SEED_LEN: int = 8
ALGO_KEY_LEN:  int = 4

_LEVEL_KEYS: dict = {
    1: bytes([0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
              0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F]),
    2: bytes([0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
              0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F]),
}
_SESSION_ORDINALS: dict = {0x01: 1, 0x02: 2, 0x03: 3}


# ---------------------------------------------------------------------------
# _EcuSimulator — compatibility shim
# test_did_*.py imports _EcuSimulator for _derive_key / _cmac.
# Delegates to xaloqi-tester._security — no inline AES needed.
# ---------------------------------------------------------------------------

class _EcuSimulator:
    _LEVEL_KEYS: dict = _LEVEL_KEYS

    @classmethod
    def _cmac(cls, key: bytes, msg: bytes) -> bytes:
        if _XALOQI_AVAILABLE:
            return aes_cmac(key, msg)
        try:
            from Crypto.Cipher import AES as _AES
            from Crypto.Hash import CMAC as _CMAC
            c = _CMAC.new(key, ciphermod=_AES); c.update(msg); return c.digest()
        except ImportError:
            raise ImportError(
                'xaloqi-tester or pycryptodome required. '
                'Run: pip install -r requirements_testgen.txt'
            )

    def _derive_key(self, seed: bytes, level: int) -> bytes:
        if _XALOQI_AVAILABLE:
            return derive_key(seed, level)
        key = self._LEVEL_KEYS.get(level, self._LEVEL_KEYS[1])
        return self._cmac(key, seed)[:4]


# ---------------------------------------------------------------------------
# IsoTpTransport — backward-compatible adapter over UdsTester.raw_request()
# ---------------------------------------------------------------------------

class IsoTpTransport:
    def __init__(self, tester: UdsTester, loop: asyncio.AbstractEventLoop) -> None:
        self._tester = tester
        self._loop   = loop

    def request(self, pdu: bytes, timeout: float = RESPONSE_TIMEOUT_S) -> Optional[bytes]:
        async def _req():
            try:
                return await self._tester.raw_request(pdu, timeout=timeout)
            except NrcError as e:
                return bytes([0x7F, e.sid, e.nrc])
            except (UdsTimeoutError, TransportError):
                return None
            except Exception:
                return None
        try:
            return self._loop.run_until_complete(_req())
        except Exception:
            return None

    def close(self) -> None:
        try:
            self._loop.run_until_complete(self._tester.__aexit__(None, None, None))
        except Exception:
            pass
        finally:
            self._loop.close()


# ---------------------------------------------------------------------------
# Inline ECU simulator state — mirrors diagnostics_config.yaml
# Used when --can-interface=simulator (default in CI).
# ---------------------------------------------------------------------------

class _InlineEcuState:
    def __init__(self) -> None:
        self.session: int = 0x01
        self.sec_unlocked: dict = {}
        self.sec_fail_count: dict = {}
        self.routine_started: dict = {}
        self.pending_seed: Optional[bytes] = None
        self.pending_level: Optional[int] = None
        self.did_store: dict = {
            61840: bytearray([0xAA] * 17),
            61836: bytearray([0xAA] * 8),
            61831: bytearray([0xAA] * 11),
            61833: bytearray([0xAA] * 4),
            61847: bytearray([0xAA] * 5),
            8193: bytearray([0xAA] * 1),
            8194: bytearray([0xAA] * 2),
            8195: bytearray([0xAA] * 2),
            8196: bytearray([0xAA] * 2),
            8197: bytearray([0xAA] * 2),
            8198: bytearray([0xAA] * 2),
            8199: bytearray([0xAA] * 2),
            8208: bytearray([0xAA] * 1),
            8449: bytearray([0xAA] * 1),
            8450: bytearray([0xAA] * 2),
            8451: bytearray([0xAA] * 2),
            8452: bytearray([0xAA] * 2),
            8705: bytearray([0xAA] * 2),
            8706: bytearray([0xAA] * 4),
            8707: bytearray([0xAA] * 4),
            8721: bytearray([0xAA] * 1),
            8722: bytearray([0xAA] * 1),
            8961: bytearray([0xAA] * 2),
            8962: bytearray([0xAA] * 1),
            8963: bytearray([0xAA] * 4),
            8964: bytearray([0xAA] * 2),
            8965: bytearray([0xAA] * 1),
            8966: bytearray([0xAA] * 3),
            9217: bytearray([0xAA] * 2),
            9218: bytearray([0xAA] * 2),
            9219: bytearray([0xAA] * 2),
            9220: bytearray([0xAA] * 2),
            9473: bytearray([0xAA] * 4),
            9474: bytearray([0xAA] * 4),
            9475: bytearray([0xAA] * 1),
        }
        self.dtcs: list = [
            {"code": 12583168, "status": 0x00},
            {"code": 12583424, "status": 0x00},
            {"code": 12583680, "status": 0x00},
            {"code": 12587008, "status": 0x00},
            {"code": 12587264, "status": 0x00},
            {"code": 11534592, "status": 0x00},
            {"code": 11534848, "status": 0x00},
            {"code": 11535104, "status": 0x00},
            {"code": 11535360, "status": 0x00},
            {"code": 11535616, "status": 0x00},
            {"code": 11535872, "status": 0x00},
            {"code": 11538432, "status": 0x00},
            {"code": 12648192, "status": 0x00},
            {"code": 12648208, "status": 0x00},
            {"code": 12648224, "status": 0x00},
            {"code": 12648240, "status": 0x00},
            {"code": 12648256, "status": 0x00},
            {"code": 12647936, "status": 0x00},
            {"code": 12647952, "status": 0x00},
        ]

    @property
    def _dids_meta(self) -> dict:
        return {
            61840: {
                "data_length":  17,
                "access_read":  True,
                "access_write": False,
                "min_session":  1,
                "read_sec":     0,
                "write_sec":    1,
            },
            61836: {
                "data_length":  8,
                "access_read":  True,
                "access_write": False,
                "min_session":  1,
                "read_sec":     0,
                "write_sec":    1,
            },
            61831: {
                "data_length":  11,
                "access_read":  True,
                "access_write": True,
                "min_session":  3,
                "read_sec":     0,
                "write_sec":    1,
            },
            61833: {
                "data_length":  4,
                "access_read":  True,
                "access_write": False,
                "min_session":  1,
                "read_sec":     0,
                "write_sec":    0,
            },
            61847: {
                "data_length":  5,
                "access_read":  True,
                "access_write": False,
                "min_session":  1,
                "read_sec":     0,
                "write_sec":    0,
            },
            8193: {
                "data_length":  1,
                "access_read":  True,
                "access_write": False,
                "min_session":  1,
                "read_sec":     0,
                "write_sec":    0,
            },
            8194: {
                "data_length":  2,
                "access_read":  True,
                "access_write": False,
                "min_session":  1,
                "read_sec":     0,
                "write_sec":    0,
            },
            8195: {
                "data_length":  2,
                "access_read":  True,
                "access_write": False,
                "min_session":  1,
                "read_sec":     0,
                "write_sec":    0,
            },
            8196: {
                "data_length":  2,
                "access_read":  True,
                "access_write": False,
                "min_session":  1,
                "read_sec":     0,
                "write_sec":    0,
            },
            8197: {
                "data_length":  2,
                "access_read":  True,
                "access_write": False,
                "min_session":  1,
                "read_sec":     0,
                "write_sec":    0,
            },
            8198: {
                "data_length":  2,
                "access_read":  True,
                "access_write": False,
                "min_session":  1,
                "read_sec":     0,
                "write_sec":    0,
            },
            8199: {
                "data_length":  2,
                "access_read":  True,
                "access_write": False,
                "min_session":  1,
                "read_sec":     0,
                "write_sec":    0,
            },
            8208: {
                "data_length":  1,
                "access_read":  True,
                "access_write": True,
                "min_session":  3,
                "read_sec":     0,
                "write_sec":    1,
            },
            8449: {
                "data_length":  1,
                "access_read":  True,
                "access_write": False,
                "min_session":  1,
                "read_sec":     0,
                "write_sec":    0,
            },
            8450: {
                "data_length":  2,
                "access_read":  True,
                "access_write": False,
                "min_session":  1,
                "read_sec":     0,
                "write_sec":    0,
            },
            8451: {
                "data_length":  2,
                "access_read":  True,
                "access_write": False,
                "min_session":  1,
                "read_sec":     0,
                "write_sec":    0,
            },
            8452: {
                "data_length":  2,
                "access_read":  True,
                "access_write": False,
                "min_session":  1,
                "read_sec":     0,
                "write_sec":    0,
            },
            8705: {
                "data_length":  2,
                "access_read":  True,
                "access_write": False,
                "min_session":  1,
                "read_sec":     0,
                "write_sec":    0,
            },
            8706: {
                "data_length":  4,
                "access_read":  True,
                "access_write": False,
                "min_session":  1,
                "read_sec":     0,
                "write_sec":    0,
            },
            8707: {
                "data_length":  4,
                "access_read":  True,
                "access_write": False,
                "min_session":  1,
                "read_sec":     0,
                "write_sec":    0,
            },
            8721: {
                "data_length":  1,
                "access_read":  True,
                "access_write": False,
                "min_session":  1,
                "read_sec":     0,
                "write_sec":    0,
            },
            8722: {
                "data_length":  1,
                "access_read":  True,
                "access_write": False,
                "min_session":  1,
                "read_sec":     0,
                "write_sec":    0,
            },
            8961: {
                "data_length":  2,
                "access_read":  True,
                "access_write": False,
                "min_session":  1,
                "read_sec":     0,
                "write_sec":    0,
            },
            8962: {
                "data_length":  1,
                "access_read":  True,
                "access_write": False,
                "min_session":  1,
                "read_sec":     0,
                "write_sec":    0,
            },
            8963: {
                "data_length":  4,
                "access_read":  True,
                "access_write": False,
                "min_session":  1,
                "read_sec":     0,
                "write_sec":    0,
            },
            8964: {
                "data_length":  2,
                "access_read":  True,
                "access_write": False,
                "min_session":  1,
                "read_sec":     0,
                "write_sec":    0,
            },
            8965: {
                "data_length":  1,
                "access_read":  True,
                "access_write": False,
                "min_session":  1,
                "read_sec":     0,
                "write_sec":    0,
            },
            8966: {
                "data_length":  3,
                "access_read":  True,
                "access_write": False,
                "min_session":  1,
                "read_sec":     0,
                "write_sec":    0,
            },
            9217: {
                "data_length":  2,
                "access_read":  True,
                "access_write": True,
                "min_session":  3,
                "read_sec":     0,
                "write_sec":    1,
            },
            9218: {
                "data_length":  2,
                "access_read":  True,
                "access_write": True,
                "min_session":  3,
                "read_sec":     0,
                "write_sec":    1,
            },
            9219: {
                "data_length":  2,
                "access_read":  True,
                "access_write": True,
                "min_session":  3,
                "read_sec":     0,
                "write_sec":    1,
            },
            9220: {
                "data_length":  2,
                "access_read":  True,
                "access_write": True,
                "min_session":  3,
                "read_sec":     0,
                "write_sec":    1,
            },
            9473: {
                "data_length":  4,
                "access_read":  True,
                "access_write": False,
                "min_session":  1,
                "read_sec":     0,
                "write_sec":    0,
            },
            9474: {
                "data_length":  4,
                "access_read":  True,
                "access_write": False,
                "min_session":  1,
                "read_sec":     0,
                "write_sec":    0,
            },
            9475: {
                "data_length":  1,
                "access_read":  True,
                "access_write": False,
                "min_session":  1,
                "read_sec":     0,
                "write_sec":    0,
            },
        }

    @property
    def _routines_meta(self) -> dict:
        return {
            65280: {
                "min_session": 3,
                "sec_level":   0,
                "support": {"start","results",                },
            },
            65281: {
                "min_session": 3,
                "sec_level":   1,
                "support": {"start",                },
            },
            65296: {
                "min_session": 2,
                "sec_level":   1,
                "support": {"start","results",                },
            },
            65297: {
                "min_session": 2,
                "sec_level":   1,
                "support": {"start","results",                },
            },
            65312: {
                "min_session": 3,
                "sec_level":   1,
                "support": {"start","stop","results",                },
            },
            65313: {
                "min_session": 3,
                "sec_level":   0,
                "support": {"start","results",                },
            },
        }


def _inline_handle(pdu: bytes, state: _InlineEcuState, verbose: bool) -> Optional[bytes]:
    """Inline ECU protocol simulator — mirrors docker/ecu_sim/sim.py."""
    if not pdu: return bytes([0x7F, 0x00, 0x13])
    sid = pdu[0]
    def nrc(s, n): return bytes([0x7F, s, n])

    if sid == 0x10:
        if len(pdu) < 2: return nrc(0x10, 0x13)
        sub = pdu[1] & 0x7F
        if sub not in (0x01, 0x02, 0x03): return nrc(0x10, 0x12)
        state.session = sub; state.sec_unlocked.clear(); state.sec_fail_count.clear()
        state.pending_seed = state.pending_level = None
        if pdu[1] & 0x80: return None
        p2_hi=0; p2_lo=25; p2s_hi=19; p2s_lo=136
        return bytes([0x50, sub, p2_hi, p2_lo, p2s_hi, p2s_lo])
    if sid == 0x3E:
        if len(pdu) < 2: return nrc(0x3E, 0x13)
        if (pdu[1] & 0x7F) != 0x00: return nrc(0x3E, 0x12)  # subFunctionNotSupported
        if pdu[1] & 0x80: return None
        return bytes([0x7E, 0x00])
    if sid == 0x11:
        if len(pdu) < 2: return nrc(0x11, 0x13)
        sub = pdu[1] & 0x7F
        if sub not in (0x01, 0x02, 0x03): return nrc(0x11, 0x12)
        state.session = 0x01; state.sec_unlocked.clear(); state.sec_fail_count.clear()
        return bytes([0x51, sub])
    if sid == 0x27:
        if len(pdu) < 2: return nrc(0x27, 0x13)
        if state.session == 0x01: return nrc(0x27, 0x7F)
        sub = pdu[1]
        if sub % 2 == 1:
            level = (sub + 1) // 2
            if state.sec_fail_count.get(level, 0) >= 3: return nrc(0x27, 0x36)
            if state.sec_unlocked.get(level): return bytes([0x67, sub] + [0x00] * ALGO_SEED_LEN)
            import os as _os
            seed = _os.urandom(6) + bytes([0x00, level & 0xFF])
            state.pending_seed = seed; state.pending_level = level
            return bytes([0x67, sub]) + seed
        else:
            level = sub // 2
            if state.sec_fail_count.get(level, 0) >= 3: return nrc(0x27, 0x36)
            if state.pending_seed is None or state.pending_level != level: return nrc(0x27, 0x24)
            if len(pdu) != 2 + ALGO_KEY_LEN: return nrc(0x27, 0x13)
            key_material = _LEVEL_KEYS.get(level, _LEVEL_KEYS[1])
            expected = (aes_cmac(key_material, state.pending_seed)[:ALGO_KEY_LEN]
                        if _XALOQI_AVAILABLE else
                        _EcuSimulator._cmac(key_material, state.pending_seed)[:ALGO_KEY_LEN])
            state.pending_seed = state.pending_level = None
            if bytes(pdu[2:]) != expected:
                state.sec_fail_count[level] = state.sec_fail_count.get(level, 0) + 1
                return nrc(0x27, 0x35)
            state.sec_fail_count[level] = 0
            state.sec_unlocked[level] = True
            return bytes([0x67, sub])
    if sid == 0x22:
        if len(pdu) < 3: return nrc(0x22, 0x13)
        if (len(pdu) - 1) % 2 != 0: return nrc(0x22, 0x13)
        resp = bytearray([0x62]); meta = state._dids_meta
        for i in range(1, len(pdu), 2):
            did_id = (pdu[i] << 8) | pdu[i + 1]; entry = meta.get(did_id)
            if entry is None: return nrc(0x22, 0x31)
            if _SESSION_ORDINALS.get(state.session, 1) < entry['min_session']: return nrc(0x22, 0x7F)
            if entry['read_sec'] > 0 and not state.sec_unlocked.get(entry['read_sec']): return nrc(0x22, 0x33)
            resp.extend([pdu[i], pdu[i+1]])
            resp.extend(state.did_store.get(did_id, bytearray(entry['data_length'])))
        return bytes(resp)
    if sid == 0x2E:
        if len(pdu) < 4: return nrc(0x2E, 0x13)
        did_id = (pdu[1] << 8) | pdu[2]; meta = state._dids_meta; entry = meta.get(did_id)
        if entry is None or not entry['access_write']: return nrc(0x2E, 0x31)
        if _SESSION_ORDINALS.get(state.session, 1) < entry['min_session']: return nrc(0x2E, 0x7F)
        if entry['write_sec'] > 0 and not state.sec_unlocked.get(entry['write_sec']): return nrc(0x2E, 0x33)
        data = pdu[3:]
        if len(data) != entry['data_length']: return nrc(0x2E, 0x13)
        state.did_store[did_id] = bytearray(data); return bytes([0x6E, pdu[1], pdu[2]])
    if sid == 0x19:
        if len(pdu) < 2: return nrc(0x19, 0x13)
        sub = pdu[1]
        if sub == 0x01:
            if len(pdu) < 3: return nrc(0x19, 0x13)
            mask = pdu[2]
            n = sum(1 for d in state.dtcs if d['status'] & mask)
            return bytes([0x59, 0x01, mask, 0x01, (n >> 8) & 0xFF, n & 0xFF])
        if sub == 0x02:
            if len(pdu) < 3: return nrc(0x19, 0x13)
            mask = pdu[2]
            resp = bytearray([0x59, 0x02, mask])
            for d in state.dtcs:
                if d['status'] & mask:
                    c = d['code']
                    resp.extend([(c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, d['status']])
            return bytes(resp)
        return nrc(0x19, 0x12)
    if sid == 0x14:
        if state.session == 0x01: return nrc(0x14, 0x7F)
        for d in state.dtcs: d['status'] = 0x00
        return bytes([0x54])
    if sid == 0x31:
        if len(pdu) < 4: return nrc(0x31, 0x13)
        sub_fn = pdu[1]
        if sub_fn < 0x01 or sub_fn > 0x03: return nrc(0x31, 0x12)
        rid = (pdu[2] << 8) | pdu[3]; meta = state._routines_meta; entry = meta.get(rid)
        if entry is None: return nrc(0x31, 0x31)
        if _SESSION_ORDINALS.get(state.session, 1) < entry['min_session']: return nrc(0x31, 0x7F)
        if entry['sec_level'] > 0 and not state.sec_unlocked.get(entry['sec_level']): return nrc(0x31, 0x33)
        if sub_fn == 0x01 and 'start'   not in entry['support']: return nrc(0x31, 0x12)
        if sub_fn == 0x02 and 'stop'    not in entry['support']: return nrc(0x31, 0x12)
        if sub_fn == 0x03 and 'results' not in entry['support']: return nrc(0x31, 0x12)
        if sub_fn == 0x03 and not state.routine_started.get(rid, False): return nrc(0x31, 0x22)
        if sub_fn == 0x01: state.routine_started[rid] = True
        return bytes([0x71, sub_fn, pdu[2], pdu[3]])
    if sid == 0x28:
        if len(pdu) < 2: return nrc(0x28, 0x13)
        if state.session == 0x01: return nrc(0x28, 0x7F)
        sub = pdu[1] & 0x7F
        if sub > 0x03: return nrc(0x28, 0x12)
        return bytes([0x68, sub])
    if sid == 0x85:
        if len(pdu) < 2: return nrc(0x85, 0x13)
        if state.session == 0x01: return nrc(0x85, 0x7F)
        sub = pdu[1] & 0x7F
        if sub not in (0x01, 0x02): return nrc(0x85, 0x12)
        return bytes([0xC5, sub])
    return bytes([0x7F, sid, 0x11])


# ---------------------------------------------------------------------------
# pytest CLI options and fixtures
# ---------------------------------------------------------------------------

def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption(
        '--can-interface', default=os.environ.get('CAN_INTERFACE', 'simulator'),
        help='CAN interface: simulator|virtual|socketcan|pcan|kvaser (default: simulator)')
    parser.addoption(
        '--can-channel', default=os.environ.get('CAN_CHANNEL', 'vcan0'),
        help='CAN channel name (default: vcan0)')
    parser.addoption(
        '--can-bitrate', type=int, default=int(os.environ.get('CAN_BITRATE', '500000')),
        help='CAN bitrate in bps (default: 500000)')
    parser.addoption('--hil', action='store_true', default=False,
        help='Enable hardware-in-loop tests (requires real ECU)')
    parser.addoption('--sec-key-l1', default=None,
        help='Hex AES-128 key for security level 1')
    parser.addoption('--sec-key-l2', default=None,
        help='Hex AES-128 key for security level 2')

@pytest.fixture(scope='session')
def can_interface(request: pytest.FixtureRequest) -> str:
    return request.config.getoption('--can-interface')

@pytest.fixture(scope='session')
def is_hil(request: pytest.FixtureRequest) -> bool:
    return bool(request.config.getoption('--hil'))

@pytest.fixture(scope='session')
def aes_keys(request: pytest.FixtureRequest) -> dict:
    keys = dict(_LEVEL_KEYS)
    k1 = request.config.getoption('--sec-key-l1')
    k2 = request.config.getoption('--sec-key-l2')
    if k1: keys[1] = bytes.fromhex(k1.replace('0x','').replace(' ',''))
    if k2: keys[2] = bytes.fromhex(k2.replace('0x','').replace(' ',''))
    return keys


@pytest.fixture(scope='function')
def uds_bus(
    can_interface: str,
    request: pytest.FixtureRequest,
    aes_keys: dict,
) -> IsoTpTransport:
    """
    Per-test IsoTpTransport fixture backed by xaloqi-tester.
    Provides the same transport.request(pdu) -> bytes API as the previous
    conftest. All test files (test_did_*.py etc.) work unchanged.
    """
    if not _XALOQI_AVAILABLE:
        pytest.skip(
            'xaloqi-tester not installed. '
            'Run: pip install -r requirements_testgen.txt'
        )
    channel = request.config.getoption('--can-channel')
    bitrate = request.config.getoption('--can-bitrate')
    loop    = asyncio.new_event_loop()
    os.environ.setdefault('XALOQI_LICENSE_SKIP', '1')

    if can_interface == 'simulator':
        tester_bus, ecu_bus = VirtualBus.pair('testgen_sim')
        from xaloqi.tester._isotp import IsoTpEngine
        state = _InlineEcuState()
        stop_event = asyncio.Event()
        async def _ecu_loop():
            eng = IsoTpEngine()
            while not stop_event.is_set():
                result = await ecu_bus.recv(timeout=0.05)
                if result is None: continue
                _, frame = result
                pci = (frame[0] >> 4) & 0x0F
                if pci == 0x0:
                    pdu = bytes(frame[1:1 + (frame[0] & 0x0F)])
                elif pci == 0x1:
                    expected_len = ((frame[0] & 0x0F) << 8) | frame[1]
                    chunks = [bytes(frame[2:])]
                    await ecu_bus.send(ECU_TX_ID, eng.flow_control_cts())
                    while sum(len(c) for c in chunks) < expected_len:
                        res = await ecu_bus.recv(timeout=0.5)
                        if res is None: break
                        _, cf = res
                        if (cf[0] >> 4) == 0x2: chunks.append(bytes(cf[1:]))
                    pdu = b''.join(chunks)[:expected_len]
                else: continue
                resp = _inline_handle(pdu, state, False)
                if resp is None: continue
                frames = eng.encode(resp)
                if len(frames) == 1:
                    await ecu_bus.send(ECU_TX_ID, frames[0])
                else:
                    await ecu_bus.send(ECU_TX_ID, frames[0])
                    await ecu_bus.recv(timeout=0.5)
                    for cf in frames[1:]: await ecu_bus.send(ECU_TX_ID, cf)
        sim_task = loop.create_task(_ecu_loop())
        tester = UdsTester(tester_bus, rx_id=ECU_TX_ID, tx_id=TESTER_TX_ID,
                           timeout=RESPONSE_TIMEOUT_S, keepalive=False)
        loop.run_until_complete(tester.__aenter__())
        transport = IsoTpTransport(tester, loop)
        yield transport
        try:
            loop.run_until_complete(tester.raw_request(
                bytes([SID_DIAGNOSTIC_SESSION_CONTROL, SESSION_DEFAULT]), timeout=0.1))
        except Exception: pass
        stop_event.set()
        loop.run_until_complete(asyncio.sleep(0.05))
        sim_task.cancel()
        try: loop.run_until_complete(sim_task)
        except asyncio.CancelledError: pass
        transport.close()
        return

    # ── Hardware / virtual modes ──────────────────────────────────────────
    try:
        if can_interface == 'virtual':
            bus, _ = VirtualBus.pair('testgen')
            rx_id, tx_id = ECU_TX_ID, TESTER_TX_ID
        elif can_interface == 'socketcan':
            from xaloqi.tester.transport.socketcan import SocketCanBus
            bus = SocketCanBus(channel); rx_id, tx_id = ECU_TX_ID, TESTER_TX_ID
        elif can_interface == 'pcan':
            from xaloqi.tester.transport.hardware import PcanBus
            bus = PcanBus(channel, bitrate=bitrate); rx_id, tx_id = ECU_TX_ID, TESTER_TX_ID
        elif can_interface == 'kvaser':
            from xaloqi.tester.transport.hardware import KvaserBus
            ch = int(channel) if channel.isdigit() else 0
            bus = KvaserBus(ch, bitrate=bitrate); rx_id, tx_id = ECU_TX_ID, TESTER_TX_ID
        else:
            loop.close(); pytest.skip(f'Unknown CAN interface: {can_interface!r}'); return
    except (ImportError, Exception) as exc:
        loop.close(); pytest.skip(str(exc)); return
    try:
        tester = UdsTester(bus, rx_id=rx_id, tx_id=tx_id,
                           timeout=RESPONSE_TIMEOUT_S, keepalive=False)
        loop.run_until_complete(tester.__aenter__())
    except Exception as exc:
        loop.close(); pytest.skip(f'Cannot open CAN bus ({can_interface}/{channel}): {exc}'); return
    transport = IsoTpTransport(tester, loop)
    yield transport
    try:
        loop.run_until_complete(tester.raw_request(
            bytes([SID_DIAGNOSTIC_SESSION_CONTROL, SESSION_DEFAULT]), timeout=0.1))
    except Exception: pass
    transport.close()
