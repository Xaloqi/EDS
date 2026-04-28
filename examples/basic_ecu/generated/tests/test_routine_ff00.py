# =============================================================================
# GENERATED — do NOT edit manually.
# Regenerate: python3 tools/codegen.py --config <yaml> --out generated/ --test-gen
#
# ECU       : BasicECU
# Version   : 0.1.0
# Generated : 2026-04-21T07:31:56Z
#
# Routine   : 0xFF00  (ECU_SelfTest)
# Session   : extended (ordinal 3)
# Security  : level 0
# Support   : start results
#
# PURPOSE: Exhaustive per-routine pytest tests covering:
#   - Happy-path startRoutine (all required preconditions satisfied)
#   - requestRoutineResults (if ROUTINE_SUPPORT_RESULTS)
#   - stopRoutine not supported (if stop absent from support flags)
#   - Session gate enforcement (wrong session → NRC 0x7F)
#   - Security gate enforcement (locked → NRC 0x33)
#   - subFunction not supported (stop or results absent → NRC 0x12)
#   - Protocol boundary cases (unknown RID, bad subFn, too-short request)
#
# Run:  pytest generated/tests/test_routine_ff00.py -v
# =============================================================================

from __future__ import annotations

import pytest
from conftest import (
    IsoTpTransport,
    SESSION_DEFAULT, SESSION_EXTENDED, SESSION_PROGRAMMING,
    SID_ROUTINE_CONTROL, SID_DIAGNOSTIC_SESSION_CONTROL, SID_SECURITY_ACCESS,
    SID_NEGATIVE_RESPONSE, POSITIVE_RESPONSE_OFFSET,
    NRC_INCORRECT_MSG_LEN, NRC_SUB_FUNCTION_NOT_SUPPORTED,
    NRC_REQUEST_OUT_OF_RANGE, NRC_SECURITY_ACCESS_DENIED,
    NRC_SERVICE_NOT_SUPPORTED_IN_SESSION,
    ALGO_SEED_LEN, ALGO_KEY_LEN,
    _EcuSimulator,
)

# ── Routine under test ────────────────────────────────────────────────────────
ROUTINE_ID:       int  = 65280
ROUTINE_HEX:      str  = "0xFF00"
ROUTINE_NAME:     str  = "ECU_SelfTest"
ROUTINE_HI:       int  = 255
ROUTINE_LO:       int  = 0
MIN_SESSION:      int  = 3    # 1=default 2=programming 3=extended
SECURITY_LEVEL:   int  = 0
SUPPORT_START:    bool = True
SUPPORT_STOP:     bool = False
SUPPORT_RESULTS:  bool = True

# Session byte values indexed by ordinal (mirrors _SESSION_BYTE in conftest)
_SESSION_BYTE = {1: SESSION_DEFAULT, 2: SESSION_PROGRAMMING, 3: SESSION_EXTENDED}
MIN_SESSION_BYTE: int = _SESSION_BYTE.get(MIN_SESSION, SESSION_DEFAULT)

# ISO 14229-1 §13 RoutineControl sub-function codes
_SUBFN_START:   int = 0x01
_SUBFN_STOP:    int = 0x02
_SUBFN_RESULTS: int = 0x03

# ── Helpers ───────────────────────────────────────────────────────────────────

def _enter_session(bus: IsoTpTransport, session_byte: int) -> None:
    pdu = bus.request(bytes([SID_DIAGNOSTIC_SESSION_CONTROL, session_byte & 0x7F]))
    assert pdu is not None, f"No response entering session 0x{session_byte:02X}"
    assert pdu[0] == (SID_DIAGNOSTIC_SESSION_CONTROL + POSITIVE_RESPONSE_OFFSET), (
        f"Failed to enter session 0x{session_byte:02X}: {pdu.hex(' ')}"
    )


def _unlock(bus: IsoTpTransport, level: int, aes_keys: dict | None = None) -> None:
    """Perform SecurityAccess seed/key exchange for the given level."""
    seed_sub = (level * 2) - 1
    key_sub  = level * 2

    seed_pdu = bus.request(bytes([SID_SECURITY_ACCESS, seed_sub]))
    assert seed_pdu is not None, f"No response to requestSeed (level {level})"
    assert seed_pdu[0] == (SID_SECURITY_ACCESS + POSITIVE_RESPONSE_OFFSET), (
        f"requestSeed failed: {seed_pdu.hex(' ')}"
    )
    seed_bytes = bytes(seed_pdu[2:2 + ALGO_SEED_LEN])

    # Derive key using AES-128-CMAC (mirrors _EcuSimulator._derive_key)
    sim_ref = _EcuSimulator()
    key_bytes = sim_ref._derive_key(seed_bytes, level)
    if aes_keys:
        provided = aes_keys.get(level)
        if provided and provided != sim_ref._LEVEL_KEYS.get(level):
            key_bytes = sim_ref._cmac(provided, seed_bytes)[:4]

    key_pdu = bus.request(bytes([SID_SECURITY_ACCESS, key_sub]) + key_bytes)
    assert key_pdu is not None, f"No response to sendKey (level {level})"
    assert key_pdu[0] == (SID_SECURITY_ACCESS + POSITIVE_RESPONSE_OFFSET), (
        f"sendKey failed for level {level}: {key_pdu.hex(' ')}"
    )


def _setup(bus: IsoTpTransport, aes_keys: dict | None = None) -> None:
    """Enter MIN_SESSION_BYTE and unlock SECURITY_LEVEL (if required)."""
    if MIN_SESSION_BYTE != SESSION_DEFAULT:
        _enter_session(bus, MIN_SESSION_BYTE)
    if SECURITY_LEVEL > 0:
        if MIN_SESSION_BYTE == SESSION_DEFAULT:
            _enter_session(bus, SESSION_EXTENDED)
        _unlock(bus, SECURITY_LEVEL, aes_keys)


def _rc(bus: IsoTpTransport, sub_fn: int,
        option: bytes = b"") -> bytes | None:
    """Send a RoutineControl request and return the raw response PDU."""
    return bus.request(
        bytes([SID_ROUTINE_CONTROL, sub_fn, ROUTINE_HI, ROUTINE_LO]) + option
    )


def _assert_positive(pdu: bytes | None, sub_fn: int) -> None:
    assert pdu is not None, "No response received"
    assert len(pdu) >= 4, f"Response too short ({len(pdu)} bytes): {pdu.hex(' ')}"
    assert pdu[0] == 0x71, (
        f"Expected SID 0x71 (positive RC), got 0x{pdu[0]:02X}: {pdu.hex(' ')}"
    )
    assert pdu[1] == sub_fn, (
        f"subFn echo: expected 0x{sub_fn:02X}, got 0x{pdu[1]:02X}"
    )
    assert pdu[2] == ROUTINE_HI, (
        f"RID_hi echo: expected 0x{ROUTINE_HI:02X}, got 0x{pdu[2]:02X}"
    )
    assert pdu[3] == ROUTINE_LO, (
        f"RID_lo echo: expected 0x{ROUTINE_LO:02X}, got 0x{pdu[3]:02X}"
    )


def _assert_nrc(pdu: bytes | None, expected_nrc: int) -> None:
    assert pdu is not None, "No response received"
    assert pdu[0] == SID_NEGATIVE_RESPONSE, (
        f"Expected NRC (0x7F), got 0x{pdu[0]:02X}: {pdu.hex(' ')}"
    )
    assert pdu[1] == SID_ROUTINE_CONTROL, (
        f"NRC service byte: expected 0x31, got 0x{pdu[1]:02X}"
    )
    assert pdu[2] == expected_nrc, (
        f"Expected NRC 0x{expected_nrc:02X}, got 0x{pdu[2]:02X}: {pdu.hex(' ')}"
    )


# =============================================================================
# §1  startRoutine — happy path
# =============================================================================

class TestStartEcuselftest:
    """
    SID 0x31 sub-function 0x01 — startRoutine 0xFF00 (ECU_SelfTest)

    min_session : extended (ordinal 3)
    security    : level 0
    """

    def test_happy_path_start(
        self, uds_bus: IsoTpTransport, aes_keys: dict
    ) -> None:
        """
        startRoutine 0xFF00 with all preconditions satisfied.
        Expected: [0x71, 0x01, 0xFF, 0x00]
        ISO 14229-1 §13.3: response = [0x71, subFn, RID_hi, RID_lo {, statusRecord}]
        """
        _setup(uds_bus, aes_keys)
        pdu = _rc(uds_bus, _SUBFN_START)
        _assert_positive(pdu, _SUBFN_START)

# =============================================================================
# §2  requestRoutineResults
# =============================================================================

class TestResultsEcuselftest:
    """
    SID 0x31 sub-function 0x03 — requestRoutineResults 0xFF00

    ECU_SelfTest has ROUTINE_SUPPORT_RESULTS.
    """

    def test_request_results(
        self, uds_bus: IsoTpTransport, aes_keys: dict
    ) -> None:
        """
        requestRoutineResults 0xFF00 after start.
        Expected: [0x71, 0x03, 0xFF, 0x00]
        """
        _setup(uds_bus, aes_keys)
        pdu = _rc(uds_bus, _SUBFN_RESULTS)
        _assert_positive(pdu, _SUBFN_RESULTS)

# =============================================================================
# §3  stopRoutine not supported
# =============================================================================

class TestStopNotSupportedEcuselftest:
    """
    SID 0x31 sub-function 0x02 — stopRoutine 0xFF00

    ECU_SelfTest does NOT have ROUTINE_SUPPORT_STOP.
    Expected: NRC 0x12 subFunctionNotSupported.
    """

    def test_stop_not_supported(
        self, uds_bus: IsoTpTransport, aes_keys: dict
    ) -> None:
        """
        stopRoutine 0xFF00 → NRC 0x12.
        stop_cb is NULL in routine_database — service_0x31.c returns NRC 0x12.
        """
        _setup(uds_bus, aes_keys)
        pdu = _rc(uds_bus, _SUBFN_STOP)
        _assert_nrc(pdu, NRC_SUB_FUNCTION_NOT_SUPPORTED)

# =============================================================================
# §4  Session gate
# =============================================================================

class TestSessionGateEcuselftest:
    """
    0xFF00 (ECU_SelfTest) requires extended session (ordinal 3).
    Default session ordinal (1) < required — must return NRC 0x7F.
    """

    def test_rejected_in_default_session(self, uds_bus: IsoTpTransport) -> None:
        """
        startRoutine 0xFF00 in Default Session → NRC 0x7F.
        s_validate_routine_access() → uds_safety_validate_session() → NRC 0x7F.
        """
        uds_bus.request(bytes([SID_DIAGNOSTIC_SESSION_CONTROL, SESSION_DEFAULT]),
                        timeout=0.1)
        pdu = _rc(uds_bus, _SUBFN_START)
        _assert_nrc(pdu, NRC_SERVICE_NOT_SUPPORTED_IN_SESSION)

# =============================================================================
# §6  Protocol boundary cases
# =============================================================================

class TestProtocolEcuselftest:
    """
    Protocol boundary cases for 0xFF00 (ECU_SelfTest).
    These tests do not depend on session or security state.
    """

    def test_bad_subfn_nrc_12(self, uds_bus: IsoTpTransport) -> None:
        """
        subFn 0x04 (undefined in ISO 14229-1 §13) → NRC 0x12.
        service_0x31.c validates sub_fn before any database lookup.
        """
        if MIN_SESSION_BYTE != SESSION_DEFAULT:
            _enter_session(uds_bus, MIN_SESSION_BYTE)
        pdu = uds_bus.request(
            bytes([SID_ROUTINE_CONTROL, 0x04, ROUTINE_HI, ROUTINE_LO])
        )
        _assert_nrc(pdu, NRC_SUB_FUNCTION_NOT_SUPPORTED)

    def test_too_short_request_nrc_13(self, uds_bus: IsoTpTransport) -> None:
        """
        3-byte request (SID + subFn + RID_hi only, missing RID_lo) → NRC 0x13.
        service_0x31.c: uds_service_validate_length(req, 4) → NRC 0x13.
        """
        pdu = uds_bus.request(
            bytes([SID_ROUTINE_CONTROL, _SUBFN_START, ROUTINE_HI])
        )
        _assert_nrc(pdu, NRC_INCORRECT_MSG_LEN)

    def test_unknown_rid_nrc_31(self, uds_bus: IsoTpTransport) -> None:
        """
        startRoutine with RID 0xDEAD (not registered) → NRC 0x31.
        routine_database_find(0xDEAD) returns NULL → NRC 0x31.
        """
        if MIN_SESSION_BYTE != SESSION_DEFAULT:
            _enter_session(uds_bus, MIN_SESSION_BYTE)
        pdu = uds_bus.request(
            bytes([SID_ROUTINE_CONTROL, _SUBFN_START, 0xDE, 0xAD])
        )
        _assert_nrc(pdu, NRC_REQUEST_OUT_OF_RANGE)
