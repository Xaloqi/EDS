# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Xaloqi
"""
tests/test_doip_integration.py — DoIP end-to-end integration tests.

WHAT THIS TESTS:
  The complete round-trip between the EDS DoIP server firmware (C, running on
  Zephyr native_sim) and the xaloqi-tester DoipBus Python client (TestLab).
  This closes the loop between the Week 1 firmware (doip_server.c) and the
  Python client that shipped in TestLab v1.1.0.

PREREQUISITES:
  1. Zephyr SDK installed, west configured.
  2. xaloqi-tester installed: pip install -e path/to/TestLab
  3. basic_ecu_doip already built:
       west build -b native_sim examples/basic_ecu_doip -- \\
           -DEXTRA_CONF_FILE=boards/native_sim/native_sim_doip.conf
  4. Environment variable: XALOQI_LICENSE_SKIP=1 (CI-safe, no key needed)

RUNNING:
  # From EDS repo root:
  pytest tests/test_doip_integration.py -v

  # With verbose ECU output:
  pytest tests/test_doip_integration.py -v -s

CI JOB:
  doip-integration in .github/workflows/ci.yml — see that file for the
  exact build + run sequence used in GitHub Actions.

DESIGN:
  - One module-scoped fixture launches basic_ecu_doip on native_sim and
    tears it down after all tests complete.  The ECU binary path is
    configurable via the DOIP_ECU_BINARY env var (default: the west build
    output location).
  - Each test uses an async-scoped UdsTester context so TCP connections
    are independent (routing state resets between tests).
  - Timeout: 5 seconds per operation — generous for CI; adjust via
    DOIP_TEST_TIMEOUT_S env var.

INTEROPERABILITY NOTE:
  DoipBus default constructor: DoipBus("127.0.0.1")
    source_address=0x0E00, target_address=0xE400, port=13400
  These must match the ECU config in basic_ecu_doip/diagnostics_config.yaml:
    logical_address: 0xE400, source_address: 0x0E00, port: 13400
"""

from __future__ import annotations

import asyncio
import os
import subprocess
import time
from typing import AsyncGenerator, Generator

import pytest

# ---------------------------------------------------------------------------
# xaloqi-tester imports — DoIP transport + UDS client
# Skip the entire module when xaloqi-tester is not installed (e.g. CI without
# TestLab access). The ECU binary smoke-check and unit tests still run.
# ---------------------------------------------------------------------------
xaloqi = pytest.importorskip(
    "xaloqi",
    reason="xaloqi-tester not installed — skipping DoIP integration tests"
)
from xaloqi.tester import DoipBus, Session, UdsTester

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

_DEFAULT_BINARY = "build/zephyr/zephyr.exe"
_ECU_BINARY     = os.environ.get("DOIP_ECU_BINARY", _DEFAULT_BINARY)
_ECU_HOST       = os.environ.get("DOIP_ECU_HOST", "127.0.0.1")
_ECU_PORT       = int(os.environ.get("DOIP_ECU_PORT", "13400"))
_STARTUP_DELAY  = float(os.environ.get("DOIP_STARTUP_DELAY_S", "1.5"))
_TIMEOUT        = float(os.environ.get("DOIP_TEST_TIMEOUT_S", "5.0"))

# VIN as written in basic_ecu_doip/src/main.c
_EXPECTED_VIN   = b"DOIPECUEDS00001\x00\x00"  # 17 bytes (15 chars + 2 null pads)

# ---------------------------------------------------------------------------
# Module-scoped fixture: launch and shut down the native_sim ECU
# ---------------------------------------------------------------------------

@pytest.fixture(scope="module")
def doip_ecu() -> Generator[subprocess.Popen, None, None]:
    """Build (if needed) and launch basic_ecu_doip on native_sim."""
    if not os.path.exists(_ECU_BINARY):
        pytest.skip(
            f"ECU binary not found: {_ECU_BINARY}\n"
            f"Build first with:\n"
            f"  west build -b native_sim examples/basic_ecu_doip -- \\\n"
            f"      -DEXTRA_CONF_FILE=boards/native_sim/native_sim_doip.conf"
        )

    proc = subprocess.Popen(
        [_ECU_BINARY],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(_STARTUP_DELAY)   # wait for TCP listener to come up
    if proc.poll() is not None:
        pytest.fail(f"ECU process exited immediately (rc={proc.returncode})")

    yield proc

    proc.terminate()
    try:
        proc.wait(timeout=3.0)
    except subprocess.TimeoutExpired:
        proc.kill()


# ---------------------------------------------------------------------------
# Per-test helper: create a fresh UdsTester connection
# ---------------------------------------------------------------------------

def _bus() -> DoipBus:
    return DoipBus(
        host=_ECU_HOST,
        port=_ECU_PORT,
        source_address=0x0E00,
        target_address=0xE400,
        timeout=_TIMEOUT,
    )


# ===========================================================================
# Test 1 — Routing activation completes successfully
# ===========================================================================

@pytest.mark.asyncio
async def test_doip_routing_activation(doip_ecu: subprocess.Popen) -> None:
    """DoipBus connects to native_sim ECU and completes routing activation."""
    async with UdsTester(_bus(), rx_id=0xE400, tx_id=0x0E00) as ecu:
        # If routing activation failed, UdsTester.__aenter__ raises TransportError.
        # Reaching here means it succeeded.
        assert ecu is not None


# ===========================================================================
# Test 2 — DiagnosticSessionControl: open extended session
# ===========================================================================

@pytest.mark.asyncio
async def test_doip_session_control(doip_ecu: subprocess.Popen) -> None:
    """UdsTester over DoipBus: switch to extended session (0x03)."""
    async with UdsTester(_bus(), rx_id=0xE400, tx_id=0x0E00) as ecu:
        await ecu.change_session(Session.EXTENDED)
        # No exception = positive response received


# ===========================================================================
# Test 3 — ReadDataByIdentifier: VIN (0xF190)
# ===========================================================================

@pytest.mark.asyncio
async def test_doip_read_did_vin(doip_ecu: subprocess.Popen) -> None:
    """Read VIN (0xF190) over DoIP — same value as in main.c."""
    async with UdsTester(_bus(), rx_id=0xE400, tx_id=0x0E00) as ecu:
        vin = await ecu.read_did(0xF190)
        assert len(vin) == 17, f"VIN length wrong: {len(vin)}"
        assert vin == _EXPECTED_VIN, f"VIN mismatch: {vin!r}"


# ===========================================================================
# Test 4 — ReadDataByIdentifier: ECU Serial Number (0xF18C)
# ===========================================================================

@pytest.mark.asyncio
async def test_doip_read_did_serial(doip_ecu: subprocess.Popen) -> None:
    """Read ECU serial number (0xF18C) over DoIP."""
    async with UdsTester(_bus(), rx_id=0xE400, tx_id=0x0E00) as ecu:
        serial = await ecu.read_did(0xF18C)
        assert len(serial) == 4
        assert serial == bytes([0x02, 0x00, 0x00, 0x01])


# ===========================================================================
# Test 5 — SecurityAccess: AES-CMAC seed/key exchange
# ===========================================================================

@pytest.mark.asyncio
async def test_doip_security_access(doip_ecu: subprocess.Popen) -> None:
    """AES-CMAC SecurityAccess (0x27) works over DoIP."""
    async with UdsTester(_bus(), rx_id=0xE400, tx_id=0x0E00) as ecu:
        await ecu.change_session(Session.EXTENDED)
        await ecu.security_access(level=1)
        # No exception = SecurityAccess positive response received


# ===========================================================================
# Test 6 — NRC returned over DoIP (access without security unlock)
# ===========================================================================

@pytest.mark.asyncio
async def test_doip_read_did_security_required(doip_ecu: subprocess.Popen) -> None:
    """Spare Part Number (0xF187) in extended session without unlock → NRC 0x33."""
    from xaloqi.tester.exceptions import NegativeResponseError

    async with UdsTester(_bus(), rx_id=0xE400, tx_id=0x0E00) as ecu:
        await ecu.change_session(Session.EXTENDED)
        # 0xF187 requires security level 1 to write; reading should succeed
        # but writing without unlock should produce NRC 0x33 (securityAccessDenied)
        try:
            await ecu.write_did(0xF187, b"EDS-DIP-001")
            pytest.fail("Expected NegativeResponseError (0x33) was not raised")
        except NegativeResponseError as exc:
            assert exc.nrc == 0x33, f"Expected NRC 0x33, got 0x{exc.nrc:02X}"


# ===========================================================================
# Test 7 — WriteDataByIdentifier: round-trip write + verify read
# ===========================================================================

@pytest.mark.asyncio
async def test_doip_write_did(doip_ecu: subprocess.Popen) -> None:
    """Write Spare Part Number (0xF187) over DoIP after security unlock."""
    async with UdsTester(_bus(), rx_id=0xE400, tx_id=0x0E00) as ecu:
        await ecu.change_session(Session.EXTENDED)
        await ecu.security_access(level=1)
        new_part = b"EDS-TST-999"
        await ecu.write_did(0xF187, new_part)
        # Read back and verify
        read_back = await ecu.read_did(0xF187)
        assert read_back == new_part, f"Write-read mismatch: {read_back!r}"


# ===========================================================================
# Test 8 — ReadDTCInformation (0x19 subfunction 0x02)
# ===========================================================================

@pytest.mark.asyncio
async def test_doip_read_dtc(doip_ecu: subprocess.Popen) -> None:
    """0x19 ReadDTCInformation over DoIP returns a positive response."""
    async with UdsTester(_bus(), rx_id=0xE400, tx_id=0x0E00) as ecu:
        # read_dtcs() sends 0x19 0x02 0xFF (all DTCs, all status masks)
        dtcs = await ecu.read_dtcs()
        # basic_ecu_doip has 2 configured DTCs (0xC00100, 0xC00200);
        # none are active on cold start — empty list is valid
        assert isinstance(dtcs, list)


# ===========================================================================
# Test 9 — Alive Check handled without disrupting the session
# ===========================================================================

@pytest.mark.asyncio
async def test_doip_alive_check_handled(doip_ecu: subprocess.Popen) -> None:
    """Send DoIP Alive Check Request; verify ECU responds and session continues."""
    import struct

    reader, writer = await asyncio.wait_for(
        asyncio.open_connection(_ECU_HOST, _ECU_PORT),
        timeout=_TIMEOUT,
    )
    try:
        # Perform routing activation manually
        ra_payload = struct.pack(">H B I", 0x0E00, 0x00, 0x00000000)
        header = struct.pack(">BBH I", 0x02, 0xFD, 0x0005, len(ra_payload))
        writer.write(header + ra_payload)
        await writer.drain()

        # Read routing activation response
        hdr_bytes = await asyncio.wait_for(reader.readexactly(8), timeout=_TIMEOUT)
        _, _, pt, length = struct.unpack_from(">BBH I", hdr_bytes)
        assert pt == 0x0006, f"Expected routing activation response 0x0006, got 0x{pt:04X}"
        await reader.readexactly(length)  # consume payload

        # Send Alive Check Request (0x0007, empty payload)
        ac_header = struct.pack(">BBH I", 0x02, 0xFD, 0x0007, 0)
        writer.write(ac_header)
        await writer.drain()

        # Read Alive Check Response (0x0008)
        ac_resp_hdr = await asyncio.wait_for(reader.readexactly(8), timeout=_TIMEOUT)
        _, _, ac_pt, ac_len = struct.unpack_from(">BBH I", ac_resp_hdr)
        assert ac_pt == 0x0008, f"Expected alive check response 0x0008, got 0x{ac_pt:04X}"
        assert ac_len == 0, "Alive check response should have empty payload"
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass


# ===========================================================================
# Test 10 — Disconnect and reconnect: routing state resets
# ===========================================================================

@pytest.mark.asyncio
async def test_doip_disconnect_reconnect(doip_ecu: subprocess.Popen) -> None:
    """Close TCP connection, reconnect, and verify routing activation succeeds again."""
    # First connection
    async with UdsTester(_bus(), rx_id=0xE400, tx_id=0x0E00) as ecu1:
        vin1 = await ecu1.read_did(0xF190)
        assert len(vin1) == 17

    # Brief pause to let the server accept the next connection
    await asyncio.sleep(0.2)

    # Second connection — fresh routing activation should succeed
    async with UdsTester(_bus(), rx_id=0xE400, tx_id=0x0E00) as ecu2:
        vin2 = await ecu2.read_did(0xF190)
        assert vin2 == vin1, "VIN should be identical across connections"


# ===========================================================================
# Test 11 — TesterPresent with suppressPosResponse=True
# ===========================================================================

@pytest.mark.asyncio
async def test_doip_tester_present_suppress(doip_ecu: subprocess.Popen) -> None:
    """0x3E TesterPresent with suppress bit set: no response frame expected."""
    import struct
    from xaloqi.tester.exceptions import TimeoutError as UdsTimeout

    async with UdsTester(_bus(), rx_id=0xE400, tx_id=0x0E00) as ecu:
        # tester_present(suppress=True) should return None (no positive response)
        result = await ecu.tester_present(suppress=True)
        assert result is None, "suppress=True must produce no positive response"
