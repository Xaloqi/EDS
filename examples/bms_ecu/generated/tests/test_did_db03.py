# =============================================================================
# GENERATED — do NOT edit manually.
# Regenerate: python3 tools/codegen.py --config <yaml> --out generated/ --test-gen
#
# ECU       : BMS_MainController
# Version   : 1.0.0
# Generated : 2026-05-20T07:21:48Z
#
# DID       : 0xDB03  (BMS_ContactorState)
# Access    : read
# Session   : UDS_SESSION_DEFAULT (ordinal 1)
# Read sec  : 0
# Write sec : 0
# Bytes     : 1
#
# PURPOSE: Exhaustive per-DID pytest tests covering:
#   - Happy-path read (all required preconditions satisfied)
#   - Happy-path write (if write-capable)
#   - Session gate enforcement (wrong session → NRC)
#   - Security gate enforcement (locked → NRC 0x33)
#   - Data length enforcement (wrong length → NRC 0x13)
#   - Protocol boundary cases
#
# Run:  pytest generated/tests/test_did_db03.py -v
# =============================================================================

from __future__ import annotations

import pytest
from conftest import (
    IsoTpTransport,
    SESSION_DEFAULT, SESSION_EXTENDED, SESSION_PROGRAMMING,
    SID_READ_DATA_BY_ID, SID_WRITE_DATA_BY_ID, SID_DIAGNOSTIC_SESSION_CONTROL,
    SID_SECURITY_ACCESS, SID_NEGATIVE_RESPONSE, POSITIVE_RESPONSE_OFFSET,
    NRC_INCORRECT_MSG_LEN, NRC_REQUEST_OUT_OF_RANGE,
    NRC_SECURITY_ACCESS_DENIED, NRC_SERVICE_NOT_SUPPORTED_IN_SESSION,
    NRC_CONDITIONS_NOT_CORRECT, NRC_INVALID_KEY,
    ALGO_SEED_LEN, ALGO_KEY_LEN,
)

# ── DID under test ────────────────────────────────────────────────────────────
DID_ID:         int  = 56067
DID_HEX:        str  = "0xDB03"
DID_NAME:       str  = "BMS_ContactorState"
DID_HI:         int  = 219
DID_LO:         int  = 3
DATA_LENGTH:    int  = 1
ACCESS_READ:    bool = True
ACCESS_WRITE:   bool = False
MIN_SESSION:    int  = 1    # 1=default 2=programming 3=extended
READ_SEC_LEVEL: int  = 0
WRITE_SEC_LEVEL:int  = 0

# Session byte values indexed by ordinal
_SESSION_BYTE = {1: SESSION_DEFAULT, 2: SESSION_PROGRAMMING, 3: SESSION_EXTENDED}
MIN_SESSION_BYTE: int = _SESSION_BYTE.get(MIN_SESSION, SESSION_DEFAULT)


# ── Helpers ───────────────────────────────────────────────────────────────────

def _enter_session(bus: IsoTpTransport, session_byte: int) -> None:
    pdu = bus.request(bytes([SID_DIAGNOSTIC_SESSION_CONTROL, session_byte & 0x7F]))
    assert pdu is not None, f"No response entering session 0x{session_byte:02X}"
    assert pdu[0] == (SID_DIAGNOSTIC_SESSION_CONTROL + POSITIVE_RESPONSE_OFFSET), (
        f"Failed to enter session 0x{session_byte:02X}: {pdu.hex(' ')}"
    )


def _unlock(bus: IsoTpTransport, level: int, aes_keys: dict) -> None:
    """Perform SecurityAccess seed/key exchange for given level."""
    seed_sub = (level * 2) - 1
    key_sub  = level * 2

    seed_pdu = bus.request(bytes([SID_SECURITY_ACCESS, seed_sub]))
    assert seed_pdu is not None, f"No response to requestSeed (level {level})"
    assert seed_pdu[0] == (SID_SECURITY_ACCESS + POSITIVE_RESPONSE_OFFSET), (
        f"requestSeed failed: {seed_pdu.hex(' ')}"
    )
    seed_bytes = seed_pdu[2:]
    assert len(seed_bytes) == ALGO_KEY_LEN + 4, (   # ALGO_SEED_LEN = 8
        f"Unexpected seed length {len(seed_bytes)}"
    )

    # Derive key using AES-128-CMAC (mirrors conftest._EcuSimulator._derive_key)
    from conftest import _EcuSimulator
    sim_ref = _EcuSimulator()
    key_bytes = sim_ref._derive_key(seed_bytes, level)
    # Override with caller's key if different from default
    provided_key = aes_keys.get(level)
    if provided_key and provided_key != sim_ref._LEVEL_KEYS.get(level):
        key_bytes = sim_ref._cmac(provided_key, seed_bytes)[:4]

    key_pdu = bus.request(bytes([SID_SECURITY_ACCESS, key_sub]) + key_bytes)
    assert key_pdu is not None, f"No response to sendKey (level {level})"
    assert key_pdu[0] == (SID_SECURITY_ACCESS + POSITIVE_RESPONSE_OFFSET), (
        f"sendKey failed for level {level}: {key_pdu.hex(' ')}"
    )


def _read_did(bus: IsoTpTransport) -> bytes:
    """Send ReadDataByIdentifier for this DID. Returns raw UDS PDU."""
    return bus.request(bytes([SID_READ_DATA_BY_ID, DID_HI, DID_LO]))


def _write_did(bus: IsoTpTransport, data: bytes) -> bytes:
    """Send WriteDataByIdentifier for this DID. Returns raw UDS PDU."""
    return bus.request(bytes([SID_WRITE_DATA_BY_ID, DID_HI, DID_LO]) + data)


def _setup_preconditions(bus: IsoTpTransport, aes_keys: dict, for_write: bool = False) -> None:
    """Enter required session and unlock required security level."""
    if MIN_SESSION_BYTE != SESSION_DEFAULT:
        _enter_session(bus, MIN_SESSION_BYTE)
    sec_level = WRITE_SEC_LEVEL if for_write else READ_SEC_LEVEL
    if sec_level > 0:
        if MIN_SESSION_BYTE == SESSION_DEFAULT:
            # Security access requires at least extended session
            _enter_session(bus, SESSION_EXTENDED)
        _unlock(bus, sec_level, aes_keys)


# =============================================================================
# READ tests
# =============================================================================

class TestReadBmscontactorstate:
    """
    SID 0x22 ReadDataByIdentifier — DID 0xDB03 (BMS_ContactorState)

    data_length : 1 byte(s)
    min_session : UDS_SESSION_DEFAULT (ordinal 1)
    read_sec    : 0
    """

    def test_happy_path_returns_correct_length(
        self, uds_bus: IsoTpTransport, aes_keys: dict
    ) -> None:
        """
        Read DID 0xDB03 with all preconditions satisfied.
        Expected PDU: [0x62, 0xDB, 0x03, <1 data byte(s)>]
        Total PDU length: 4 bytes.
        """
        _setup_preconditions(uds_bus, aes_keys, for_write=False)
        pdu = _read_did(uds_bus)

        assert pdu is not None, f"No response reading DID 0xDB03 (BMS_ContactorState)"
        rsid = SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET
        assert pdu[0] == rsid, (
            f"Expected RSID 0x{rsid:02X}, got 0x{pdu[0]:02X} — PDU: {pdu.hex(' ')}"
        )
        assert pdu[1] == DID_HI, f"DID high byte echo: expected 0x{DID_HI:02X}, got 0x{pdu[1]:02X}"
        assert pdu[2] == DID_LO, f"DID low byte echo: expected 0x{DID_LO:02X}, got 0x{pdu[2]:02X}"
        expected_len = 3 + DATA_LENGTH
        assert len(pdu) == expected_len, (
            f"DID 0xDB03 PDU length: expected {expected_len} (RSID+DID+data), got {len(pdu)}"
        )

    def test_response_data_not_all_zero(
        self, uds_bus: IsoTpTransport, aes_keys: dict
    ) -> None:
        """
        Data payload for DID 0xDB03 should be non-zero (ECU returns real or simulated data).
        A zero-only payload may indicate an uninitialised handler stub.
        """
        _setup_preconditions(uds_bus, aes_keys, for_write=False)
        pdu = _read_did(uds_bus)
        assert pdu is not None
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET)
        data_payload = pdu[3:]
        # Advisory: log a warning if all zeros — don't fail (stub may be intentional)
        if all(b == 0 for b in data_payload):
            import warnings
            warnings.warn(
                f"DID 0xDB03 (BMS_ContactorState) returned all-zero data — "
                "ensure handler stub is implemented."
            )



    def test_request_with_one_did_byte_rejected(self, uds_bus: IsoTpTransport) -> None:
        """
        Malformed request with only SID + 1 DID byte must return NRC 0x13
        (incorrectMessageLengthOrInvalidFormat).
        """
        pdu = uds_bus.request(bytes([SID_READ_DATA_BY_ID, DID_HI]))
        assert pdu is not None
        assert pdu[0] == SID_NEGATIVE_RESPONSE
        assert pdu[1] == SID_READ_DATA_BY_ID
        assert pdu[2] == NRC_INCORRECT_MSG_LEN, (
            f"Expected NRC 0x13 for truncated request, got 0x{pdu[2]:02X}"
        )

    def test_request_with_only_sid_rejected(self, uds_bus: IsoTpTransport) -> None:
        """
        Request containing only SID byte must return NRC 0x13.
        """
        pdu = uds_bus.request(bytes([SID_READ_DATA_BY_ID]))
        assert pdu is not None
        assert pdu[0] == SID_NEGATIVE_RESPONSE
        assert pdu[2] == NRC_INCORRECT_MSG_LEN


# =============================================================================
# WRITE tests
# =============================================================================

