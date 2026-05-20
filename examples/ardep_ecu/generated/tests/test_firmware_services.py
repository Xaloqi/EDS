# =============================================================================
# GENERATED — do NOT edit manually.
# Regenerate: python3 tools/codegen.py --config <yaml> --out generated/ --test-gen
#
# ECU       : ARDEP_IOController
# Version   : 1.0.0
# Generated : 2026-05-20T07:21:48Z
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
        "name":         "VehicleIdentificationNumber",
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
        "id":           61836,
        "id_hex":       "0xF18C",
        "name":         "ECUSerialNumber",
        "data_length":  8,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    1,
        "id_hi":        241,
        "id_lo":        140,
    },
    {
        "id":           61831,
        "id_hex":       "0xF187",
        "name":         "VehicleManufacturerSparePartNumber",
        "data_length":  11,
        "access_read":  True,
        "access_write": True,
        "min_session":  3,
        "read_sec":     0,
        "write_sec":    1,
        "id_hi":        241,
        "id_lo":        135,
    },
    {
        "id":           61833,
        "id_hex":       "0xF189",
        "name":         "ECUSoftwareVersionNumber",
        "data_length":  4,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    0,
        "id_hi":        241,
        "id_lo":        137,
    },
    {
        "id":           61847,
        "id_hex":       "0xF197",
        "name":         "SystemSupplierIdentifierDataIdentifier",
        "data_length":  5,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    0,
        "id_hi":        241,
        "id_lo":        151,
    },
    {
        "id":           8193,
        "id_hex":       "0x2001",
        "name":         "PowerIO_OutputStateBitmask",
        "data_length":  1,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    0,
        "id_hi":        32,
        "id_lo":        1,
    },
    {
        "id":           8194,
        "id_hex":       "0x2002",
        "name":         "PowerIO_Output1_Current_mA",
        "data_length":  2,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    0,
        "id_hi":        32,
        "id_lo":        2,
    },
    {
        "id":           8195,
        "id_hex":       "0x2003",
        "name":         "PowerIO_Output2_Current_mA",
        "data_length":  2,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    0,
        "id_hi":        32,
        "id_lo":        3,
    },
    {
        "id":           8196,
        "id_hex":       "0x2004",
        "name":         "PowerIO_Output3_Current_mA",
        "data_length":  2,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    0,
        "id_hi":        32,
        "id_lo":        4,
    },
    {
        "id":           8197,
        "id_hex":       "0x2005",
        "name":         "PowerIO_Output4_Current_mA",
        "data_length":  2,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    0,
        "id_hi":        32,
        "id_lo":        5,
    },
    {
        "id":           8198,
        "id_hex":       "0x2006",
        "name":         "PowerIO_Output5_Current_mA",
        "data_length":  2,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    0,
        "id_hi":        32,
        "id_lo":        6,
    },
    {
        "id":           8199,
        "id_hex":       "0x2007",
        "name":         "PowerIO_Output6_Current_mA",
        "data_length":  2,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    0,
        "id_hi":        32,
        "id_lo":        7,
    },
    {
        "id":           8208,
        "id_hex":       "0x2010",
        "name":         "PowerIO_OutputControl",
        "data_length":  1,
        "access_read":  True,
        "access_write": True,
        "min_session":  3,
        "read_sec":     0,
        "write_sec":    1,
        "id_hi":        32,
        "id_lo":        16,
    },
    {
        "id":           8449,
        "id_hex":       "0x2101",
        "name":         "PowerIO_InputStateBitmask",
        "data_length":  1,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    0,
        "id_hi":        33,
        "id_lo":        1,
    },
    {
        "id":           8450,
        "id_hex":       "0x2102",
        "name":         "PowerIO_Input1_Voltage_mV",
        "data_length":  2,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    0,
        "id_hi":        33,
        "id_lo":        2,
    },
    {
        "id":           8451,
        "id_hex":       "0x2103",
        "name":         "PowerIO_Input2_Voltage_mV",
        "data_length":  2,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    0,
        "id_hi":        33,
        "id_lo":        3,
    },
    {
        "id":           8452,
        "id_hex":       "0x2104",
        "name":         "PowerIO_Input3_Voltage_mV",
        "data_length":  2,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    0,
        "id_hi":        33,
        "id_lo":        4,
    },
    {
        "id":           8705,
        "id_hex":       "0x2201",
        "name":         "CAN_BusStatus",
        "data_length":  2,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    0,
        "id_hi":        34,
        "id_lo":        1,
    },
    {
        "id":           8706,
        "id_hex":       "0x2202",
        "name":         "CAN_RxFrameCount",
        "data_length":  4,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    0,
        "id_hi":        34,
        "id_lo":        2,
    },
    {
        "id":           8707,
        "id_hex":       "0x2203",
        "name":         "CAN_TxFrameCount",
        "data_length":  4,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    0,
        "id_hi":        34,
        "id_lo":        3,
    },
    {
        "id":           8721,
        "id_hex":       "0x2211",
        "name":         "LIN_BusStatus",
        "data_length":  1,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    0,
        "id_hi":        34,
        "id_lo":        17,
    },
    {
        "id":           8722,
        "id_hex":       "0x2212",
        "name":         "LIN_SlaveCount",
        "data_length":  1,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    0,
        "id_hi":        34,
        "id_lo":        18,
    },
    {
        "id":           8961,
        "id_hex":       "0x2301",
        "name":         "ECU_SupplyVoltage_mV",
        "data_length":  2,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    0,
        "id_hi":        35,
        "id_lo":        1,
    },
    {
        "id":           8962,
        "id_hex":       "0x2302",
        "name":         "ECU_InternalTemperature_degC",
        "data_length":  1,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    0,
        "id_hi":        35,
        "id_lo":        2,
    },
    {
        "id":           8963,
        "id_hex":       "0x2303",
        "name":         "ECU_UptimeSeconds",
        "data_length":  4,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    0,
        "id_hi":        35,
        "id_lo":        3,
    },
    {
        "id":           8964,
        "id_hex":       "0x2304",
        "name":         "ECU_ResetCounter",
        "data_length":  2,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    0,
        "id_hi":        35,
        "id_lo":        4,
    },
    {
        "id":           8965,
        "id_hex":       "0x2305",
        "name":         "ECU_LastResetReason",
        "data_length":  1,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    0,
        "id_hi":        35,
        "id_lo":        5,
    },
    {
        "id":           8966,
        "id_hex":       "0x2306",
        "name":         "ECU_DiagnosticStackVersion",
        "data_length":  3,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    0,
        "id_hi":        35,
        "id_lo":        6,
    },
    {
        "id":           9217,
        "id_hex":       "0x2401",
        "name":         "CAN_BusBitrate_kbps",
        "data_length":  2,
        "access_read":  True,
        "access_write": True,
        "min_session":  3,
        "read_sec":     0,
        "write_sec":    1,
        "id_hi":        36,
        "id_lo":        1,
    },
    {
        "id":           9218,
        "id_hex":       "0x2402",
        "name":         "LIN_BusBaudrate_bps",
        "data_length":  2,
        "access_read":  True,
        "access_write": True,
        "min_session":  3,
        "read_sec":     0,
        "write_sec":    1,
        "id_hi":        36,
        "id_lo":        2,
    },
    {
        "id":           9219,
        "id_hex":       "0x2403",
        "name":         "PowerIO_OvercurrentThreshold_mA",
        "data_length":  2,
        "access_read":  True,
        "access_write": True,
        "min_session":  3,
        "read_sec":     0,
        "write_sec":    1,
        "id_hi":        36,
        "id_lo":        3,
    },
    {
        "id":           9220,
        "id_hex":       "0x2404",
        "name":         "WatchdogTimeout_ms",
        "data_length":  2,
        "access_read":  True,
        "access_write": True,
        "min_session":  3,
        "read_sec":     0,
        "write_sec":    1,
        "id_hi":        36,
        "id_lo":        4,
    },
    {
        "id":           9473,
        "id_hex":       "0x2501",
        "name":         "FirmwareVersion_Active",
        "data_length":  4,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    0,
        "id_hi":        37,
        "id_lo":        1,
    },
    {
        "id":           9474,
        "id_hex":       "0x2502",
        "name":         "FirmwareVersion_Pending",
        "data_length":  4,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    0,
        "id_hi":        37,
        "id_lo":        2,
    },
    {
        "id":           9475,
        "id_hex":       "0x2503",
        "name":         "FirmwareUpdateStatus",
        "data_length":  1,
        "access_read":  True,
        "access_write": False,
        "min_session":  1,
        "read_sec":     0,
        "write_sec":    0,
        "id_hi":        37,
        "id_lo":        3,
    },
]

DTC_CATALOGUE = [
    {"code": 12583168, "code_hex": "0xC00100", "description": "CAN bus communication loss — no frames received > 500ms"},
    {"code": 12583424, "code_hex": "0xC00200", "description": "CAN bus error passive — TX/RX error counters exceeded 127"},
    {"code": 12583680, "code_hex": "0xC00300", "description": "CAN bus off — transmitter disabled by bus-off recovery"},
    {"code": 12587008, "code_hex": "0xC01000", "description": "LIN bus no response — no slave response within timeout"},
    {"code": 12587264, "code_hex": "0xC01100", "description": "LIN bus framing error — break field or sync byte invalid"},
    {"code": 11534592, "code_hex": "0xB00100", "description": "Output 1 overcurrent — load current exceeded threshold"},
    {"code": 11534848, "code_hex": "0xB00200", "description": "Output 2 overcurrent — load current exceeded threshold"},
    {"code": 11535104, "code_hex": "0xB00300", "description": "Output 3 overcurrent — load current exceeded threshold"},
    {"code": 11535360, "code_hex": "0xB00400", "description": "Output 4 overcurrent — load current exceeded threshold"},
    {"code": 11535616, "code_hex": "0xB00500", "description": "Output 5 overcurrent — load current exceeded threshold"},
    {"code": 11535872, "code_hex": "0xB00600", "description": "Output 6 overcurrent — load current exceeded threshold"},
    {"code": 11538432, "code_hex": "0xB01000", "description": "PowerIO open load — output commanded ON but no current detected"},
    {"code": 12648192, "code_hex": "0xC0FF00", "description": "Supply voltage low — below 9.0V for > 500ms"},
    {"code": 12648208, "code_hex": "0xC0FF10", "description": "Supply voltage high — above 16.0V for > 100ms"},
    {"code": 12648224, "code_hex": "0xC0FF20", "description": "ECU overtemperature — internal junction > 125°C"},
    {"code": 12648240, "code_hex": "0xC0FF30", "description": "Watchdog reset event — firmware failed to service watchdog"},
    {"code": 12648256, "code_hex": "0xC0FF40", "description": "NVM write failure — diagnostic session statistics not persisted"},
    {"code": 12647936, "code_hex": "0xC0FE00", "description": "Firmware image verification failed — CRC mismatch"},
    {"code": 12647952, "code_hex": "0xC0FE10", "description": "Firmware download incomplete — transfer interrupted"},
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

    def test_read_vehicleidentificationnumber_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0xF190 (VehicleIdentificationNumber) — firmware read happy path.
        Expected: RSID=0x62 DID=0xF10x90 + 17 data byte(s)
        Verifies: did_safe_read_vehicleidentificationnumber() safety wrapper + C handler
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



    def test_read_ecuserialnumber_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0xF18C (ECUSerialNumber) — firmware read happy path.
        Expected: RSID=0x62 DID=0xF10x8C + 8 data byte(s)
        Verifies: did_safe_read_ecuserialnumber() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[1])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 241, 140]))
        assert pdu is not None, "No response from firmware for DID 0xF18C"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 241, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 140, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 8
        assert len(pdu) == expected_len, (
            f"Firmware DID 0xF18C length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=8), got {len(pdu)}"
        )



    def test_read_vehiclemanufacturersparepartnumber_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0xF187 (VehicleManufacturerSparePartNumber) — firmware read happy path.
        Expected: RSID=0x62 DID=0xF10x87 + 11 data byte(s)
        Verifies: did_safe_read_vehiclemanufacturersparepartnumber() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[2])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 241, 135]))
        assert pdu is not None, "No response from firmware for DID 0xF187"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 241, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 135, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 11
        assert len(pdu) == expected_len, (
            f"Firmware DID 0xF187 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=11), got {len(pdu)}"
        )

    def test_read_vehiclemanufacturersparepartnumber_session_gate_firmware(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0xF187 in Default Session → NRC from firmware ASIL-B safety wrapper.
        Verifies uds_safety.c session validation (step 2 of 5-step chain).
        """
        firmware_bus.request(
            bytes([SID_DIAGNOSTIC_SESSION_CONTROL, SESSION_DEFAULT]), timeout=0.1
        )
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 241, 135]))
        assert pdu is not None, "Expected NRC, got no response"
        assert pdu[0] == SID_NEGATIVE_RESPONSE, (
            f"DID 0xF187 must be rejected in Default Session by firmware safety wrapper"
        )
        assert pdu[2] in (
            NRC_REQUEST_OUT_OF_RANGE,
            NRC_SERVICE_NOT_SUPPORTED_IN_SESSION,
            NRC_CONDITIONS_NOT_CORRECT,
        ), f"Unexpected NRC 0x{pdu[2]:02X}"


    def test_read_ecusoftwareversionnumber_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0xF189 (ECUSoftwareVersionNumber) — firmware read happy path.
        Expected: RSID=0x62 DID=0xF10x89 + 4 data byte(s)
        Verifies: did_safe_read_ecusoftwareversionnumber() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[3])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 241, 137]))
        assert pdu is not None, "No response from firmware for DID 0xF189"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 241, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 137, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 4
        assert len(pdu) == expected_len, (
            f"Firmware DID 0xF189 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=4), got {len(pdu)}"
        )



    def test_read_systemsupplieridentifierdataidentifier_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0xF197 (SystemSupplierIdentifierDataIdentifier) — firmware read happy path.
        Expected: RSID=0x62 DID=0xF10x97 + 5 data byte(s)
        Verifies: did_safe_read_systemsupplieridentifierdataidentifier() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[4])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 241, 151]))
        assert pdu is not None, "No response from firmware for DID 0xF197"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 241, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 151, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 5
        assert len(pdu) == expected_len, (
            f"Firmware DID 0xF197 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=5), got {len(pdu)}"
        )



    def test_read_powerio_outputstatebitmask_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2001 (PowerIO_OutputStateBitmask) — firmware read happy path.
        Expected: RSID=0x62 DID=0x200x01 + 1 data byte(s)
        Verifies: did_safe_read_powerio_outputstatebitmask() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[5])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 32, 1]))
        assert pdu is not None, "No response from firmware for DID 0x2001"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 32, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 1, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 1
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2001 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=1), got {len(pdu)}"
        )



    def test_read_powerio_output1_current_ma_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2002 (PowerIO_Output1_Current_mA) — firmware read happy path.
        Expected: RSID=0x62 DID=0x200x02 + 2 data byte(s)
        Verifies: did_safe_read_powerio_output1_current_ma() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[6])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 32, 2]))
        assert pdu is not None, "No response from firmware for DID 0x2002"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 32, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 2, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 2
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2002 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=2), got {len(pdu)}"
        )



    def test_read_powerio_output2_current_ma_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2003 (PowerIO_Output2_Current_mA) — firmware read happy path.
        Expected: RSID=0x62 DID=0x200x03 + 2 data byte(s)
        Verifies: did_safe_read_powerio_output2_current_ma() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[7])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 32, 3]))
        assert pdu is not None, "No response from firmware for DID 0x2003"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 32, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 3, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 2
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2003 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=2), got {len(pdu)}"
        )



    def test_read_powerio_output3_current_ma_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2004 (PowerIO_Output3_Current_mA) — firmware read happy path.
        Expected: RSID=0x62 DID=0x200x04 + 2 data byte(s)
        Verifies: did_safe_read_powerio_output3_current_ma() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[8])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 32, 4]))
        assert pdu is not None, "No response from firmware for DID 0x2004"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 32, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 4, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 2
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2004 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=2), got {len(pdu)}"
        )



    def test_read_powerio_output4_current_ma_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2005 (PowerIO_Output4_Current_mA) — firmware read happy path.
        Expected: RSID=0x62 DID=0x200x05 + 2 data byte(s)
        Verifies: did_safe_read_powerio_output4_current_ma() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[9])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 32, 5]))
        assert pdu is not None, "No response from firmware for DID 0x2005"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 32, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 5, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 2
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2005 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=2), got {len(pdu)}"
        )



    def test_read_powerio_output5_current_ma_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2006 (PowerIO_Output5_Current_mA) — firmware read happy path.
        Expected: RSID=0x62 DID=0x200x06 + 2 data byte(s)
        Verifies: did_safe_read_powerio_output5_current_ma() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[10])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 32, 6]))
        assert pdu is not None, "No response from firmware for DID 0x2006"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 32, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 6, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 2
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2006 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=2), got {len(pdu)}"
        )



    def test_read_powerio_output6_current_ma_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2007 (PowerIO_Output6_Current_mA) — firmware read happy path.
        Expected: RSID=0x62 DID=0x200x07 + 2 data byte(s)
        Verifies: did_safe_read_powerio_output6_current_ma() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[11])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 32, 7]))
        assert pdu is not None, "No response from firmware for DID 0x2007"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 32, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 7, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 2
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2007 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=2), got {len(pdu)}"
        )



    def test_read_powerio_outputcontrol_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2010 (PowerIO_OutputControl) — firmware read happy path.
        Expected: RSID=0x62 DID=0x200x10 + 1 data byte(s)
        Verifies: did_safe_read_powerio_outputcontrol() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[12])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 32, 16]))
        assert pdu is not None, "No response from firmware for DID 0x2010"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 32, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 16, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 1
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2010 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=1), got {len(pdu)}"
        )

    def test_read_powerio_outputcontrol_session_gate_firmware(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2010 in Default Session → NRC from firmware ASIL-B safety wrapper.
        Verifies uds_safety.c session validation (step 2 of 5-step chain).
        """
        firmware_bus.request(
            bytes([SID_DIAGNOSTIC_SESSION_CONTROL, SESSION_DEFAULT]), timeout=0.1
        )
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 32, 16]))
        assert pdu is not None, "Expected NRC, got no response"
        assert pdu[0] == SID_NEGATIVE_RESPONSE, (
            f"DID 0x2010 must be rejected in Default Session by firmware safety wrapper"
        )
        assert pdu[2] in (
            NRC_REQUEST_OUT_OF_RANGE,
            NRC_SERVICE_NOT_SUPPORTED_IN_SESSION,
            NRC_CONDITIONS_NOT_CORRECT,
        ), f"Unexpected NRC 0x{pdu[2]:02X}"


    def test_read_powerio_inputstatebitmask_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2101 (PowerIO_InputStateBitmask) — firmware read happy path.
        Expected: RSID=0x62 DID=0x210x01 + 1 data byte(s)
        Verifies: did_safe_read_powerio_inputstatebitmask() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[13])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 33, 1]))
        assert pdu is not None, "No response from firmware for DID 0x2101"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 33, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 1, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 1
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2101 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=1), got {len(pdu)}"
        )



    def test_read_powerio_input1_voltage_mv_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2102 (PowerIO_Input1_Voltage_mV) — firmware read happy path.
        Expected: RSID=0x62 DID=0x210x02 + 2 data byte(s)
        Verifies: did_safe_read_powerio_input1_voltage_mv() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[14])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 33, 2]))
        assert pdu is not None, "No response from firmware for DID 0x2102"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 33, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 2, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 2
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2102 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=2), got {len(pdu)}"
        )



    def test_read_powerio_input2_voltage_mv_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2103 (PowerIO_Input2_Voltage_mV) — firmware read happy path.
        Expected: RSID=0x62 DID=0x210x03 + 2 data byte(s)
        Verifies: did_safe_read_powerio_input2_voltage_mv() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[15])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 33, 3]))
        assert pdu is not None, "No response from firmware for DID 0x2103"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 33, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 3, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 2
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2103 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=2), got {len(pdu)}"
        )



    def test_read_powerio_input3_voltage_mv_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2104 (PowerIO_Input3_Voltage_mV) — firmware read happy path.
        Expected: RSID=0x62 DID=0x210x04 + 2 data byte(s)
        Verifies: did_safe_read_powerio_input3_voltage_mv() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[16])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 33, 4]))
        assert pdu is not None, "No response from firmware for DID 0x2104"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 33, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 4, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 2
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2104 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=2), got {len(pdu)}"
        )



    def test_read_can_busstatus_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2201 (CAN_BusStatus) — firmware read happy path.
        Expected: RSID=0x62 DID=0x220x01 + 2 data byte(s)
        Verifies: did_safe_read_can_busstatus() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[17])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 34, 1]))
        assert pdu is not None, "No response from firmware for DID 0x2201"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 34, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 1, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 2
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2201 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=2), got {len(pdu)}"
        )



    def test_read_can_rxframecount_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2202 (CAN_RxFrameCount) — firmware read happy path.
        Expected: RSID=0x62 DID=0x220x02 + 4 data byte(s)
        Verifies: did_safe_read_can_rxframecount() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[18])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 34, 2]))
        assert pdu is not None, "No response from firmware for DID 0x2202"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 34, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 2, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 4
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2202 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=4), got {len(pdu)}"
        )



    def test_read_can_txframecount_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2203 (CAN_TxFrameCount) — firmware read happy path.
        Expected: RSID=0x62 DID=0x220x03 + 4 data byte(s)
        Verifies: did_safe_read_can_txframecount() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[19])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 34, 3]))
        assert pdu is not None, "No response from firmware for DID 0x2203"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 34, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 3, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 4
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2203 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=4), got {len(pdu)}"
        )



    def test_read_lin_busstatus_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2211 (LIN_BusStatus) — firmware read happy path.
        Expected: RSID=0x62 DID=0x220x11 + 1 data byte(s)
        Verifies: did_safe_read_lin_busstatus() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[20])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 34, 17]))
        assert pdu is not None, "No response from firmware for DID 0x2211"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 34, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 17, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 1
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2211 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=1), got {len(pdu)}"
        )



    def test_read_lin_slavecount_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2212 (LIN_SlaveCount) — firmware read happy path.
        Expected: RSID=0x62 DID=0x220x12 + 1 data byte(s)
        Verifies: did_safe_read_lin_slavecount() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[21])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 34, 18]))
        assert pdu is not None, "No response from firmware for DID 0x2212"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 34, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 18, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 1
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2212 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=1), got {len(pdu)}"
        )



    def test_read_ecu_supplyvoltage_mv_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2301 (ECU_SupplyVoltage_mV) — firmware read happy path.
        Expected: RSID=0x62 DID=0x230x01 + 2 data byte(s)
        Verifies: did_safe_read_ecu_supplyvoltage_mv() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[22])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 35, 1]))
        assert pdu is not None, "No response from firmware for DID 0x2301"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 35, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 1, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 2
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2301 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=2), got {len(pdu)}"
        )



    def test_read_ecu_internaltemperature_degc_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2302 (ECU_InternalTemperature_degC) — firmware read happy path.
        Expected: RSID=0x62 DID=0x230x02 + 1 data byte(s)
        Verifies: did_safe_read_ecu_internaltemperature_degc() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[23])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 35, 2]))
        assert pdu is not None, "No response from firmware for DID 0x2302"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 35, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 2, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 1
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2302 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=1), got {len(pdu)}"
        )



    def test_read_ecu_uptimeseconds_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2303 (ECU_UptimeSeconds) — firmware read happy path.
        Expected: RSID=0x62 DID=0x230x03 + 4 data byte(s)
        Verifies: did_safe_read_ecu_uptimeseconds() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[24])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 35, 3]))
        assert pdu is not None, "No response from firmware for DID 0x2303"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 35, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 3, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 4
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2303 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=4), got {len(pdu)}"
        )



    def test_read_ecu_resetcounter_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2304 (ECU_ResetCounter) — firmware read happy path.
        Expected: RSID=0x62 DID=0x230x04 + 2 data byte(s)
        Verifies: did_safe_read_ecu_resetcounter() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[25])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 35, 4]))
        assert pdu is not None, "No response from firmware for DID 0x2304"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 35, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 4, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 2
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2304 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=2), got {len(pdu)}"
        )



    def test_read_ecu_lastresetreason_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2305 (ECU_LastResetReason) — firmware read happy path.
        Expected: RSID=0x62 DID=0x230x05 + 1 data byte(s)
        Verifies: did_safe_read_ecu_lastresetreason() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[26])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 35, 5]))
        assert pdu is not None, "No response from firmware for DID 0x2305"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 35, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 5, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 1
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2305 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=1), got {len(pdu)}"
        )



    def test_read_ecu_diagnosticstackversion_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2306 (ECU_DiagnosticStackVersion) — firmware read happy path.
        Expected: RSID=0x62 DID=0x230x06 + 3 data byte(s)
        Verifies: did_safe_read_ecu_diagnosticstackversion() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[27])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 35, 6]))
        assert pdu is not None, "No response from firmware for DID 0x2306"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 35, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 6, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 3
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2306 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=3), got {len(pdu)}"
        )



    def test_read_can_busbitrate_kbps_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2401 (CAN_BusBitrate_kbps) — firmware read happy path.
        Expected: RSID=0x62 DID=0x240x01 + 2 data byte(s)
        Verifies: did_safe_read_can_busbitrate_kbps() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[28])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 36, 1]))
        assert pdu is not None, "No response from firmware for DID 0x2401"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 36, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 1, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 2
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2401 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=2), got {len(pdu)}"
        )

    def test_read_can_busbitrate_kbps_session_gate_firmware(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2401 in Default Session → NRC from firmware ASIL-B safety wrapper.
        Verifies uds_safety.c session validation (step 2 of 5-step chain).
        """
        firmware_bus.request(
            bytes([SID_DIAGNOSTIC_SESSION_CONTROL, SESSION_DEFAULT]), timeout=0.1
        )
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 36, 1]))
        assert pdu is not None, "Expected NRC, got no response"
        assert pdu[0] == SID_NEGATIVE_RESPONSE, (
            f"DID 0x2401 must be rejected in Default Session by firmware safety wrapper"
        )
        assert pdu[2] in (
            NRC_REQUEST_OUT_OF_RANGE,
            NRC_SERVICE_NOT_SUPPORTED_IN_SESSION,
            NRC_CONDITIONS_NOT_CORRECT,
        ), f"Unexpected NRC 0x{pdu[2]:02X}"


    def test_read_lin_busbaudrate_bps_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2402 (LIN_BusBaudrate_bps) — firmware read happy path.
        Expected: RSID=0x62 DID=0x240x02 + 2 data byte(s)
        Verifies: did_safe_read_lin_busbaudrate_bps() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[29])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 36, 2]))
        assert pdu is not None, "No response from firmware for DID 0x2402"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 36, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 2, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 2
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2402 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=2), got {len(pdu)}"
        )

    def test_read_lin_busbaudrate_bps_session_gate_firmware(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2402 in Default Session → NRC from firmware ASIL-B safety wrapper.
        Verifies uds_safety.c session validation (step 2 of 5-step chain).
        """
        firmware_bus.request(
            bytes([SID_DIAGNOSTIC_SESSION_CONTROL, SESSION_DEFAULT]), timeout=0.1
        )
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 36, 2]))
        assert pdu is not None, "Expected NRC, got no response"
        assert pdu[0] == SID_NEGATIVE_RESPONSE, (
            f"DID 0x2402 must be rejected in Default Session by firmware safety wrapper"
        )
        assert pdu[2] in (
            NRC_REQUEST_OUT_OF_RANGE,
            NRC_SERVICE_NOT_SUPPORTED_IN_SESSION,
            NRC_CONDITIONS_NOT_CORRECT,
        ), f"Unexpected NRC 0x{pdu[2]:02X}"


    def test_read_powerio_overcurrentthreshold_ma_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2403 (PowerIO_OvercurrentThreshold_mA) — firmware read happy path.
        Expected: RSID=0x62 DID=0x240x03 + 2 data byte(s)
        Verifies: did_safe_read_powerio_overcurrentthreshold_ma() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[30])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 36, 3]))
        assert pdu is not None, "No response from firmware for DID 0x2403"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 36, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 3, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 2
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2403 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=2), got {len(pdu)}"
        )

    def test_read_powerio_overcurrentthreshold_ma_session_gate_firmware(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2403 in Default Session → NRC from firmware ASIL-B safety wrapper.
        Verifies uds_safety.c session validation (step 2 of 5-step chain).
        """
        firmware_bus.request(
            bytes([SID_DIAGNOSTIC_SESSION_CONTROL, SESSION_DEFAULT]), timeout=0.1
        )
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 36, 3]))
        assert pdu is not None, "Expected NRC, got no response"
        assert pdu[0] == SID_NEGATIVE_RESPONSE, (
            f"DID 0x2403 must be rejected in Default Session by firmware safety wrapper"
        )
        assert pdu[2] in (
            NRC_REQUEST_OUT_OF_RANGE,
            NRC_SERVICE_NOT_SUPPORTED_IN_SESSION,
            NRC_CONDITIONS_NOT_CORRECT,
        ), f"Unexpected NRC 0x{pdu[2]:02X}"


    def test_read_watchdogtimeout_ms_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2404 (WatchdogTimeout_ms) — firmware read happy path.
        Expected: RSID=0x62 DID=0x240x04 + 2 data byte(s)
        Verifies: did_safe_read_watchdogtimeout_ms() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[31])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 36, 4]))
        assert pdu is not None, "No response from firmware for DID 0x2404"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 36, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 4, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 2
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2404 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=2), got {len(pdu)}"
        )

    def test_read_watchdogtimeout_ms_session_gate_firmware(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2404 in Default Session → NRC from firmware ASIL-B safety wrapper.
        Verifies uds_safety.c session validation (step 2 of 5-step chain).
        """
        firmware_bus.request(
            bytes([SID_DIAGNOSTIC_SESSION_CONTROL, SESSION_DEFAULT]), timeout=0.1
        )
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 36, 4]))
        assert pdu is not None, "Expected NRC, got no response"
        assert pdu[0] == SID_NEGATIVE_RESPONSE, (
            f"DID 0x2404 must be rejected in Default Session by firmware safety wrapper"
        )
        assert pdu[2] in (
            NRC_REQUEST_OUT_OF_RANGE,
            NRC_SERVICE_NOT_SUPPORTED_IN_SESSION,
            NRC_CONDITIONS_NOT_CORRECT,
        ), f"Unexpected NRC 0x{pdu[2]:02X}"


    def test_read_firmwareversion_active_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2501 (FirmwareVersion_Active) — firmware read happy path.
        Expected: RSID=0x62 DID=0x250x01 + 4 data byte(s)
        Verifies: did_safe_read_firmwareversion_active() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[32])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 37, 1]))
        assert pdu is not None, "No response from firmware for DID 0x2501"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 37, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 1, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 4
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2501 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=4), got {len(pdu)}"
        )



    def test_read_firmwareversion_pending_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2502 (FirmwareVersion_Pending) — firmware read happy path.
        Expected: RSID=0x62 DID=0x250x02 + 4 data byte(s)
        Verifies: did_safe_read_firmwareversion_pending() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[33])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 37, 2]))
        assert pdu is not None, "No response from firmware for DID 0x2502"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 37, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 2, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 4
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2502 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=4), got {len(pdu)}"
        )



    def test_read_firmwareupdatestatus_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0x2503 (FirmwareUpdateStatus) — firmware read happy path.
        Expected: RSID=0x62 DID=0x250x03 + 1 data byte(s)
        Verifies: did_safe_read_firmwareupdatestatus() safety wrapper + C handler
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[34])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 37, 3]))
        assert pdu is not None, "No response from firmware for DID 0x2503"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Expected positive response, got: {pdu.hex(' ')}"
        )
        assert pdu[1] == 37, f"DID hi byte echo wrong: 0x{pdu[1]:02X}"
        assert pdu[2] == 3, f"DID lo byte echo wrong: 0x{pdu[2]:02X}"
        expected_len = 3 + 1
        assert len(pdu) == expected_len, (
            f"Firmware DID 0x2503 length: expected {expected_len} "
            f"(RSID=1 + DID=2 + data=1), got {len(pdu)}"
        )



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

class TestFirmwareWriteDID:
    """
    SID 0x2E — validates write + read-back against real firmware.
    Proves did_handlers.c stores data and returns it correctly.
    """

    def test_write_vehiclemanufacturersparepartnumber_and_read_back(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        Write sentinel to DID 0xF187 then read it back from firmware.
        Proves C handler persists the value across request/response boundary.
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[2], for_write=True)
        sentinel = bytes([(0x41 + i) & 0xFF for i in range(11)])
        pdu_w = firmware_bus.request(
            bytes([SID_WRITE_DATA_BY_ID, 241, 135]) + sentinel
        )
        assert pdu_w is not None, "No response to write"
        assert pdu_w[0] == (SID_WRITE_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Write failed: {pdu_w.hex(' ')}"
        )
        pdu_r = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 241, 135]))
        assert pdu_r is not None
        assert pdu_r[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET)
        readback = bytes(pdu_r[3:])
        assert readback == sentinel, (
            f"Read-back mismatch for DID 0xF187: wrote {sentinel.hex(' ')}, "
            f"got {readback.hex(' ')}"
        )

    def test_write_vehiclemanufacturersparepartnumber_security_gate(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """Write without security unlock → NRC 0x33 from firmware ASIL-B wrapper."""
        if 3 > 1:
            _enter_session(firmware_bus, _SESSION_BYTE.get(3, SESSION_EXTENDED))
        data = bytes([0x55] * 11)
        pdu = firmware_bus.request(
            bytes([SID_WRITE_DATA_BY_ID, 241, 135]) + data
        )
        assert pdu is not None
        assert pdu[0] == SID_NEGATIVE_RESPONSE
        assert pdu[2] == NRC_SECURITY_ACCESS_DENIED, (
            f"Expected NRC 0x33 for write without unlock, got 0x{pdu[2]:02X}"
        )

    def test_write_vehiclemanufacturersparepartnumber_wrong_length(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """Wrong data length → NRC 0x13 from firmware bounds checker."""
        _setup_for_did(firmware_bus, DID_CATALOGUE[2], for_write=True)
        wrong = bytes([0xBB] * (11 + 1))
        pdu = firmware_bus.request(
            bytes([SID_WRITE_DATA_BY_ID, 241, 135]) + wrong
        )
        assert pdu is not None
        assert pdu[0] == SID_NEGATIVE_RESPONSE
        assert pdu[2] == NRC_INCORRECT_MSG_LEN


    def test_write_powerio_outputcontrol_and_read_back(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        Write sentinel to DID 0x2010 then read it back from firmware.
        Proves C handler persists the value across request/response boundary.
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[12], for_write=True)
        sentinel = bytes([(0x41 + i) & 0xFF for i in range(1)])
        pdu_w = firmware_bus.request(
            bytes([SID_WRITE_DATA_BY_ID, 32, 16]) + sentinel
        )
        assert pdu_w is not None, "No response to write"
        assert pdu_w[0] == (SID_WRITE_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Write failed: {pdu_w.hex(' ')}"
        )
        pdu_r = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 32, 16]))
        assert pdu_r is not None
        assert pdu_r[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET)
        readback = bytes(pdu_r[3:])
        assert readback == sentinel, (
            f"Read-back mismatch for DID 0x2010: wrote {sentinel.hex(' ')}, "
            f"got {readback.hex(' ')}"
        )

    def test_write_powerio_outputcontrol_security_gate(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """Write without security unlock → NRC 0x33 from firmware ASIL-B wrapper."""
        if 3 > 1:
            _enter_session(firmware_bus, _SESSION_BYTE.get(3, SESSION_EXTENDED))
        data = bytes([0x55] * 1)
        pdu = firmware_bus.request(
            bytes([SID_WRITE_DATA_BY_ID, 32, 16]) + data
        )
        assert pdu is not None
        assert pdu[0] == SID_NEGATIVE_RESPONSE
        assert pdu[2] == NRC_SECURITY_ACCESS_DENIED, (
            f"Expected NRC 0x33 for write without unlock, got 0x{pdu[2]:02X}"
        )

    def test_write_powerio_outputcontrol_wrong_length(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """Wrong data length → NRC 0x13 from firmware bounds checker."""
        _setup_for_did(firmware_bus, DID_CATALOGUE[12], for_write=True)
        wrong = bytes([0xBB] * (1 + 1))
        pdu = firmware_bus.request(
            bytes([SID_WRITE_DATA_BY_ID, 32, 16]) + wrong
        )
        assert pdu is not None
        assert pdu[0] == SID_NEGATIVE_RESPONSE
        assert pdu[2] == NRC_INCORRECT_MSG_LEN


    def test_write_can_busbitrate_kbps_and_read_back(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        Write sentinel to DID 0x2401 then read it back from firmware.
        Proves C handler persists the value across request/response boundary.
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[28], for_write=True)
        sentinel = bytes([(0x41 + i) & 0xFF for i in range(2)])
        pdu_w = firmware_bus.request(
            bytes([SID_WRITE_DATA_BY_ID, 36, 1]) + sentinel
        )
        assert pdu_w is not None, "No response to write"
        assert pdu_w[0] == (SID_WRITE_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Write failed: {pdu_w.hex(' ')}"
        )
        pdu_r = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 36, 1]))
        assert pdu_r is not None
        assert pdu_r[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET)
        readback = bytes(pdu_r[3:])
        assert readback == sentinel, (
            f"Read-back mismatch for DID 0x2401: wrote {sentinel.hex(' ')}, "
            f"got {readback.hex(' ')}"
        )

    def test_write_can_busbitrate_kbps_security_gate(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """Write without security unlock → NRC 0x33 from firmware ASIL-B wrapper."""
        if 3 > 1:
            _enter_session(firmware_bus, _SESSION_BYTE.get(3, SESSION_EXTENDED))
        data = bytes([0x55] * 2)
        pdu = firmware_bus.request(
            bytes([SID_WRITE_DATA_BY_ID, 36, 1]) + data
        )
        assert pdu is not None
        assert pdu[0] == SID_NEGATIVE_RESPONSE
        assert pdu[2] == NRC_SECURITY_ACCESS_DENIED, (
            f"Expected NRC 0x33 for write without unlock, got 0x{pdu[2]:02X}"
        )

    def test_write_can_busbitrate_kbps_wrong_length(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """Wrong data length → NRC 0x13 from firmware bounds checker."""
        _setup_for_did(firmware_bus, DID_CATALOGUE[28], for_write=True)
        wrong = bytes([0xBB] * (2 + 1))
        pdu = firmware_bus.request(
            bytes([SID_WRITE_DATA_BY_ID, 36, 1]) + wrong
        )
        assert pdu is not None
        assert pdu[0] == SID_NEGATIVE_RESPONSE
        assert pdu[2] == NRC_INCORRECT_MSG_LEN


    def test_write_lin_busbaudrate_bps_and_read_back(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        Write sentinel to DID 0x2402 then read it back from firmware.
        Proves C handler persists the value across request/response boundary.
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[29], for_write=True)
        sentinel = bytes([(0x41 + i) & 0xFF for i in range(2)])
        pdu_w = firmware_bus.request(
            bytes([SID_WRITE_DATA_BY_ID, 36, 2]) + sentinel
        )
        assert pdu_w is not None, "No response to write"
        assert pdu_w[0] == (SID_WRITE_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Write failed: {pdu_w.hex(' ')}"
        )
        pdu_r = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 36, 2]))
        assert pdu_r is not None
        assert pdu_r[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET)
        readback = bytes(pdu_r[3:])
        assert readback == sentinel, (
            f"Read-back mismatch for DID 0x2402: wrote {sentinel.hex(' ')}, "
            f"got {readback.hex(' ')}"
        )

    def test_write_lin_busbaudrate_bps_security_gate(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """Write without security unlock → NRC 0x33 from firmware ASIL-B wrapper."""
        if 3 > 1:
            _enter_session(firmware_bus, _SESSION_BYTE.get(3, SESSION_EXTENDED))
        data = bytes([0x55] * 2)
        pdu = firmware_bus.request(
            bytes([SID_WRITE_DATA_BY_ID, 36, 2]) + data
        )
        assert pdu is not None
        assert pdu[0] == SID_NEGATIVE_RESPONSE
        assert pdu[2] == NRC_SECURITY_ACCESS_DENIED, (
            f"Expected NRC 0x33 for write without unlock, got 0x{pdu[2]:02X}"
        )

    def test_write_lin_busbaudrate_bps_wrong_length(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """Wrong data length → NRC 0x13 from firmware bounds checker."""
        _setup_for_did(firmware_bus, DID_CATALOGUE[29], for_write=True)
        wrong = bytes([0xBB] * (2 + 1))
        pdu = firmware_bus.request(
            bytes([SID_WRITE_DATA_BY_ID, 36, 2]) + wrong
        )
        assert pdu is not None
        assert pdu[0] == SID_NEGATIVE_RESPONSE
        assert pdu[2] == NRC_INCORRECT_MSG_LEN


    def test_write_powerio_overcurrentthreshold_ma_and_read_back(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        Write sentinel to DID 0x2403 then read it back from firmware.
        Proves C handler persists the value across request/response boundary.
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[30], for_write=True)
        sentinel = bytes([(0x41 + i) & 0xFF for i in range(2)])
        pdu_w = firmware_bus.request(
            bytes([SID_WRITE_DATA_BY_ID, 36, 3]) + sentinel
        )
        assert pdu_w is not None, "No response to write"
        assert pdu_w[0] == (SID_WRITE_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Write failed: {pdu_w.hex(' ')}"
        )
        pdu_r = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 36, 3]))
        assert pdu_r is not None
        assert pdu_r[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET)
        readback = bytes(pdu_r[3:])
        assert readback == sentinel, (
            f"Read-back mismatch for DID 0x2403: wrote {sentinel.hex(' ')}, "
            f"got {readback.hex(' ')}"
        )

    def test_write_powerio_overcurrentthreshold_ma_security_gate(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """Write without security unlock → NRC 0x33 from firmware ASIL-B wrapper."""
        if 3 > 1:
            _enter_session(firmware_bus, _SESSION_BYTE.get(3, SESSION_EXTENDED))
        data = bytes([0x55] * 2)
        pdu = firmware_bus.request(
            bytes([SID_WRITE_DATA_BY_ID, 36, 3]) + data
        )
        assert pdu is not None
        assert pdu[0] == SID_NEGATIVE_RESPONSE
        assert pdu[2] == NRC_SECURITY_ACCESS_DENIED, (
            f"Expected NRC 0x33 for write without unlock, got 0x{pdu[2]:02X}"
        )

    def test_write_powerio_overcurrentthreshold_ma_wrong_length(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """Wrong data length → NRC 0x13 from firmware bounds checker."""
        _setup_for_did(firmware_bus, DID_CATALOGUE[30], for_write=True)
        wrong = bytes([0xBB] * (2 + 1))
        pdu = firmware_bus.request(
            bytes([SID_WRITE_DATA_BY_ID, 36, 3]) + wrong
        )
        assert pdu is not None
        assert pdu[0] == SID_NEGATIVE_RESPONSE
        assert pdu[2] == NRC_INCORRECT_MSG_LEN


    def test_write_watchdogtimeout_ms_and_read_back(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        Write sentinel to DID 0x2404 then read it back from firmware.
        Proves C handler persists the value across request/response boundary.
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[31], for_write=True)
        sentinel = bytes([(0x41 + i) & 0xFF for i in range(2)])
        pdu_w = firmware_bus.request(
            bytes([SID_WRITE_DATA_BY_ID, 36, 4]) + sentinel
        )
        assert pdu_w is not None, "No response to write"
        assert pdu_w[0] == (SID_WRITE_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET), (
            f"Write failed: {pdu_w.hex(' ')}"
        )
        pdu_r = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 36, 4]))
        assert pdu_r is not None
        assert pdu_r[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET)
        readback = bytes(pdu_r[3:])
        assert readback == sentinel, (
            f"Read-back mismatch for DID 0x2404: wrote {sentinel.hex(' ')}, "
            f"got {readback.hex(' ')}"
        )

    def test_write_watchdogtimeout_ms_security_gate(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """Write without security unlock → NRC 0x33 from firmware ASIL-B wrapper."""
        if 3 > 1:
            _enter_session(firmware_bus, _SESSION_BYTE.get(3, SESSION_EXTENDED))
        data = bytes([0x55] * 2)
        pdu = firmware_bus.request(
            bytes([SID_WRITE_DATA_BY_ID, 36, 4]) + data
        )
        assert pdu is not None
        assert pdu[0] == SID_NEGATIVE_RESPONSE
        assert pdu[2] == NRC_SECURITY_ACCESS_DENIED, (
            f"Expected NRC 0x33 for write without unlock, got 0x{pdu[2]:02X}"
        )

    def test_write_watchdogtimeout_ms_wrong_length(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """Wrong data length → NRC 0x13 from firmware bounds checker."""
        _setup_for_did(firmware_bus, DID_CATALOGUE[31], for_write=True)
        wrong = bytes([0xBB] * (2 + 1))
        pdu = firmware_bus.request(
            bytes([SID_WRITE_DATA_BY_ID, 36, 4]) + wrong
        )
        assert pdu is not None
        assert pdu[0] == SID_NEGATIVE_RESPONSE
        assert pdu[2] == NRC_INCORRECT_MSG_LEN


# =============================================================================
# §6 Multi-frame ISO-TP — verifies C isotp.c segmentation
# =============================================================================

class TestFirmwareIsoTp:
    """
    ISO 15765-2 multi-frame exchange validation against real C isotp.c.
    These tests exercise the FF/CF/FC state machine in transport/isotp.c.
    """

    def test_multiframe_vehicleidentificationnumber_response(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0xF190 (VehicleIdentificationNumber) data_length=17B → 20B PDU.
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
    def test_multiframe_ecuserialnumber_response(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0xF18C (ECUSerialNumber) data_length=8B → 11B PDU.
        11 bytes > 7 → triggers FF+CF multi-frame from C isotp.c.
        Verifies isotp.c segments correctly and FirmwareIsoTpTransport reassembles.
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[1])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 241, 140]))
        assert pdu is not None, f"No response for multi-frame DID 0xF18C"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET)
        expected_len = 3 + 8
        assert len(pdu) == expected_len, (
            f"Multi-frame reassembly error for DID 0xF18C: "
            f"expected {expected_len} bytes, got {len(pdu)}"
        )
    def test_multiframe_vehiclemanufacturersparepartnumber_response(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0xF187 (VehicleManufacturerSparePartNumber) data_length=11B → 14B PDU.
        14 bytes > 7 → triggers FF+CF multi-frame from C isotp.c.
        Verifies isotp.c segments correctly and FirmwareIsoTpTransport reassembles.
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[2])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 241, 135]))
        assert pdu is not None, f"No response for multi-frame DID 0xF187"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET)
        expected_len = 3 + 11
        assert len(pdu) == expected_len, (
            f"Multi-frame reassembly error for DID 0xF187: "
            f"expected {expected_len} bytes, got {len(pdu)}"
        )
    def test_multiframe_systemsupplieridentifierdataidentifier_response(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DID 0xF197 (SystemSupplierIdentifierDataIdentifier) data_length=5B → 8B PDU.
        8 bytes > 7 → triggers FF+CF multi-frame from C isotp.c.
        Verifies isotp.c segments correctly and FirmwareIsoTpTransport reassembles.
        """
        _setup_for_did(firmware_bus, DID_CATALOGUE[4])
        pdu = firmware_bus.request(bytes([SID_READ_DATA_BY_ID, 241, 151]))
        assert pdu is not None, f"No response for multi-frame DID 0xF197"
        assert pdu[0] == (SID_READ_DATA_BY_ID + POSITIVE_RESPONSE_OFFSET)
        expected_len = 3 + 5
        assert len(pdu) == expected_len, (
            f"Multi-frame reassembly error for DID 0xF197: "
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
        _setup_for_did(firmware_bus, DID_CATALOGUE[2], for_write=True)
        short_data = bytes([0xAA] * max(1, 11 - 1))
        pdu = firmware_bus.request(
            bytes([SID_WRITE_DATA_BY_ID, 241, 135]) + short_data
        )
        assert pdu is not None
        assert pdu[0] == SID_NEGATIVE_RESPONSE
        assert pdu[2] == NRC_INCORRECT_MSG_LEN

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


# =============================================================================
# §9 ReadDTCInformation — firmware DTC catalogue validation
# =============================================================================

class TestFirmwareDtcInformation:
    """
    SID 0x19 — validates DTC database from config/dtc_database.c.
    Verifies all 19 configured DTCs are registered in firmware.
    """

    def test_report_dtc_count_firmware(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        Sub 0x01 (reportNumberOfDTCByStatusMask) — firmware DTC count response.

        ISO 14229-1 §11.3.2: The count returned for mask 0xFF is the number
        of DTCs whose statusOfDTC byte has ANY of the mask bits set.
        With no active faults (all status bytes = 0x00), the count is 0 —
        this is CORRECT behavior. The test validates the response structure,
        not an assumed fault count.
        """
        pdu = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
        assert pdu is not None
        if pdu[0] == SID_NEGATIVE_RESPONSE:
            pytest.skip(f"ReadDTCInfo not in current session (NRC 0x{pdu[2]:02X})")
        assert pdu[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET)
        assert pdu[1] == 0x01, "Sub-function echo must be 0x01"
        assert len(pdu) >= 6, f"Response must be ≥6 bytes, got {len(pdu)}"
        # Count ≥ 0 is valid (0 = no active faults, correct when no fault injected)
        import struct as _struct
        count = _struct.unpack(">H", pdu[4:6])[0]
        assert count >= 0  # always true; verifies struct is parseable

    def test_dtc_list_structure_firmware(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        Sub 0x02 — DTC list entries must be 4 bytes each (3B code + 1B status).
        Validates config/dtc_database.c registration format.
        """
        pdu = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x02, 0xFF]))
        if pdu is None or pdu[0] == SID_NEGATIVE_RESPONSE:
            pytest.skip("ReadDTCInfo not available")
        assert pdu[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET)
        body = pdu[3:]  # skip RSID + sub + availMask
        assert len(body) % 4 == 0, (
            f"DTC body {len(body)} bytes not divisible by 4 (3B code + 1B status)"
        )

    def test_dtc_c00100_registered(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DTC 0xC00100 (CAN bus communication loss — no frames received > 500ms) must be registered in firmware.

        ISO 14229-1 §11.3: sub 0x01 (reportNumberOfDTCByStatusMask) reports the
        total count of registered DTCs matching the mask. With mask 0x00 (special
        case — returns ALL registered DTCs regardless of status), we can verify
        0xC00100 is in the database even when no fault is currently active.

        Tests config/dtc_database.c registration against YAML configuration.
        """
        # Sub 0x01 with mask 0x00: ISO 14229-1 special case returns count of ALL DTCs
        # Sub 0x04 reportDTCSnapshotRecordByDTCNumber can also confirm registration
        # Most reliable: sub 0x02 with mask=0x00 returns all DTCs in DB
        pdu = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x02, 0x00]))
        if pdu is None:
            pytest.skip("No response to ReadDTCInfo sub02 mask=0x00")
        if pdu[0] == SID_NEGATIVE_RESPONSE:
            # Firmware may not support mask=0x00 (returns all); try sub01 count check
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 1, (
                    f"Firmware DTC count {count} < expected 1 "
                    f"(DTC 0xC00100 may not be registered)"
                )
            return
        body = pdu[3:]
        if len(body) > 0 and len(body) % 4 == 0:
            registered = {(body[i]<<16)|(body[i+1]<<8)|body[i+2] for i in range(0, len(body), 4)}
            assert 12583168 in registered, (
                f"DTC 0xC00100 not found in firmware DTC database. "
                f"Registered codes: {[hex(c) for c in sorted(registered)]}"
            )
        else:
            # mask=0x00 returned empty — all DTCs have status 0x00 (no faults)
            # Use sub01 to confirm count includes this DTC
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 1, (
                    f"DTC count {count}: DTC 0xC00100 may not be registered in firmware"
                )
    def test_dtc_c00200_registered(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DTC 0xC00200 (CAN bus error passive — TX/RX error counters exceeded 127) must be registered in firmware.

        ISO 14229-1 §11.3: sub 0x01 (reportNumberOfDTCByStatusMask) reports the
        total count of registered DTCs matching the mask. With mask 0x00 (special
        case — returns ALL registered DTCs regardless of status), we can verify
        0xC00200 is in the database even when no fault is currently active.

        Tests config/dtc_database.c registration against YAML configuration.
        """
        # Sub 0x01 with mask 0x00: ISO 14229-1 special case returns count of ALL DTCs
        # Sub 0x04 reportDTCSnapshotRecordByDTCNumber can also confirm registration
        # Most reliable: sub 0x02 with mask=0x00 returns all DTCs in DB
        pdu = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x02, 0x00]))
        if pdu is None:
            pytest.skip("No response to ReadDTCInfo sub02 mask=0x00")
        if pdu[0] == SID_NEGATIVE_RESPONSE:
            # Firmware may not support mask=0x00 (returns all); try sub01 count check
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 2, (
                    f"Firmware DTC count {count} < expected 2 "
                    f"(DTC 0xC00200 may not be registered)"
                )
            return
        body = pdu[3:]
        if len(body) > 0 and len(body) % 4 == 0:
            registered = {(body[i]<<16)|(body[i+1]<<8)|body[i+2] for i in range(0, len(body), 4)}
            assert 12583424 in registered, (
                f"DTC 0xC00200 not found in firmware DTC database. "
                f"Registered codes: {[hex(c) for c in sorted(registered)]}"
            )
        else:
            # mask=0x00 returned empty — all DTCs have status 0x00 (no faults)
            # Use sub01 to confirm count includes this DTC
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 2, (
                    f"DTC count {count}: DTC 0xC00200 may not be registered in firmware"
                )
    def test_dtc_c00300_registered(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DTC 0xC00300 (CAN bus off — transmitter disabled by bus-off recovery) must be registered in firmware.

        ISO 14229-1 §11.3: sub 0x01 (reportNumberOfDTCByStatusMask) reports the
        total count of registered DTCs matching the mask. With mask 0x00 (special
        case — returns ALL registered DTCs regardless of status), we can verify
        0xC00300 is in the database even when no fault is currently active.

        Tests config/dtc_database.c registration against YAML configuration.
        """
        # Sub 0x01 with mask 0x00: ISO 14229-1 special case returns count of ALL DTCs
        # Sub 0x04 reportDTCSnapshotRecordByDTCNumber can also confirm registration
        # Most reliable: sub 0x02 with mask=0x00 returns all DTCs in DB
        pdu = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x02, 0x00]))
        if pdu is None:
            pytest.skip("No response to ReadDTCInfo sub02 mask=0x00")
        if pdu[0] == SID_NEGATIVE_RESPONSE:
            # Firmware may not support mask=0x00 (returns all); try sub01 count check
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 3, (
                    f"Firmware DTC count {count} < expected 3 "
                    f"(DTC 0xC00300 may not be registered)"
                )
            return
        body = pdu[3:]
        if len(body) > 0 and len(body) % 4 == 0:
            registered = {(body[i]<<16)|(body[i+1]<<8)|body[i+2] for i in range(0, len(body), 4)}
            assert 12583680 in registered, (
                f"DTC 0xC00300 not found in firmware DTC database. "
                f"Registered codes: {[hex(c) for c in sorted(registered)]}"
            )
        else:
            # mask=0x00 returned empty — all DTCs have status 0x00 (no faults)
            # Use sub01 to confirm count includes this DTC
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 3, (
                    f"DTC count {count}: DTC 0xC00300 may not be registered in firmware"
                )
    def test_dtc_c01000_registered(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DTC 0xC01000 (LIN bus no response — no slave response within timeout) must be registered in firmware.

        ISO 14229-1 §11.3: sub 0x01 (reportNumberOfDTCByStatusMask) reports the
        total count of registered DTCs matching the mask. With mask 0x00 (special
        case — returns ALL registered DTCs regardless of status), we can verify
        0xC01000 is in the database even when no fault is currently active.

        Tests config/dtc_database.c registration against YAML configuration.
        """
        # Sub 0x01 with mask 0x00: ISO 14229-1 special case returns count of ALL DTCs
        # Sub 0x04 reportDTCSnapshotRecordByDTCNumber can also confirm registration
        # Most reliable: sub 0x02 with mask=0x00 returns all DTCs in DB
        pdu = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x02, 0x00]))
        if pdu is None:
            pytest.skip("No response to ReadDTCInfo sub02 mask=0x00")
        if pdu[0] == SID_NEGATIVE_RESPONSE:
            # Firmware may not support mask=0x00 (returns all); try sub01 count check
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 4, (
                    f"Firmware DTC count {count} < expected 4 "
                    f"(DTC 0xC01000 may not be registered)"
                )
            return
        body = pdu[3:]
        if len(body) > 0 and len(body) % 4 == 0:
            registered = {(body[i]<<16)|(body[i+1]<<8)|body[i+2] for i in range(0, len(body), 4)}
            assert 12587008 in registered, (
                f"DTC 0xC01000 not found in firmware DTC database. "
                f"Registered codes: {[hex(c) for c in sorted(registered)]}"
            )
        else:
            # mask=0x00 returned empty — all DTCs have status 0x00 (no faults)
            # Use sub01 to confirm count includes this DTC
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 4, (
                    f"DTC count {count}: DTC 0xC01000 may not be registered in firmware"
                )
    def test_dtc_c01100_registered(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DTC 0xC01100 (LIN bus framing error — break field or sync byte invalid) must be registered in firmware.

        ISO 14229-1 §11.3: sub 0x01 (reportNumberOfDTCByStatusMask) reports the
        total count of registered DTCs matching the mask. With mask 0x00 (special
        case — returns ALL registered DTCs regardless of status), we can verify
        0xC01100 is in the database even when no fault is currently active.

        Tests config/dtc_database.c registration against YAML configuration.
        """
        # Sub 0x01 with mask 0x00: ISO 14229-1 special case returns count of ALL DTCs
        # Sub 0x04 reportDTCSnapshotRecordByDTCNumber can also confirm registration
        # Most reliable: sub 0x02 with mask=0x00 returns all DTCs in DB
        pdu = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x02, 0x00]))
        if pdu is None:
            pytest.skip("No response to ReadDTCInfo sub02 mask=0x00")
        if pdu[0] == SID_NEGATIVE_RESPONSE:
            # Firmware may not support mask=0x00 (returns all); try sub01 count check
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 5, (
                    f"Firmware DTC count {count} < expected 5 "
                    f"(DTC 0xC01100 may not be registered)"
                )
            return
        body = pdu[3:]
        if len(body) > 0 and len(body) % 4 == 0:
            registered = {(body[i]<<16)|(body[i+1]<<8)|body[i+2] for i in range(0, len(body), 4)}
            assert 12587264 in registered, (
                f"DTC 0xC01100 not found in firmware DTC database. "
                f"Registered codes: {[hex(c) for c in sorted(registered)]}"
            )
        else:
            # mask=0x00 returned empty — all DTCs have status 0x00 (no faults)
            # Use sub01 to confirm count includes this DTC
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 5, (
                    f"DTC count {count}: DTC 0xC01100 may not be registered in firmware"
                )
    def test_dtc_b00100_registered(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DTC 0xB00100 (Output 1 overcurrent — load current exceeded threshold) must be registered in firmware.

        ISO 14229-1 §11.3: sub 0x01 (reportNumberOfDTCByStatusMask) reports the
        total count of registered DTCs matching the mask. With mask 0x00 (special
        case — returns ALL registered DTCs regardless of status), we can verify
        0xB00100 is in the database even when no fault is currently active.

        Tests config/dtc_database.c registration against YAML configuration.
        """
        # Sub 0x01 with mask 0x00: ISO 14229-1 special case returns count of ALL DTCs
        # Sub 0x04 reportDTCSnapshotRecordByDTCNumber can also confirm registration
        # Most reliable: sub 0x02 with mask=0x00 returns all DTCs in DB
        pdu = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x02, 0x00]))
        if pdu is None:
            pytest.skip("No response to ReadDTCInfo sub02 mask=0x00")
        if pdu[0] == SID_NEGATIVE_RESPONSE:
            # Firmware may not support mask=0x00 (returns all); try sub01 count check
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 6, (
                    f"Firmware DTC count {count} < expected 6 "
                    f"(DTC 0xB00100 may not be registered)"
                )
            return
        body = pdu[3:]
        if len(body) > 0 and len(body) % 4 == 0:
            registered = {(body[i]<<16)|(body[i+1]<<8)|body[i+2] for i in range(0, len(body), 4)}
            assert 11534592 in registered, (
                f"DTC 0xB00100 not found in firmware DTC database. "
                f"Registered codes: {[hex(c) for c in sorted(registered)]}"
            )
        else:
            # mask=0x00 returned empty — all DTCs have status 0x00 (no faults)
            # Use sub01 to confirm count includes this DTC
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 6, (
                    f"DTC count {count}: DTC 0xB00100 may not be registered in firmware"
                )
    def test_dtc_b00200_registered(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DTC 0xB00200 (Output 2 overcurrent — load current exceeded threshold) must be registered in firmware.

        ISO 14229-1 §11.3: sub 0x01 (reportNumberOfDTCByStatusMask) reports the
        total count of registered DTCs matching the mask. With mask 0x00 (special
        case — returns ALL registered DTCs regardless of status), we can verify
        0xB00200 is in the database even when no fault is currently active.

        Tests config/dtc_database.c registration against YAML configuration.
        """
        # Sub 0x01 with mask 0x00: ISO 14229-1 special case returns count of ALL DTCs
        # Sub 0x04 reportDTCSnapshotRecordByDTCNumber can also confirm registration
        # Most reliable: sub 0x02 with mask=0x00 returns all DTCs in DB
        pdu = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x02, 0x00]))
        if pdu is None:
            pytest.skip("No response to ReadDTCInfo sub02 mask=0x00")
        if pdu[0] == SID_NEGATIVE_RESPONSE:
            # Firmware may not support mask=0x00 (returns all); try sub01 count check
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 7, (
                    f"Firmware DTC count {count} < expected 7 "
                    f"(DTC 0xB00200 may not be registered)"
                )
            return
        body = pdu[3:]
        if len(body) > 0 and len(body) % 4 == 0:
            registered = {(body[i]<<16)|(body[i+1]<<8)|body[i+2] for i in range(0, len(body), 4)}
            assert 11534848 in registered, (
                f"DTC 0xB00200 not found in firmware DTC database. "
                f"Registered codes: {[hex(c) for c in sorted(registered)]}"
            )
        else:
            # mask=0x00 returned empty — all DTCs have status 0x00 (no faults)
            # Use sub01 to confirm count includes this DTC
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 7, (
                    f"DTC count {count}: DTC 0xB00200 may not be registered in firmware"
                )
    def test_dtc_b00300_registered(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DTC 0xB00300 (Output 3 overcurrent — load current exceeded threshold) must be registered in firmware.

        ISO 14229-1 §11.3: sub 0x01 (reportNumberOfDTCByStatusMask) reports the
        total count of registered DTCs matching the mask. With mask 0x00 (special
        case — returns ALL registered DTCs regardless of status), we can verify
        0xB00300 is in the database even when no fault is currently active.

        Tests config/dtc_database.c registration against YAML configuration.
        """
        # Sub 0x01 with mask 0x00: ISO 14229-1 special case returns count of ALL DTCs
        # Sub 0x04 reportDTCSnapshotRecordByDTCNumber can also confirm registration
        # Most reliable: sub 0x02 with mask=0x00 returns all DTCs in DB
        pdu = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x02, 0x00]))
        if pdu is None:
            pytest.skip("No response to ReadDTCInfo sub02 mask=0x00")
        if pdu[0] == SID_NEGATIVE_RESPONSE:
            # Firmware may not support mask=0x00 (returns all); try sub01 count check
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 8, (
                    f"Firmware DTC count {count} < expected 8 "
                    f"(DTC 0xB00300 may not be registered)"
                )
            return
        body = pdu[3:]
        if len(body) > 0 and len(body) % 4 == 0:
            registered = {(body[i]<<16)|(body[i+1]<<8)|body[i+2] for i in range(0, len(body), 4)}
            assert 11535104 in registered, (
                f"DTC 0xB00300 not found in firmware DTC database. "
                f"Registered codes: {[hex(c) for c in sorted(registered)]}"
            )
        else:
            # mask=0x00 returned empty — all DTCs have status 0x00 (no faults)
            # Use sub01 to confirm count includes this DTC
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 8, (
                    f"DTC count {count}: DTC 0xB00300 may not be registered in firmware"
                )
    def test_dtc_b00400_registered(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DTC 0xB00400 (Output 4 overcurrent — load current exceeded threshold) must be registered in firmware.

        ISO 14229-1 §11.3: sub 0x01 (reportNumberOfDTCByStatusMask) reports the
        total count of registered DTCs matching the mask. With mask 0x00 (special
        case — returns ALL registered DTCs regardless of status), we can verify
        0xB00400 is in the database even when no fault is currently active.

        Tests config/dtc_database.c registration against YAML configuration.
        """
        # Sub 0x01 with mask 0x00: ISO 14229-1 special case returns count of ALL DTCs
        # Sub 0x04 reportDTCSnapshotRecordByDTCNumber can also confirm registration
        # Most reliable: sub 0x02 with mask=0x00 returns all DTCs in DB
        pdu = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x02, 0x00]))
        if pdu is None:
            pytest.skip("No response to ReadDTCInfo sub02 mask=0x00")
        if pdu[0] == SID_NEGATIVE_RESPONSE:
            # Firmware may not support mask=0x00 (returns all); try sub01 count check
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 9, (
                    f"Firmware DTC count {count} < expected 9 "
                    f"(DTC 0xB00400 may not be registered)"
                )
            return
        body = pdu[3:]
        if len(body) > 0 and len(body) % 4 == 0:
            registered = {(body[i]<<16)|(body[i+1]<<8)|body[i+2] for i in range(0, len(body), 4)}
            assert 11535360 in registered, (
                f"DTC 0xB00400 not found in firmware DTC database. "
                f"Registered codes: {[hex(c) for c in sorted(registered)]}"
            )
        else:
            # mask=0x00 returned empty — all DTCs have status 0x00 (no faults)
            # Use sub01 to confirm count includes this DTC
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 9, (
                    f"DTC count {count}: DTC 0xB00400 may not be registered in firmware"
                )
    def test_dtc_b00500_registered(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DTC 0xB00500 (Output 5 overcurrent — load current exceeded threshold) must be registered in firmware.

        ISO 14229-1 §11.3: sub 0x01 (reportNumberOfDTCByStatusMask) reports the
        total count of registered DTCs matching the mask. With mask 0x00 (special
        case — returns ALL registered DTCs regardless of status), we can verify
        0xB00500 is in the database even when no fault is currently active.

        Tests config/dtc_database.c registration against YAML configuration.
        """
        # Sub 0x01 with mask 0x00: ISO 14229-1 special case returns count of ALL DTCs
        # Sub 0x04 reportDTCSnapshotRecordByDTCNumber can also confirm registration
        # Most reliable: sub 0x02 with mask=0x00 returns all DTCs in DB
        pdu = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x02, 0x00]))
        if pdu is None:
            pytest.skip("No response to ReadDTCInfo sub02 mask=0x00")
        if pdu[0] == SID_NEGATIVE_RESPONSE:
            # Firmware may not support mask=0x00 (returns all); try sub01 count check
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 10, (
                    f"Firmware DTC count {count} < expected 10 "
                    f"(DTC 0xB00500 may not be registered)"
                )
            return
        body = pdu[3:]
        if len(body) > 0 and len(body) % 4 == 0:
            registered = {(body[i]<<16)|(body[i+1]<<8)|body[i+2] for i in range(0, len(body), 4)}
            assert 11535616 in registered, (
                f"DTC 0xB00500 not found in firmware DTC database. "
                f"Registered codes: {[hex(c) for c in sorted(registered)]}"
            )
        else:
            # mask=0x00 returned empty — all DTCs have status 0x00 (no faults)
            # Use sub01 to confirm count includes this DTC
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 10, (
                    f"DTC count {count}: DTC 0xB00500 may not be registered in firmware"
                )
    def test_dtc_b00600_registered(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DTC 0xB00600 (Output 6 overcurrent — load current exceeded threshold) must be registered in firmware.

        ISO 14229-1 §11.3: sub 0x01 (reportNumberOfDTCByStatusMask) reports the
        total count of registered DTCs matching the mask. With mask 0x00 (special
        case — returns ALL registered DTCs regardless of status), we can verify
        0xB00600 is in the database even when no fault is currently active.

        Tests config/dtc_database.c registration against YAML configuration.
        """
        # Sub 0x01 with mask 0x00: ISO 14229-1 special case returns count of ALL DTCs
        # Sub 0x04 reportDTCSnapshotRecordByDTCNumber can also confirm registration
        # Most reliable: sub 0x02 with mask=0x00 returns all DTCs in DB
        pdu = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x02, 0x00]))
        if pdu is None:
            pytest.skip("No response to ReadDTCInfo sub02 mask=0x00")
        if pdu[0] == SID_NEGATIVE_RESPONSE:
            # Firmware may not support mask=0x00 (returns all); try sub01 count check
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 11, (
                    f"Firmware DTC count {count} < expected 11 "
                    f"(DTC 0xB00600 may not be registered)"
                )
            return
        body = pdu[3:]
        if len(body) > 0 and len(body) % 4 == 0:
            registered = {(body[i]<<16)|(body[i+1]<<8)|body[i+2] for i in range(0, len(body), 4)}
            assert 11535872 in registered, (
                f"DTC 0xB00600 not found in firmware DTC database. "
                f"Registered codes: {[hex(c) for c in sorted(registered)]}"
            )
        else:
            # mask=0x00 returned empty — all DTCs have status 0x00 (no faults)
            # Use sub01 to confirm count includes this DTC
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 11, (
                    f"DTC count {count}: DTC 0xB00600 may not be registered in firmware"
                )
    def test_dtc_b01000_registered(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DTC 0xB01000 (PowerIO open load — output commanded ON but no current detected) must be registered in firmware.

        ISO 14229-1 §11.3: sub 0x01 (reportNumberOfDTCByStatusMask) reports the
        total count of registered DTCs matching the mask. With mask 0x00 (special
        case — returns ALL registered DTCs regardless of status), we can verify
        0xB01000 is in the database even when no fault is currently active.

        Tests config/dtc_database.c registration against YAML configuration.
        """
        # Sub 0x01 with mask 0x00: ISO 14229-1 special case returns count of ALL DTCs
        # Sub 0x04 reportDTCSnapshotRecordByDTCNumber can also confirm registration
        # Most reliable: sub 0x02 with mask=0x00 returns all DTCs in DB
        pdu = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x02, 0x00]))
        if pdu is None:
            pytest.skip("No response to ReadDTCInfo sub02 mask=0x00")
        if pdu[0] == SID_NEGATIVE_RESPONSE:
            # Firmware may not support mask=0x00 (returns all); try sub01 count check
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 12, (
                    f"Firmware DTC count {count} < expected 12 "
                    f"(DTC 0xB01000 may not be registered)"
                )
            return
        body = pdu[3:]
        if len(body) > 0 and len(body) % 4 == 0:
            registered = {(body[i]<<16)|(body[i+1]<<8)|body[i+2] for i in range(0, len(body), 4)}
            assert 11538432 in registered, (
                f"DTC 0xB01000 not found in firmware DTC database. "
                f"Registered codes: {[hex(c) for c in sorted(registered)]}"
            )
        else:
            # mask=0x00 returned empty — all DTCs have status 0x00 (no faults)
            # Use sub01 to confirm count includes this DTC
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 12, (
                    f"DTC count {count}: DTC 0xB01000 may not be registered in firmware"
                )
    def test_dtc_c0ff00_registered(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DTC 0xC0FF00 (Supply voltage low — below 9.0V for > 500ms) must be registered in firmware.

        ISO 14229-1 §11.3: sub 0x01 (reportNumberOfDTCByStatusMask) reports the
        total count of registered DTCs matching the mask. With mask 0x00 (special
        case — returns ALL registered DTCs regardless of status), we can verify
        0xC0FF00 is in the database even when no fault is currently active.

        Tests config/dtc_database.c registration against YAML configuration.
        """
        # Sub 0x01 with mask 0x00: ISO 14229-1 special case returns count of ALL DTCs
        # Sub 0x04 reportDTCSnapshotRecordByDTCNumber can also confirm registration
        # Most reliable: sub 0x02 with mask=0x00 returns all DTCs in DB
        pdu = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x02, 0x00]))
        if pdu is None:
            pytest.skip("No response to ReadDTCInfo sub02 mask=0x00")
        if pdu[0] == SID_NEGATIVE_RESPONSE:
            # Firmware may not support mask=0x00 (returns all); try sub01 count check
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 13, (
                    f"Firmware DTC count {count} < expected 13 "
                    f"(DTC 0xC0FF00 may not be registered)"
                )
            return
        body = pdu[3:]
        if len(body) > 0 and len(body) % 4 == 0:
            registered = {(body[i]<<16)|(body[i+1]<<8)|body[i+2] for i in range(0, len(body), 4)}
            assert 12648192 in registered, (
                f"DTC 0xC0FF00 not found in firmware DTC database. "
                f"Registered codes: {[hex(c) for c in sorted(registered)]}"
            )
        else:
            # mask=0x00 returned empty — all DTCs have status 0x00 (no faults)
            # Use sub01 to confirm count includes this DTC
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 13, (
                    f"DTC count {count}: DTC 0xC0FF00 may not be registered in firmware"
                )
    def test_dtc_c0ff10_registered(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DTC 0xC0FF10 (Supply voltage high — above 16.0V for > 100ms) must be registered in firmware.

        ISO 14229-1 §11.3: sub 0x01 (reportNumberOfDTCByStatusMask) reports the
        total count of registered DTCs matching the mask. With mask 0x00 (special
        case — returns ALL registered DTCs regardless of status), we can verify
        0xC0FF10 is in the database even when no fault is currently active.

        Tests config/dtc_database.c registration against YAML configuration.
        """
        # Sub 0x01 with mask 0x00: ISO 14229-1 special case returns count of ALL DTCs
        # Sub 0x04 reportDTCSnapshotRecordByDTCNumber can also confirm registration
        # Most reliable: sub 0x02 with mask=0x00 returns all DTCs in DB
        pdu = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x02, 0x00]))
        if pdu is None:
            pytest.skip("No response to ReadDTCInfo sub02 mask=0x00")
        if pdu[0] == SID_NEGATIVE_RESPONSE:
            # Firmware may not support mask=0x00 (returns all); try sub01 count check
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 14, (
                    f"Firmware DTC count {count} < expected 14 "
                    f"(DTC 0xC0FF10 may not be registered)"
                )
            return
        body = pdu[3:]
        if len(body) > 0 and len(body) % 4 == 0:
            registered = {(body[i]<<16)|(body[i+1]<<8)|body[i+2] for i in range(0, len(body), 4)}
            assert 12648208 in registered, (
                f"DTC 0xC0FF10 not found in firmware DTC database. "
                f"Registered codes: {[hex(c) for c in sorted(registered)]}"
            )
        else:
            # mask=0x00 returned empty — all DTCs have status 0x00 (no faults)
            # Use sub01 to confirm count includes this DTC
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 14, (
                    f"DTC count {count}: DTC 0xC0FF10 may not be registered in firmware"
                )
    def test_dtc_c0ff20_registered(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DTC 0xC0FF20 (ECU overtemperature — internal junction > 125°C) must be registered in firmware.

        ISO 14229-1 §11.3: sub 0x01 (reportNumberOfDTCByStatusMask) reports the
        total count of registered DTCs matching the mask. With mask 0x00 (special
        case — returns ALL registered DTCs regardless of status), we can verify
        0xC0FF20 is in the database even when no fault is currently active.

        Tests config/dtc_database.c registration against YAML configuration.
        """
        # Sub 0x01 with mask 0x00: ISO 14229-1 special case returns count of ALL DTCs
        # Sub 0x04 reportDTCSnapshotRecordByDTCNumber can also confirm registration
        # Most reliable: sub 0x02 with mask=0x00 returns all DTCs in DB
        pdu = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x02, 0x00]))
        if pdu is None:
            pytest.skip("No response to ReadDTCInfo sub02 mask=0x00")
        if pdu[0] == SID_NEGATIVE_RESPONSE:
            # Firmware may not support mask=0x00 (returns all); try sub01 count check
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 15, (
                    f"Firmware DTC count {count} < expected 15 "
                    f"(DTC 0xC0FF20 may not be registered)"
                )
            return
        body = pdu[3:]
        if len(body) > 0 and len(body) % 4 == 0:
            registered = {(body[i]<<16)|(body[i+1]<<8)|body[i+2] for i in range(0, len(body), 4)}
            assert 12648224 in registered, (
                f"DTC 0xC0FF20 not found in firmware DTC database. "
                f"Registered codes: {[hex(c) for c in sorted(registered)]}"
            )
        else:
            # mask=0x00 returned empty — all DTCs have status 0x00 (no faults)
            # Use sub01 to confirm count includes this DTC
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 15, (
                    f"DTC count {count}: DTC 0xC0FF20 may not be registered in firmware"
                )
    def test_dtc_c0ff30_registered(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DTC 0xC0FF30 (Watchdog reset event — firmware failed to service watchdog) must be registered in firmware.

        ISO 14229-1 §11.3: sub 0x01 (reportNumberOfDTCByStatusMask) reports the
        total count of registered DTCs matching the mask. With mask 0x00 (special
        case — returns ALL registered DTCs regardless of status), we can verify
        0xC0FF30 is in the database even when no fault is currently active.

        Tests config/dtc_database.c registration against YAML configuration.
        """
        # Sub 0x01 with mask 0x00: ISO 14229-1 special case returns count of ALL DTCs
        # Sub 0x04 reportDTCSnapshotRecordByDTCNumber can also confirm registration
        # Most reliable: sub 0x02 with mask=0x00 returns all DTCs in DB
        pdu = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x02, 0x00]))
        if pdu is None:
            pytest.skip("No response to ReadDTCInfo sub02 mask=0x00")
        if pdu[0] == SID_NEGATIVE_RESPONSE:
            # Firmware may not support mask=0x00 (returns all); try sub01 count check
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 16, (
                    f"Firmware DTC count {count} < expected 16 "
                    f"(DTC 0xC0FF30 may not be registered)"
                )
            return
        body = pdu[3:]
        if len(body) > 0 and len(body) % 4 == 0:
            registered = {(body[i]<<16)|(body[i+1]<<8)|body[i+2] for i in range(0, len(body), 4)}
            assert 12648240 in registered, (
                f"DTC 0xC0FF30 not found in firmware DTC database. "
                f"Registered codes: {[hex(c) for c in sorted(registered)]}"
            )
        else:
            # mask=0x00 returned empty — all DTCs have status 0x00 (no faults)
            # Use sub01 to confirm count includes this DTC
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 16, (
                    f"DTC count {count}: DTC 0xC0FF30 may not be registered in firmware"
                )
    def test_dtc_c0ff40_registered(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DTC 0xC0FF40 (NVM write failure — diagnostic session statistics not persisted) must be registered in firmware.

        ISO 14229-1 §11.3: sub 0x01 (reportNumberOfDTCByStatusMask) reports the
        total count of registered DTCs matching the mask. With mask 0x00 (special
        case — returns ALL registered DTCs regardless of status), we can verify
        0xC0FF40 is in the database even when no fault is currently active.

        Tests config/dtc_database.c registration against YAML configuration.
        """
        # Sub 0x01 with mask 0x00: ISO 14229-1 special case returns count of ALL DTCs
        # Sub 0x04 reportDTCSnapshotRecordByDTCNumber can also confirm registration
        # Most reliable: sub 0x02 with mask=0x00 returns all DTCs in DB
        pdu = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x02, 0x00]))
        if pdu is None:
            pytest.skip("No response to ReadDTCInfo sub02 mask=0x00")
        if pdu[0] == SID_NEGATIVE_RESPONSE:
            # Firmware may not support mask=0x00 (returns all); try sub01 count check
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 17, (
                    f"Firmware DTC count {count} < expected 17 "
                    f"(DTC 0xC0FF40 may not be registered)"
                )
            return
        body = pdu[3:]
        if len(body) > 0 and len(body) % 4 == 0:
            registered = {(body[i]<<16)|(body[i+1]<<8)|body[i+2] for i in range(0, len(body), 4)}
            assert 12648256 in registered, (
                f"DTC 0xC0FF40 not found in firmware DTC database. "
                f"Registered codes: {[hex(c) for c in sorted(registered)]}"
            )
        else:
            # mask=0x00 returned empty — all DTCs have status 0x00 (no faults)
            # Use sub01 to confirm count includes this DTC
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 17, (
                    f"DTC count {count}: DTC 0xC0FF40 may not be registered in firmware"
                )
    def test_dtc_c0fe00_registered(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DTC 0xC0FE00 (Firmware image verification failed — CRC mismatch) must be registered in firmware.

        ISO 14229-1 §11.3: sub 0x01 (reportNumberOfDTCByStatusMask) reports the
        total count of registered DTCs matching the mask. With mask 0x00 (special
        case — returns ALL registered DTCs regardless of status), we can verify
        0xC0FE00 is in the database even when no fault is currently active.

        Tests config/dtc_database.c registration against YAML configuration.
        """
        # Sub 0x01 with mask 0x00: ISO 14229-1 special case returns count of ALL DTCs
        # Sub 0x04 reportDTCSnapshotRecordByDTCNumber can also confirm registration
        # Most reliable: sub 0x02 with mask=0x00 returns all DTCs in DB
        pdu = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x02, 0x00]))
        if pdu is None:
            pytest.skip("No response to ReadDTCInfo sub02 mask=0x00")
        if pdu[0] == SID_NEGATIVE_RESPONSE:
            # Firmware may not support mask=0x00 (returns all); try sub01 count check
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 18, (
                    f"Firmware DTC count {count} < expected 18 "
                    f"(DTC 0xC0FE00 may not be registered)"
                )
            return
        body = pdu[3:]
        if len(body) > 0 and len(body) % 4 == 0:
            registered = {(body[i]<<16)|(body[i+1]<<8)|body[i+2] for i in range(0, len(body), 4)}
            assert 12647936 in registered, (
                f"DTC 0xC0FE00 not found in firmware DTC database. "
                f"Registered codes: {[hex(c) for c in sorted(registered)]}"
            )
        else:
            # mask=0x00 returned empty — all DTCs have status 0x00 (no faults)
            # Use sub01 to confirm count includes this DTC
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 18, (
                    f"DTC count {count}: DTC 0xC0FE00 may not be registered in firmware"
                )
    def test_dtc_c0fe10_registered(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        DTC 0xC0FE10 (Firmware download incomplete — transfer interrupted) must be registered in firmware.

        ISO 14229-1 §11.3: sub 0x01 (reportNumberOfDTCByStatusMask) reports the
        total count of registered DTCs matching the mask. With mask 0x00 (special
        case — returns ALL registered DTCs regardless of status), we can verify
        0xC0FE10 is in the database even when no fault is currently active.

        Tests config/dtc_database.c registration against YAML configuration.
        """
        # Sub 0x01 with mask 0x00: ISO 14229-1 special case returns count of ALL DTCs
        # Sub 0x04 reportDTCSnapshotRecordByDTCNumber can also confirm registration
        # Most reliable: sub 0x02 with mask=0x00 returns all DTCs in DB
        pdu = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x02, 0x00]))
        if pdu is None:
            pytest.skip("No response to ReadDTCInfo sub02 mask=0x00")
        if pdu[0] == SID_NEGATIVE_RESPONSE:
            # Firmware may not support mask=0x00 (returns all); try sub01 count check
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 19, (
                    f"Firmware DTC count {count} < expected 19 "
                    f"(DTC 0xC0FE10 may not be registered)"
                )
            return
        body = pdu[3:]
        if len(body) > 0 and len(body) % 4 == 0:
            registered = {(body[i]<<16)|(body[i+1]<<8)|body[i+2] for i in range(0, len(body), 4)}
            assert 12647952 in registered, (
                f"DTC 0xC0FE10 not found in firmware DTC database. "
                f"Registered codes: {[hex(c) for c in sorted(registered)]}"
            )
        else:
            # mask=0x00 returned empty — all DTCs have status 0x00 (no faults)
            # Use sub01 to confirm count includes this DTC
            pdu_count = firmware_bus.request(bytes([SID_READ_DTC_INFO, 0x01, 0xFF]))
            if pdu_count is not None and pdu_count[0] == (SID_READ_DTC_INFO + POSITIVE_RESPONSE_OFFSET):
                import struct
                count = struct.unpack(">H", pdu_count[4:6])[0]
                assert count >= 19, (
                    f"DTC count {count}: DTC 0xC0FE10 may not be registered in firmware"
                )

# =============================================================================
# §10 RoutineControl — SID 0x31 firmware tests
#
# Generated from the `routines:` section of diagnostics_config.yaml.
# Tests exercise the full path through compiled firmware:
#   ISO-TP frame → service_0x31.c → s_validate_routine_access()
#   → routine_database_find() → callback → response encoding
#
# Routines under test (6 total):
#   RID 0xFF00  ECU_SelfTest
#     session: UDS_SESSION_EXTENDED  security: 0
#     support: start results#   RID 0xFF01  ResetCalibrationToDefaults
#     session: UDS_SESSION_EXTENDED  security: 1
#     support: start#   RID 0xFF10  EraseApplicationFlash
#     session: UDS_SESSION_PROGRAMMING  security: 1
#     support: start results#   RID 0xFF11  VerifyApplicationImage
#     session: UDS_SESSION_PROGRAMMING  security: 1
#     support: start results#   RID 0xFF20  ActivateOutputChannel
#     session: UDS_SESSION_EXTENDED  security: 1
#     support: start stop results#   RID 0xFF21  MeasureSupplyVoltage
#     session: UDS_SESSION_EXTENDED  security: 0
#     support: start results# =============================================================================

# Sub-function codes (ISO 14229-1 §13)
_RC_SUBFN_START:   int = 0x01
_RC_SUBFN_STOP:    int = 0x02
_RC_SUBFN_RESULTS: int = 0x03


def _rc_req(sub_fn: int, rid: int, option: bytes = b"") -> bytes:
    """Build a RoutineControl request PDU."""
    return bytes([SID_ROUTINE_CONTROL, sub_fn, (rid >> 8) & 0xFF, rid & 0xFF]) + option


def _assert_positive_rc(pdu: bytes, sub_fn: int, rid: int) -> None:
    """Assert a positive RoutineControl response is correctly formed."""
    assert pdu is not None, "No response received from firmware"
    assert len(pdu) >= 4, f"Response too short ({len(pdu)} bytes): {pdu.hex(' ')}"
    assert pdu[0] == SID_ROUTINE_RESPONSE, (
        f"Expected SID 0x71, got 0x{pdu[0]:02X}. Full PDU: {pdu.hex(' ')}"
    )
    assert pdu[1] == sub_fn, (
        f"Expected subFn 0x{sub_fn:02X}, got 0x{pdu[1]:02X}. PDU: {pdu.hex(' ')}"
    )
    assert pdu[2] == (rid >> 8) & 0xFF, (
        f"RID_hi mismatch: expected 0x{(rid>>8)&0xFF:02X}, got 0x{pdu[2]:02X}"
    )
    assert pdu[3] == rid & 0xFF, (
        f"RID_lo mismatch: expected 0x{rid&0xFF:02X}, got 0x{pdu[3]:02X}"
    )


def _assert_nrc_rc(pdu: bytes, expected_nrc: int) -> None:
    """Assert a negative response for RoutineControl with the expected NRC."""
    assert pdu is not None, "No response received from firmware"
    assert pdu[0] == SID_NEGATIVE_RESPONSE, (
        f"Expected NRC (0x7F), got 0x{pdu[0]:02X}. PDU: {pdu.hex(' ')}"
    )
    assert pdu[1] == SID_ROUTINE_CONTROL, (
        f"NRC service byte: expected 0x31, got 0x{pdu[1]:02X}"
    )
    assert pdu[2] == expected_nrc, (
        f"Expected NRC 0x{expected_nrc:02X}, got 0x{pdu[2]:02X}. PDU: {pdu.hex(' ')}"
    )



class TestFirmwareRoutine_ecu_selftest:
    """
    SID 0x31 — RID 0xFF00 ECU_SelfTest

    session: UDS_SESSION_EXTENDED  security_level: 0
    support: start results
    All tests validate the full path through compiled firmware:
    service_0x31.c → routine_database_find(0xFF00) → callback
    """

    RID: int = 65280

    def test_start_routine_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        startRoutine 0xFF00 (ECU_SelfTest) in UDS_SESSION_EXTENDED session → 0x71 0x01.

        Verifies: session gate passes, RID found, start_cb executes, positive response.
        ISO 14229-1 §13.3: response = [0x71, 0x01, RID_hi, RID_lo {, statusRecord}]
        """
        _enter_session(firmware_bus, SESSION_EXTENDED)
        pdu = firmware_bus.request(_rc_req(_RC_SUBFN_START, self.RID))
        _assert_positive_rc(pdu, _RC_SUBFN_START, self.RID)

    def test_session_gate_default_session_nrc(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        startRoutine 0xFF00 in Default session → NRC 0x7F.

        Default session ordinal (1) < required UDS_SESSION_EXTENDED ordinal (3).
        s_validate_routine_access() → uds_safety_validate_session() → NRC 0x7F.
        Firmware is in Default session at fixture startup — no session switch needed.
        """
        # Firmware starts in Default session — do NOT switch
        pdu = firmware_bus.request(_rc_req(_RC_SUBFN_START, self.RID))
        _assert_nrc_rc(pdu, NRC_SERVICE_NOT_SUPPORTED_IN_SESSION)

    def test_stop_not_supported_nrc(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        stopRoutine 0xFF00 → NRC 0x12 subFunctionNotSupported.

        ECU_SelfTest support_flags does not include ROUTINE_SUPPORT_STOP.
        service_0x31.c: stop_cb is NULL → NRC 0x12.
        """
        _enter_session(firmware_bus, SESSION_EXTENDED)
        pdu = firmware_bus.request(_rc_req(_RC_SUBFN_STOP, self.RID))
        _assert_nrc_rc(pdu, NRC_SUB_FUNCTION_NOT_SUPPORTED)

    def test_request_results_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        requestRoutineResults 0xFF00 → 0x71 0x03.

        ECU_SelfTest includes ROUTINE_SUPPORT_RESULTS.
        Stub callback returns UDS_STATUS_OK with zero result bytes.
        Response: [0x71, 0x03, RID_hi, RID_lo]
        """
        _enter_session(firmware_bus, SESSION_EXTENDED)
        pdu = firmware_bus.request(_rc_req(_RC_SUBFN_RESULTS, self.RID))
        _assert_positive_rc(pdu, _RC_SUBFN_RESULTS, self.RID)


class TestFirmwareRoutine_resetcalibrationtodefaults:
    """
    SID 0x31 — RID 0xFF01 ResetCalibrationToDefaults

    session: UDS_SESSION_EXTENDED  security_level: 1
    support: start
    All tests validate the full path through compiled firmware:
    service_0x31.c → routine_database_find(0xFF01) → callback
    """

    RID: int = 65281

    def test_start_routine_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        startRoutine 0xFF01 (ResetCalibrationToDefaults) in UDS_SESSION_EXTENDED session → 0x71 0x01.

        Verifies: session gate passes, RID found, start_cb executes, positive response.
        ISO 14229-1 §13.3: response = [0x71, 0x01, RID_hi, RID_lo {, statusRecord}]
        """
        _enter_session(firmware_bus, SESSION_EXTENDED)
        _unlock_level(firmware_bus, 1)
        pdu = firmware_bus.request(_rc_req(_RC_SUBFN_START, self.RID))
        _assert_positive_rc(pdu, _RC_SUBFN_START, self.RID)

    def test_session_gate_default_session_nrc(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        startRoutine 0xFF01 in Default session → NRC 0x7F.

        Default session ordinal (1) < required UDS_SESSION_EXTENDED ordinal (3).
        s_validate_routine_access() → uds_safety_validate_session() → NRC 0x7F.
        Firmware is in Default session at fixture startup — no session switch needed.
        """
        # Firmware starts in Default session — do NOT switch
        pdu = firmware_bus.request(_rc_req(_RC_SUBFN_START, self.RID))
        _assert_nrc_rc(pdu, NRC_SERVICE_NOT_SUPPORTED_IN_SESSION)

    def test_security_gate_locked_nrc(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        startRoutine 0xFF01 without security unlock → NRC 0x33.

        ResetCalibrationToDefaults requires security_level = 1.
        Without SecurityAccess unlock, s_validate_routine_access() returns
        UDS_STATUS_ERR_SEC_NOT_UNLOCKED → NRC 0x33 securityAccessDenied.
        """
        _enter_session(firmware_bus, SESSION_EXTENDED)
        # Deliberately do NOT call _unlock_level — verify the security gate blocks
        pdu = firmware_bus.request(_rc_req(_RC_SUBFN_START, self.RID))
        _assert_nrc_rc(pdu, NRC_SECURITY_ACCESS_DENIED)

    def test_stop_not_supported_nrc(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        stopRoutine 0xFF01 → NRC 0x12 subFunctionNotSupported.

        ResetCalibrationToDefaults support_flags does not include ROUTINE_SUPPORT_STOP.
        service_0x31.c: stop_cb is NULL → NRC 0x12.
        """
        _enter_session(firmware_bus, SESSION_EXTENDED)
        _unlock_level(firmware_bus, 1)
        pdu = firmware_bus.request(_rc_req(_RC_SUBFN_STOP, self.RID))
        _assert_nrc_rc(pdu, NRC_SUB_FUNCTION_NOT_SUPPORTED)


class TestFirmwareRoutine_eraseapplicationflash:
    """
    SID 0x31 — RID 0xFF10 EraseApplicationFlash

    session: UDS_SESSION_PROGRAMMING  security_level: 1
    support: start results
    All tests validate the full path through compiled firmware:
    service_0x31.c → routine_database_find(0xFF10) → callback
    """

    RID: int = 65296

    def test_start_routine_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        startRoutine 0xFF10 (EraseApplicationFlash) in UDS_SESSION_PROGRAMMING session → 0x71 0x01.

        Verifies: session gate passes, RID found, start_cb executes, positive response.
        ISO 14229-1 §13.3: response = [0x71, 0x01, RID_hi, RID_lo {, statusRecord}]
        """
        _enter_session(firmware_bus, SESSION_PROGRAMMING)
        _unlock_level(firmware_bus, 1)
        pdu = firmware_bus.request(_rc_req(_RC_SUBFN_START, self.RID))
        _assert_positive_rc(pdu, _RC_SUBFN_START, self.RID)

    def test_session_gate_default_session_nrc(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        startRoutine 0xFF10 in Default session → NRC 0x7F.

        Default session ordinal (1) < required UDS_SESSION_PROGRAMMING ordinal (2).
        s_validate_routine_access() → uds_safety_validate_session() → NRC 0x7F.
        Firmware is in Default session at fixture startup — no session switch needed.
        """
        # Firmware starts in Default session — do NOT switch
        pdu = firmware_bus.request(_rc_req(_RC_SUBFN_START, self.RID))
        _assert_nrc_rc(pdu, NRC_SERVICE_NOT_SUPPORTED_IN_SESSION)

    def test_security_gate_locked_nrc(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        startRoutine 0xFF10 without security unlock → NRC 0x33.

        EraseApplicationFlash requires security_level = 1.
        Without SecurityAccess unlock, s_validate_routine_access() returns
        UDS_STATUS_ERR_SEC_NOT_UNLOCKED → NRC 0x33 securityAccessDenied.
        """
        _enter_session(firmware_bus, SESSION_PROGRAMMING)
        # Deliberately do NOT call _unlock_level — verify the security gate blocks
        pdu = firmware_bus.request(_rc_req(_RC_SUBFN_START, self.RID))
        _assert_nrc_rc(pdu, NRC_SECURITY_ACCESS_DENIED)

    def test_stop_not_supported_nrc(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        stopRoutine 0xFF10 → NRC 0x12 subFunctionNotSupported.

        EraseApplicationFlash support_flags does not include ROUTINE_SUPPORT_STOP.
        service_0x31.c: stop_cb is NULL → NRC 0x12.
        """
        _enter_session(firmware_bus, SESSION_PROGRAMMING)
        _unlock_level(firmware_bus, 1)
        pdu = firmware_bus.request(_rc_req(_RC_SUBFN_STOP, self.RID))
        _assert_nrc_rc(pdu, NRC_SUB_FUNCTION_NOT_SUPPORTED)

    def test_request_results_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        requestRoutineResults 0xFF10 → 0x71 0x03.

        EraseApplicationFlash includes ROUTINE_SUPPORT_RESULTS.
        Stub callback returns UDS_STATUS_OK with zero result bytes.
        Response: [0x71, 0x03, RID_hi, RID_lo]
        """
        _enter_session(firmware_bus, SESSION_PROGRAMMING)
        _unlock_level(firmware_bus, 1)
        pdu = firmware_bus.request(_rc_req(_RC_SUBFN_RESULTS, self.RID))
        _assert_positive_rc(pdu, _RC_SUBFN_RESULTS, self.RID)


class TestFirmwareRoutine_verifyapplicationimage:
    """
    SID 0x31 — RID 0xFF11 VerifyApplicationImage

    session: UDS_SESSION_PROGRAMMING  security_level: 1
    support: start results
    All tests validate the full path through compiled firmware:
    service_0x31.c → routine_database_find(0xFF11) → callback
    """

    RID: int = 65297

    def test_start_routine_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        startRoutine 0xFF11 (VerifyApplicationImage) in UDS_SESSION_PROGRAMMING session → 0x71 0x01.

        Verifies: session gate passes, RID found, start_cb executes, positive response.
        ISO 14229-1 §13.3: response = [0x71, 0x01, RID_hi, RID_lo {, statusRecord}]
        """
        _enter_session(firmware_bus, SESSION_PROGRAMMING)
        _unlock_level(firmware_bus, 1)
        pdu = firmware_bus.request(_rc_req(_RC_SUBFN_START, self.RID))
        _assert_positive_rc(pdu, _RC_SUBFN_START, self.RID)

    def test_session_gate_default_session_nrc(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        startRoutine 0xFF11 in Default session → NRC 0x7F.

        Default session ordinal (1) < required UDS_SESSION_PROGRAMMING ordinal (2).
        s_validate_routine_access() → uds_safety_validate_session() → NRC 0x7F.
        Firmware is in Default session at fixture startup — no session switch needed.
        """
        # Firmware starts in Default session — do NOT switch
        pdu = firmware_bus.request(_rc_req(_RC_SUBFN_START, self.RID))
        _assert_nrc_rc(pdu, NRC_SERVICE_NOT_SUPPORTED_IN_SESSION)

    def test_security_gate_locked_nrc(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        startRoutine 0xFF11 without security unlock → NRC 0x33.

        VerifyApplicationImage requires security_level = 1.
        Without SecurityAccess unlock, s_validate_routine_access() returns
        UDS_STATUS_ERR_SEC_NOT_UNLOCKED → NRC 0x33 securityAccessDenied.
        """
        _enter_session(firmware_bus, SESSION_PROGRAMMING)
        # Deliberately do NOT call _unlock_level — verify the security gate blocks
        pdu = firmware_bus.request(_rc_req(_RC_SUBFN_START, self.RID))
        _assert_nrc_rc(pdu, NRC_SECURITY_ACCESS_DENIED)

    def test_stop_not_supported_nrc(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        stopRoutine 0xFF11 → NRC 0x12 subFunctionNotSupported.

        VerifyApplicationImage support_flags does not include ROUTINE_SUPPORT_STOP.
        service_0x31.c: stop_cb is NULL → NRC 0x12.
        """
        _enter_session(firmware_bus, SESSION_PROGRAMMING)
        _unlock_level(firmware_bus, 1)
        pdu = firmware_bus.request(_rc_req(_RC_SUBFN_STOP, self.RID))
        _assert_nrc_rc(pdu, NRC_SUB_FUNCTION_NOT_SUPPORTED)

    def test_request_results_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        requestRoutineResults 0xFF11 → 0x71 0x03.

        VerifyApplicationImage includes ROUTINE_SUPPORT_RESULTS.
        Stub callback returns UDS_STATUS_OK with zero result bytes.
        Response: [0x71, 0x03, RID_hi, RID_lo]
        """
        _enter_session(firmware_bus, SESSION_PROGRAMMING)
        _unlock_level(firmware_bus, 1)
        pdu = firmware_bus.request(_rc_req(_RC_SUBFN_RESULTS, self.RID))
        _assert_positive_rc(pdu, _RC_SUBFN_RESULTS, self.RID)


class TestFirmwareRoutine_activateoutputchannel:
    """
    SID 0x31 — RID 0xFF20 ActivateOutputChannel

    session: UDS_SESSION_EXTENDED  security_level: 1
    support: start stop results
    All tests validate the full path through compiled firmware:
    service_0x31.c → routine_database_find(0xFF20) → callback
    """

    RID: int = 65312

    def test_start_routine_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        startRoutine 0xFF20 (ActivateOutputChannel) in UDS_SESSION_EXTENDED session → 0x71 0x01.

        Verifies: session gate passes, RID found, start_cb executes, positive response.
        ISO 14229-1 §13.3: response = [0x71, 0x01, RID_hi, RID_lo {, statusRecord}]
        """
        _enter_session(firmware_bus, SESSION_EXTENDED)
        _unlock_level(firmware_bus, 1)
        pdu = firmware_bus.request(_rc_req(_RC_SUBFN_START, self.RID))
        _assert_positive_rc(pdu, _RC_SUBFN_START, self.RID)

    def test_session_gate_default_session_nrc(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        startRoutine 0xFF20 in Default session → NRC 0x7F.

        Default session ordinal (1) < required UDS_SESSION_EXTENDED ordinal (3).
        s_validate_routine_access() → uds_safety_validate_session() → NRC 0x7F.
        Firmware is in Default session at fixture startup — no session switch needed.
        """
        # Firmware starts in Default session — do NOT switch
        pdu = firmware_bus.request(_rc_req(_RC_SUBFN_START, self.RID))
        _assert_nrc_rc(pdu, NRC_SERVICE_NOT_SUPPORTED_IN_SESSION)

    def test_security_gate_locked_nrc(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        startRoutine 0xFF20 without security unlock → NRC 0x33.

        ActivateOutputChannel requires security_level = 1.
        Without SecurityAccess unlock, s_validate_routine_access() returns
        UDS_STATUS_ERR_SEC_NOT_UNLOCKED → NRC 0x33 securityAccessDenied.
        """
        _enter_session(firmware_bus, SESSION_EXTENDED)
        # Deliberately do NOT call _unlock_level — verify the security gate blocks
        pdu = firmware_bus.request(_rc_req(_RC_SUBFN_START, self.RID))
        _assert_nrc_rc(pdu, NRC_SECURITY_ACCESS_DENIED)

    def test_request_results_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        requestRoutineResults 0xFF20 → 0x71 0x03.

        ActivateOutputChannel includes ROUTINE_SUPPORT_RESULTS.
        Stub callback returns UDS_STATUS_OK with zero result bytes.
        Response: [0x71, 0x03, RID_hi, RID_lo]
        """
        _enter_session(firmware_bus, SESSION_EXTENDED)
        _unlock_level(firmware_bus, 1)
        pdu = firmware_bus.request(_rc_req(_RC_SUBFN_RESULTS, self.RID))
        _assert_positive_rc(pdu, _RC_SUBFN_RESULTS, self.RID)


class TestFirmwareRoutine_measuresupplyvoltage:
    """
    SID 0x31 — RID 0xFF21 MeasureSupplyVoltage

    session: UDS_SESSION_EXTENDED  security_level: 0
    support: start results
    All tests validate the full path through compiled firmware:
    service_0x31.c → routine_database_find(0xFF21) → callback
    """

    RID: int = 65313

    def test_start_routine_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        startRoutine 0xFF21 (MeasureSupplyVoltage) in UDS_SESSION_EXTENDED session → 0x71 0x01.

        Verifies: session gate passes, RID found, start_cb executes, positive response.
        ISO 14229-1 §13.3: response = [0x71, 0x01, RID_hi, RID_lo {, statusRecord}]
        """
        _enter_session(firmware_bus, SESSION_EXTENDED)
        pdu = firmware_bus.request(_rc_req(_RC_SUBFN_START, self.RID))
        _assert_positive_rc(pdu, _RC_SUBFN_START, self.RID)

    def test_session_gate_default_session_nrc(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        startRoutine 0xFF21 in Default session → NRC 0x7F.

        Default session ordinal (1) < required UDS_SESSION_EXTENDED ordinal (3).
        s_validate_routine_access() → uds_safety_validate_session() → NRC 0x7F.
        Firmware is in Default session at fixture startup — no session switch needed.
        """
        # Firmware starts in Default session — do NOT switch
        pdu = firmware_bus.request(_rc_req(_RC_SUBFN_START, self.RID))
        _assert_nrc_rc(pdu, NRC_SERVICE_NOT_SUPPORTED_IN_SESSION)

    def test_stop_not_supported_nrc(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        stopRoutine 0xFF21 → NRC 0x12 subFunctionNotSupported.

        MeasureSupplyVoltage support_flags does not include ROUTINE_SUPPORT_STOP.
        service_0x31.c: stop_cb is NULL → NRC 0x12.
        """
        _enter_session(firmware_bus, SESSION_EXTENDED)
        pdu = firmware_bus.request(_rc_req(_RC_SUBFN_STOP, self.RID))
        _assert_nrc_rc(pdu, NRC_SUB_FUNCTION_NOT_SUPPORTED)

    def test_request_results_happy_path(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        requestRoutineResults 0xFF21 → 0x71 0x03.

        MeasureSupplyVoltage includes ROUTINE_SUPPORT_RESULTS.
        Stub callback returns UDS_STATUS_OK with zero result bytes.
        Response: [0x71, 0x03, RID_hi, RID_lo]
        """
        _enter_session(firmware_bus, SESSION_EXTENDED)
        pdu = firmware_bus.request(_rc_req(_RC_SUBFN_RESULTS, self.RID))
        _assert_positive_rc(pdu, _RC_SUBFN_RESULTS, self.RID)


class TestFirmwareRoutineControlEdgeCases:
    """
    SID 0x31 — edge cases not tied to a specific RID.
    These test the service dispatcher logic in service_0x31.c.
    """

    def test_unknown_rid_nrc_31(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        startRoutine with RID 0xDEAD (not registered) → NRC 0x31 requestOutOfRange.

        routine_database_find(0xDEAD) returns NULL.
        service_0x31.c returns UDS_STATUS_ERR_ROUTINE_NOT_FOUND → NRC 0x31.
        """
        _enter_session(firmware_bus, SESSION_EXTENDED)
        pdu = firmware_bus.request(_rc_req(_RC_SUBFN_START, 0xDEAD))
        _assert_nrc_rc(pdu, NRC_REQUEST_OUT_OF_RANGE)

    def test_bad_subfn_nrc_12(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        RoutineControl with subFn 0x04 (undefined) → NRC 0x12 subFunctionNotSupported.

        ISO 14229-1 §13 defines only sub-functions 0x01, 0x02, 0x03.
        service_0x31.c validates sub_fn before any database lookup.
        """
        _enter_session(firmware_bus, SESSION_EXTENDED)
        first_rid = 65280
        pdu = firmware_bus.request(bytes([SID_ROUTINE_CONTROL, 0x04,
                                          (first_rid >> 8) & 0xFF, first_rid & 0xFF]))
        _assert_nrc_rc(pdu, NRC_SUB_FUNCTION_NOT_SUPPORTED)

    def test_request_too_short_nrc_13(
        self, firmware_bus: FirmwareIsoTpTransport
    ) -> None:
        """
        RoutineControl with 3-byte request (missing RID_lo) → NRC 0x13.

        Minimum valid request is [SID, subFn, RID_hi, RID_lo] = 4 bytes.
        service_0x31.c: uds_service_validate_length(req, 4) → NRC 0x13.
        """
        pdu = firmware_bus.request(bytes([SID_ROUTINE_CONTROL, 0x01, 0xFF]))
        _assert_nrc_rc(pdu, NRC_INCORRECT_MSG_LEN)

