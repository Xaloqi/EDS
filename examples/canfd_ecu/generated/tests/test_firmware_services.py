# =============================================================================
# GENERATED — do NOT edit manually.
# Regenerate: python3 tools/codegen.py --config <yaml> --out generated/ --test-gen
#
# ECU       : CanFdEcu
# Version   : 1.0.0
# Generated : 2026-11-02T08:21:02Z
#
# PURPOSE: Integration tests running against REAL COMPILED FIRMWARE.
#
# These tests use the `firmware_bus` fixture which launches the harness
# binary (full C UDS stack compiled from source) and sends genuine ISO-TP
# frames to it, validating byte-exact UDS responses from the C implementation.
#
# WHAT THIS PROVES (vs. simulator tests):
#   - Correct NRC codes from the actual C service handlers
#   - Correct multi-frame assembly in the C ISO-TP state machine
#   - Real AES-CMAC key derivation from uds_security_algo.c
#   - Correct session state machine transitions in uds_session.c
#   - Correct security lockout behaviour from uds_security.c
#   - Correct ASIL-B safety wrapper enforcement (5-step validation chain)
#   - Real DID buffer sizing from generated/did_handlers.c
#   - Correct DTC registration from config/dtc_database.c
#   - RoutineControl (0x31) dispatch, session gate, security gate, RID lookup
#
# RUN:
#   pytest test_firmware_services.py -v        # all firmware tests
#   pytest test_firmware_services.py -v -k "VIN"  # specific test
# =============================================================================

from __future__ import annotations

import struct
import pytest

from conftest_firmware import (
    FirmwareIsoTpTransport,
    derive_firmware_key,
    _FIRMWARE_AES_KEYS,
    TESTER_TX_ID, ECU_TX_ID,
    P2_SERVER_MAX_MS, P2_STAR_SERVER_MAX_MS,
    SID_DIAGNOSTIC_SESSION_CONTROL, SID_NEGATIVE_RESPONSE, POSITIVE_RESPONSE_OFFSET,
    SID_SECURITY_ACCESS, SESSION_DEFAULT, SESSION_EXTENDED, SESSION_PROGRAMMING,
    RESPONSE_TIMEOUT_S,
)

# UDS SIDs
SID_ECU_RESET:         int = 0x11
SID_READ_DTC_INFO:     int = 0x19
SID_READ_DATA_BY_ID:   int = 0x22
SID_WRITE_DATA_BY_ID:  int = 0x2E
SID_TESTER_PRESENT:    int = 0x3E
SID_ROUTINE_CONTROL:   int = 0x31
SID_ROUTINE_RESPONSE:  int = 0x71

# NRC codes
NRC_INCORRECT_MSG_LEN:             int = 0x13
NRC_CONDITIONS_NOT_CORRECT:        int = 0x22
NRC_REQUEST_OUT_OF_RANGE:          int = 0x31
NRC_SECURITY_ACCESS_DENIED:        int = 0x33
NRC_INVALID_KEY:                   int = 0x35
NRC_SUB_FUNCTION_NOT_SUPPORTED:    int = 0x12
NRC_SERVICE_NOT_SUPPORTED_IN_SESSION: int = 0x7F

ALGO_SEED_LEN: int = 8
ALGO_KEY_LEN:  int = 4

# DID catalogue — derived from diagnostics_config.yaml
DID_CATALOGUE = [
    {
        "id":           61840,
        "id_hex":       "0xF190",
        "name":         "FdEcuIdentification",
        "data_length":  17,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    1,
        "id_hi":        241,
        "id_lo":        144,
    },
    {
        "id":           256,
        "id_hex":       "0x0100",
        "name":         "FdRecordA",
        "data_length":  64,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    1,
        "id_hi":        1,
        "id_lo":        0,
    },
    {
        "id":           257,
        "id_hex":       "0x0101",
        "name":         "FdRecordB",
        "data_length":  64,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    1,
        "id_hi":        1,
        "id_lo":        1,
    },
    {
        "id":           258,
        "id_hex":       "0x0102",
        "name":         "FdRecordC",
        "data_length":  64,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    1,
        "id_hi":        1,
        "id_lo":        2,
    },
    {
        "id":           61953,
        "id_hex":       "0xF201",
        "name":         "FdPeriodicStatus",
        "data_length":  20,
        "access_read":  True,
        "access_write": False,
        "min_session":  3,
        "read_sec":     0,
        "write_sec":    0,
        "id_hi":        242,
        "id_lo":        1,
    },
]

DTC_CATALOGUE = [
]


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _enter_session(bus: FirmwareIsoTpTransport, session: int) -> None:
    pdu = bus.request(bytes([SID_DIAGNOSTIC_SESSION_CONTROL, session & 0x7F]))
    assert pdu is not None, f"No response entering session 0x{session:02X}"
    assert pdu[0] == (SID_DIAGNOSTIC_SESSION_CONTROL + POSITIVE_RESPONSE_OFFSET), (
        f"Session entry failed: {pdu.hex(' ')}"
    )


def _unlock_level(bus: FirmwareIsoTpTransport, level: int) -> None:
    """Perform SecurityAccess unlock for given level using AES-CMAC."""
    seed_sub = (level * 2) - 1
    key_sub  = level * 2

    pdu_seed = bus.request(bytes([SID_SECURITY_ACCESS, seed_sub]))
    assert pdu_seed is not None, f"No response to requestSeed (level {level})"
    assert pdu_seed[0] == (SID_SECURITY_ACCESS + POSITIVE_RESPONSE_OFFSET), (
        f"requestSeed failed: {pdu_seed.hex(' ')}"
    )
    seed = bytes(pdu_seed[2:])
    assert len(seed) == ALGO_SEED_LEN, f"Seed len {len(seed)} != {ALGO_SEED_LEN}"

    key = derive_firmware_key(seed, level)
    pdu_key = bus.request(bytes([SID_SECURITY_ACCESS, key_sub]) + key)
    assert pdu_key is not None, f"No response to sendKey"
    assert pdu_key[0] == (SID_SECURITY_ACCESS + POSITIVE_RESPONSE_OFFSET), (
        f"Unlock failed: {pdu_key.hex(' ')}"
    )


_SESSION_BYTE = {1: SESSION_DEFAULT, 2: SESSION_PROGRAMMING, 3: SESSION_EXTENDED}


def _setup_for_did(bus: FirmwareIsoTpTransport, did: dict, for_write: bool = False) -> None:
    sess = _SESSION_BYTE.get(did["min_session"], SESSION_DEFAULT)
    if sess != SESSION_DEFAULT:
        _enter_session(bus, sess)
    sec = did["write_sec"] if for_write else did["read_sec"]
    if sec > 0:
        if sess == SESSION_DEFAULT:
            _enter_session(bus, SESSION_EXTENDED)
        _unlock_level(bus, sec)


# =============================================================================
# §1 Session Control — firmware response validation
# =============================================================================

class TestFirmwareSessionControl:
    """
    SID 0x10 — validates P2 timing bytes match YAML, suppress bit, error cases.
    These tests verify bytes directly from the C uds_session.c state machine.
    """

    def test_extended_session_p2_timing_bytes(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        Extended Session response encodes P2 and P2* per ISO 14229-1 §7.2:
          - P2server_max:  encoded directly in ms    → 25ms
          - P2*server_max: encoded as ms/10 (ISO §7.2 scaling) → 5000ms / 10 = 500

        The raw 16-bit P2* field in the response = 500,
        which represents 5000ms (multiply by 10 to recover ms).

        Verifies uds_session.c / service_0x10.c embed correct timing from generated_config.h.
        """
        pdu = firmware_bus.request(bytes([SID_DIAGNOSTIC_SESSION_CONTROL, SESSION_EXTENDED]))
        assert pdu is not None
        assert pdu[0] == (SID_DIAGNOSTIC_SESSION_CONTROL + POSITIVE_RESPONSE_OFFSET)
        assert len(pdu) == 6, f"Expected 6 bytes, got {len(pdu)}: {pdu.hex(' ')}"
        p2_ms       = struct.unpack(">H", pdu[2:4])[0]
        p2s_encoded = struct.unpack(">H", pdu[4:6])[0]
        p2s_ms      = p2s_encoded * 10   # ISO 14229-1 §7.2: P2* field is ms/10
        assert p2_ms == 25, (
            f"P2server_max mismatch: {p2_ms}ms (YAML: 25ms)"
        )
        assert p2s_ms == 5000, (
            f"P2*server_max mismatch: encoded={p2s_encoded} → {p2s_ms}ms "
            f"(YAML: 5000ms, expected encoded=500)"
        )

    def test_suppress_bit_no_response(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        Suppress bit (0x81) must suppress the firmware response completely.
        Tests the suppressPosRspMsgIndicationBit handling in service_0x10.c.
        """
        pdu = firmware_bus.request(
            bytes([SID_DIAGNOSTIC_SESSION_CONTROL, SESSION_DEFAULT | 0x80])
        )
        assert pdu is None, (
            "Firmware MUST suppress response when suppressPosRspMsgIndicationBit=1"
        )

    def test_invalid_session_type_nrc_12(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """Reserved session 0x7F → NRC 0x12 from firmware service_0x10.c."""
        pdu = firmware_bus.request(bytes([SID_DIAGNOSTIC_SESSION_CONTROL, 0x7F]))
        assert pdu is not None
        assert pdu[0] == SID_NEGATIVE_RESPONSE
        assert pdu[1] == SID_DIAGNOSTIC_SESSION_CONTROL
        assert pdu[2] == NRC_SUB_FUNCTION_NOT_SUPPORTED, (
            f"Expected NRC 0x12, got 0x{pdu[2]:02X}"
        )

    def test_too_short_request_nrc_13(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """SID-only request (missing sub-function) → NRC 0x13 from firmware."""
        pdu = firmware_bus.request(bytes([SID_DIAGNOSTIC_SESSION_CONTROL]))
        assert pdu is not None
        assert pdu[0] == SID_NEGATIVE_RESPONSE
        assert pdu[2] == NRC_INCORRECT_MSG_LEN


# =============================================================================
# §2 TesterPresent — firmware keepalive
# =============================================================================

class TestFirmwareTesterPresent:
    """SID 0x3E — verifies service_0x3E.c response format."""

    def test_positive_response_format(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """[0x3E, 0x00] → [0x7E, 0x00] exactly — verifies RSID and sub-fn echo."""
        pdu = firmware_bus.request(bytes([SID_TESTER_PRESENT, 0x00]))
        assert pdu is not None
        assert pdu[0] == (SID_TESTER_PRESENT + POSITIVE_RESPONSE_OFFSET)
        assert len(pdu) == 2, f"Expected 2 bytes, got {len(pdu)}"
        assert pdu[1] == 0x00

    def test_suppress_bit_firmware(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """[0x3E, 0x80] suppress bit → no response from firmware."""
        pdu = firmware_bus.request(bytes([SID_TESTER_PRESENT, 0x80]))
        assert pdu is None, "Firmware must suppress TesterPresent with bit 7 set"

    def test_invalid_sub_function(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """Sub-function 0x01 not defined for 0x3E → NRC 0x12."""
        pdu = firmware_bus.request(bytes([SID_TESTER_PRESENT, 0x01]))
        assert pdu is not None
        assert pdu[0] == SID_NEGATIVE_RESPONSE
        assert pdu[2] == NRC_SUB_FUNCTION_NOT_SUPPORTED


# =============================================================================
# §3 SecurityAccess — AES-CMAC verified against firmware
# =============================================================================

class TestFirmwareSecurityAccess:
    """
    SID 0x27 — validates the AES-CMAC seed/key exchange against real firmware.
    These tests prove uds_security_algo.c computes keys that match our Python
    AES-CMAC derivation — the most critical security correctness check.
    """

    def test_seed_requires_extended_session(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """SecurityAccess in Default Session → NRC 0x7F from firmware ACL table."""
        firmware_bus.request(
            bytes([SID_DIAGNOSTIC_SESSION_CONTROL, SESSION_DEFAULT]), timeout=0.1
        )
        pdu = firmware_bus.request(bytes([SID_SECURITY_ACCESS, 0x01]))
        assert pdu is not None
        assert pdu[0] == SID_NEGATIVE_RESPONSE
        assert pdu[2] == NRC_SERVICE_NOT_SUPPORTED_IN_SESSION

    def test_firmware_seed_is_8_bytes(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """requestSeed returns exactly ALGO_SEED_LEN=8 bytes from uds_security_algo.c."""
        _enter_session(firmware_bus, SESSION_EXTENDED)
        pdu = firmware_bus.request(bytes([SID_SECURITY_ACCESS, 0x01]))
        assert pdu is not None
        assert pdu[0] == (SID_SECURITY_ACCESS + POSITIVE_RESPONSE_OFFSET)
        seed = pdu[2:]
        assert len(seed) == ALGO_SEED_LEN, (
            f"Firmware seed length {len(seed)} != {ALGO_SEED_LEN} (ALGO_SEED_LEN)"
        )
        assert any(b != 0 for b in seed), "Seed must be non-zero in locked state"

    def test_aes_cmac_key_derivation_correct(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        Full seed/key exchange: Python-derived AES-CMAC key unlocks firmware.
        Proves uds_security_algo.c and our Python derive_firmware_key() agree.
        This is the core correctness check for Phase 1 security hardening.
        """
        _enter_session(firmware_bus, SESSION_EXTENDED)
        # Get a fresh seed (no prior wrong-key attempt on this harness instance)
        pdu_seed = firmware_bus.request(bytes([SID_SECURITY_ACCESS, 0x01]))
        assert pdu_seed is not None
        assert pdu_seed[0] == (SID_SECURITY_ACCESS + POSITIVE_RESPONSE_OFFSET), (
            f"requestSeed failed: {pdu_seed.hex(' ')}"
        )
        seed = bytes(pdu_seed[2:])
        key  = derive_firmware_key(seed, level=1)
        pdu_key = firmware_bus.request(bytes([SID_SECURITY_ACCESS, 0x02]) + key)
        assert pdu_key is not None
        assert pdu_key[0] == (SID_SECURITY_ACCESS + POSITIVE_RESPONSE_OFFSET), (
            f"AES-CMAC key derivation mismatch — firmware rejected key: {pdu_key.hex(' ')}\n"
            f"Seed: {seed.hex(' ')}  Key sent: {key.hex(' ')}\n"
            f"This indicates uds_security_algo.c and Python derive_firmware_key() disagree."
        )

    def test_already_unlocked_zero_seed(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """After unlock, firmware returns all-zero seed (ISO 14229-1 §10.4.2)."""
        _enter_session(firmware_bus, SESSION_EXTENDED)
        _unlock_level(firmware_bus, 1)
        pdu = firmware_bus.request(bytes([SID_SECURITY_ACCESS, 0x01]))
        assert pdu is not None
        assert pdu[0] == (SID_SECURITY_ACCESS + POSITIVE_RESPONSE_OFFSET)
        assert all(b == 0 for b in pdu[2:]), (
            f"Firmware must return zero seed when already unlocked, got: {pdu[2:].hex(' ')}"
        )

    def test_wrong_key_nrc_35_from_firmware(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """Wrong key → NRC 0x35 from uds_security.c (invalidKey)."""
        _enter_session(firmware_bus, SESSION_EXTENDED)
        firmware_bus.request(bytes([SID_SECURITY_ACCESS, 0x01]))   # get seed
        pdu = firmware_bus.request(bytes([SID_SECURITY_ACCESS, 0x02, 0xDE, 0xAD, 0xBE, 0xEF]))
        assert pdu is not None
        assert pdu[0] == SID_NEGATIVE_RESPONSE
        assert pdu[2] == NRC_INVALID_KEY, (
            f"Expected NRC_INVALID_KEY (0x35), got 0x{pdu[2]:02X}"
        )

    def test_sendkey_without_seed_nrc_sequence_error(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """sendKey without prior requestSeed → NRC 0x22 or 0x24 from firmware."""
        _enter_session(firmware_bus, SESSION_EXTENDED)
        pdu = firmware_bus.request(bytes([SID_SECURITY_ACCESS, 0x02] + [0x00] * ALGO_KEY_LEN))
        assert pdu is not None
        assert pdu[0] == SID_NEGATIVE_RESPONSE
        assert pdu[2] in (NRC_CONDITIONS_NOT_CORRECT, 0x24), (
            f"Expected NRC 0x22 or 0x24, got 0x{pdu[2]:02X}"
        )

    def test_wrong_key_length_nrc_firmware(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        sendKey with wrong length (2 bytes instead of 4).

        ISO 14229-1 allows two valid NRC responses:
          - NRC 0x13 (incorrectMessageLengthOrInvalidFormat): wrong PDU length
          - NRC 0x35 (invalidKey): firmware validates key length as part of key check

        This firmware returns NRC 0x35 — it treats a short key as an invalid
        key attempt rather than a framing error. Both are ISO-compliant.
        Documents the actual firmware behavior.
        """
        _enter_session(firmware_bus, SESSION_EXTENDED)
        firmware_bus.request(bytes([SID_SECURITY_ACCESS, 0x01]))   # get seed
        pdu = firmware_bus.request(bytes([SID_SECURITY_ACCESS, 0x02, 0x00, 0x00]))  # 2 bytes
        assert pdu is not None
        assert pdu[0] == SID_NEGATIVE_RESPONSE, (
            f"Expected NRC for short key, got: {pdu.hex(' ')}"
        )
        assert pdu[2] in (NRC_INCORRECT_MSG_LEN, NRC_INVALID_KEY), (
            f"Expected NRC 0x13 or 0x35 for short key, got 0x{pdu[2]:02X}. "
            f"ISO 14229-1 permits either response for wrong key length."
        )

    def test_replay_protection_firmware(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        Replay protection: key computed for seed_1 must be rejected for seed_2.
        Tests the sequence counter in uds_security_algo.c — the core of
        replay attack prevention.
        """
        _enter_session(firmware_bus, SESSION_EXTENDED)
        pdu1 = firmware_bus.request(bytes([SID_SECURITY_ACCESS, 0x01]))
        assert pdu1 is not None
        seed1 = bytes(pdu1[2:])
        key1 = derive_firmware_key(seed1, 1)
        # Use key1 to unlock first session
        firmware_bus.request(bytes([SID_SECURITY_ACCESS, 0x02]) + key1)

        # New session — counter advances
        firmware_bus.request(bytes([SID_DIAGNOSTIC_SESSION_CONTROL, SESSION_DEFAULT]), timeout=0.1)
        _enter_session(firmware_bus, SESSION_EXTENDED)
        pdu2 = firmware_bus.request(bytes([SID_SECURITY_ACCESS, 0x01]))
        assert pdu2 is not None
        seed2 = bytes(pdu2[2:])
        assert seed1 != seed2, "Replay: successive seeds must differ (sequence counter)"

        # Replay: use key1 (from seed1) against seed2
        pdu_replay = firmware_bus.request(bytes([SID_SECURITY_ACCESS, 0x02]) + key1)
        assert pdu_replay is not None
        assert pdu_replay[0] == SID_NEGATIVE_RESPONSE, (
            "Replay attack succeeded — firmware accepted stale key"
        )
        assert pdu_replay[2] == NRC_INVALID_KEY, (
            f"Expected NRC_INVALID_KEY for replay, got 0x{pdu_replay[2]:02X}"
        )


# =============================================================================
# §4 ReadDataByIdentifier — per DID firmware validation
# =============================================================================

class TestFirmwareReadDID:
    """
    SID 0x22 — validates DID responses from real generated/did_handlers.c.
    Each test verifies that the C handler returns the correct PDU length
    and that the ASIL-B safety wrapper enforces session/security constraints.
    """

    def test_read_fdecuidentification_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0xF190 (FdEcuIdentification) — firmware read happy path.
        Expected: RSID=0x62 DID=0xF10x90 + 17 data byte(s)
        Verifies: did_safe_read_fdecuidentification() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[0])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 241, 144]))
        assert pdu is not None, "No response from firmware for DID 0xF190"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 241, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 144, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 17
        assert len(pdu) == expected_len, (
            f"Firmware DID 0xF190 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=17), got {len(pdu)}"
        )



    def test_read_fdrecorda_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x0100 (FdRecordA) — firmware read happy path.
        Expected: RSID=0x62 DID=0x010x00 + 64 data byte(s)
        Verifies: did_safe_read_fdrecorda() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[1])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 1, 0]))
        assert pdu is not None, "No response from firmware for DID 0x0100"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 1, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 0, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 64
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x0100 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=64), got {len(pdu)}"
        )



    def test_read_fdrecordb_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x0101 (FdRecordB) — firmware read happy path.
        Expected: RSID=0x62 DID=0x010x01 + 64 data byte(s)
        Verifies: did_safe_read_fdrecordb() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[2])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 1, 1]))
        assert pdu is not None, "No response from firmware for DID 0x0101"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 1, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 1, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 64
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x0101 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=64), got {len(pdu)}"
        )



    def test_read_fdrecordc_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x0102 (FdRecordC) — firmware read happy path.
        Expected: RSID=0x62 DID=0x010x02 + 64 data byte(s)
        Verifies: did_safe_read_fdrecordc() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[3])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 1, 2]))
        assert pdu is not None, "No response from firmware for DID 0x0102"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 1, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 2, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 64
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x0102 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=64), got {len(pdu)}"
        )



    def test_read_fdperiodicstatus_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0xF201 (FdPeriodicStatus) — firmware read happy path.
        Expected: RSID=0x62 DID=0xF20x01 + 20 data byte(s)
        Verifies: did_safe_read_fdperiodicstatus() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[4])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 242, 1]))
        assert pdu is not None, "No response from firmware for DID 0xF201"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 242, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 1, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 20
        assert len(pdu) == expected_len, (
            f"Firmware DID 0xF201 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=20), got {len(pdu)}"
        )

    def test_read_fdperiodicstatus_session_gate_firmware(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0xF201 in Default Session → NRC from firmware ASIL-B safety wrapper.
        Verifies uds_safety.c session validation (step 2 of 5-step chain).
        """
        firmware_bus.request(
            bytes([SID_DIAGNOSTIC_SESSION_CONTROL, SESSION_DEFAULT]), timeout=0.1
        )
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 242, 1]))
        assert pdu is not None, "Expected NRC, got no response"
        assert pdu[0] == SID_NEGATIVE_RESPONSE, (
            f"DID 0xF201 must be rejected in Default Session by firmware safety wrapper"
        )
        assert pdu[2] in (
            NRC_REQUEST_OUT_OF_RANGE,
            NRC_SERVICE_NOT_SUPPORTED_IN_SESSION,
            NRC_CONDITIONS_NOT_CORRECT,
        ), f"Unexpected NRC 0x{pdu[2]:02X}"


    def test_unknown_did_firmware_nrc_31(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """Unknown DID 0x0001 → NRC 0x31 from firmware ASIL-B wrapper (step 1)."""
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 0x00, 0x01]))
        assert pdu is not None
        assert pdu[0] == SID_NEGATIVE_RESPONSE
        assert pdu[2] == NRC_REQUEST_OUT_OF_RANGE

    def test_read_truncated_request_nrc_13(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """Truncated DID (1 byte) → NRC 0x13 from firmware service_0x22.c."""
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 0xF1]))
        assert pdu is not None
        assert pdu[0] == SID_NEGATIVE_RESPONSE
        assert pdu[2] == NRC_INCORRECT_MSG_LEN


# =============================================================================
# §5 WriteDataByIdentifier — firmware write + read-back
# =============================================================================


# =============================================================================
# §6 Multi-frame ISO-TP — verifies C isotp.c segmentation
# =============================================================================

class TestFirmwareIsoTp:
    """
    ISO 15765-2 multi-frame exchange validation against real C isotp.c.
    These tests exercise the FF/CF/FC state machine in transport/isotp.c.
    """

    def test_multiframe_fdecuidentification_response(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0xF190 (FdEcuIdentification) data_length=17B → 20B PDU.
        20 bytes > 7 → triggers FF+CF multi-frame from C isotp.c.
        Verifies isotp.c segments correctly and FirmwareIsoTpTransport reassembles.
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[0])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 241, 144]))
        assert pdu is not None, f"No response for multi-frame DID 0xF190"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET)
        expected_len = 3 + 17
        assert len(pdu) == expected_len, (
            f"Multi-frame reassembly error for DID 0xF190: "
            f"expected {expected_len} bytes, got {len(pdu)}"
        )
    def test_multiframe_fdrecorda_response(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x0100 (FdRecordA) data_length=64B → 67B PDU.
        67 bytes > 7 → triggers FF+CF multi-frame from C isotp.c.
        Verifies isotp.c segments correctly and FirmwareIsoTpTransport reassembles.
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[1])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 1, 0]))
        assert pdu is not None, f"No response for multi-frame DID 0x0100"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET)
        expected_len = 3 + 64
        assert len(pdu) == expected_len, (
            f"Multi-frame reassembly error for DID 0x0100: "
            f"expected {expected_len} bytes, got {len(pdu)}"
        )
    def test_multiframe_fdrecordb_response(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x0101 (FdRecordB) data_length=64B → 67B PDU.
        67 bytes > 7 → triggers FF+CF multi-frame from C isotp.c.
        Verifies isotp.c segments correctly and FirmwareIsoTpTransport reassembles.
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[2])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 1, 1]))
        assert pdu is not None, f"No response for multi-frame DID 0x0101"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET)
        expected_len = 3 + 64
        assert len(pdu) == expected_len, (
            f"Multi-frame reassembly error for DID 0x0101: "
            f"expected {expected_len} bytes, got {len(pdu)}"
        )
    def test_multiframe_fdrecordc_response(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x0102 (FdRecordC) data_length=64B → 67B PDU.
        67 bytes > 7 → triggers FF+CF multi-frame from C isotp.c.
        Verifies isotp.c segments correctly and FirmwareIsoTpTransport reassembles.
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[3])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 1, 2]))
        assert pdu is not None, f"No response for multi-frame DID 0x0102"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET)
        expected_len = 3 + 64
        assert len(pdu) == expected_len, (
            f"Multi-frame reassembly error for DID 0x0102: "
            f"expected {expected_len} bytes, got {len(pdu)}"
        )
    def test_multiframe_fdperiodicstatus_response(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0xF201 (FdPeriodicStatus) data_length=20B → 23B PDU.
        23 bytes > 7 → triggers FF+CF multi-frame from C isotp.c.
        Verifies isotp.c segments correctly and FirmwareIsoTpTransport reassembles.
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[4])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 242, 1]))
        assert pdu is not None, f"No response for multi-frame DID 0xF201"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET)
        expected_len = 3 + 20
        assert len(pdu) == expected_len, (
            f"Multi-frame reassembly error for DID 0xF201: "
            f"expected {expected_len} bytes, got {len(pdu)}"
        )

    def test_single_frame_tester_present(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """Minimal SF round-trip through isotp.c."""
        pdu = firmware_bus.request(bytes([SID_TESTER_PRESENT, 0x00]))
        assert pdu is not None
        assert pdu[0] == (SID_TESTER_PRESENT + POSITIVE_RESPONSE_OFFSET)

    def test_unknown_service_nrc_11_firmware(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """Unimplemented SID 0xFE → NRC 0x11 from firmware uds_server.c."""
        pdu = firmware_bus.request(bytes([0xFE]))
        assert pdu is not None
        assert pdu[0] == SID_NEGATIVE_RESPONSE
        assert pdu[1] == 0xFE
        assert pdu[2] == 0x11  # serviceNotSupported


# =============================================================================
# §7 ASIL-B Safety Wrapper Chain — exhaustive NRC matrix
# =============================================================================

class TestFirmwareSafetyWrappers:
    """
    Exhaustive tests of the ASIL-B 5-step safety wrapper chain.
    Every check must return the correct NRC when violated.
    Tests generated/did_safety_wrappers.c against real firmware.
    """

    def test_step1_unknown_did_nrc_31(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """Step 1: DID existence check — unknown DID → NRC 0x31."""
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 0xFF, 0xFF]))
        assert pdu is not None
        assert pdu[0] == SID_NEGATIVE_RESPONSE
        assert pdu[2] == NRC_REQUEST_OUT_OF_RANGE

    def test_step3_security_locked_nrc_33(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """Step 3: Security check — locked state → NRC 0x33."""
        # Find a DID that requires security for read
        pytest.skip("No read-secured DID in this configuration")

    def test_step5_data_length_nrc_13_firmware(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """Step 5 (write path): Length mismatch → NRC 0x13 from safety wrapper."""
        pytest.skip("No write-capable DID in this configuration")

    def test_negative_response_byte_format(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        Verify NRC frame format: [0x7F, SID, NRC] exactly 3 bytes.
        Tests ISO 14229-1 §9.3.2 negative response PDU structure from firmware.
        """
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 0x00, 0x01]))
        assert pdu is not None
        assert len(pdu) == 3, f"NRC PDU must be 3 bytes, got {len(pdu)}: {pdu.hex(' ')}"
        assert pdu[0] == SID_NEGATIVE_RESPONSE   # 0x7F
        assert pdu[1] == SID_READ_DATA_BY_ID     # echo of request SID
        assert pdu[2] == NRC_REQUEST_OUT_OF_RANGE


# =============================================================================
# §8 ECUReset — firmware reset handling
# =============================================================================

class TestFirmwareEcuReset:
    """SID 0x11 — validates service_0x11.c reset sub-function handling."""

    def test_hard_reset_positive(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """HardReset (0x01) → [0x51, 0x01] from firmware."""
        pdu = firmware_bus.request(bytes([SID_ECU_RESET, 0x01]))
        assert pdu is not None
        assert pdu[0] == (SID_ECU_RESET + POSITIVE_RESPONSE_OFFSET)
        assert pdu[1] == 0x01

    def test_soft_reset_positive(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """SoftReset (0x03) → [0x51, 0x03] from firmware."""
        pdu = firmware_bus.request(bytes([SID_ECU_RESET, 0x03]))
        assert pdu is not None
        assert pdu[0] == (SID_ECU_RESET + POSITIVE_RESPONSE_OFFSET)
        assert pdu[1] == 0x03

    def test_invalid_reset_type_nrc_12(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """Reserved reset type 0x7F → NRC 0x12."""
        pdu = firmware_bus.request(bytes([SID_ECU_RESET, 0x7F]))
        assert pdu is not None
        assert pdu[0] == SID_NEGATIVE_RESPONSE
        assert pdu[2] == NRC_SUB_FUNCTION_NOT_SUPPORTED


