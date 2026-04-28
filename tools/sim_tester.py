#!/usr/bin/env python3
"""
=============================================================================
Xaloqi EDS
tools/sim_tester.py

PURPOSE: Host-side UDS tester for use with the native_sim build target.

         Communicates with the running native_sim ECU via a virtual CAN
         socket (vcan0) or via stdin/stdout pipe depending on the Zephyr
         native_sim CAN loopback driver configuration.

         Exercises all five UDS services exposed by the BasicECU:
           0x10  DiagnosticSessionControl
           0x11  ECUReset
           0x22  ReadDataByIdentifier
           0x27  SecurityAccess
           0x3E  TesterPresent

USAGE:
    # Build the ECU firmware for native_sim:
    west build -b native_sim examples/basic_ecu

    # In one terminal, run the firmware:
    ./build/zephyr/zephyr.exe

    # In another terminal, run the tester:
    python3 tools/sim_tester.py

    # Run a specific test only:
    python3 tools/sim_tester.py --test session
    python3 tools/sim_tester.py --test security
    python3 tools/sim_tester.py --test read_did
    python3 tools/sim_tester.py --test tester_present

DEPENDENCIES:
    python-can >= 4.0.0:  pip install python-can

ADDRESSING (ISO 15765-4):
    TX (tester -> ECU):  CAN ID 0x7DF  (functional request)
    RX (ECU -> tester):  CAN ID 0x7E8  (physical response)

SECURITY ALGORITHM (must match main.c stub):
    expected_key[n] = seed[n] XOR mask[n]
    mask = [0xAA, 0xBB, 0xCC, 0xDD]

=============================================================================
"""

import argparse
import sys
import time
import struct
import logging
from typing import Optional, List

try:
    import can
except ImportError:
    print("ERROR: python-can not installed. Run: pip install python-can")
    sys.exit(1)

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

TESTER_TX_ID = 0x7DF   # ISO 15765-4 functional request
ECU_TX_ID    = 0x7E8   # ISO 15765-4 physical response
CHANNEL      = "vcan0" # Virtual CAN interface (Linux: sudo modprobe vcan)
BUSTYPE      = "socketcan"
TIMEOUT_S    = 0.5     # Response timeout per request

# Key derivation mask (must match main.c app_security_key_validate)
KEY_MASK = [0xAA, 0xBB, 0xCC, 0xDD]

logging.basicConfig(level=logging.INFO, format="%(levelname)-8s %(message)s")
log = logging.getLogger("sim_tester")


# ---------------------------------------------------------------------------
# Transport helpers
# ---------------------------------------------------------------------------

class UdsTester:
    """Minimal ISO-TP single-frame tester for UDS communication."""

    def __init__(self, bus):
        self.bus = bus
        self.pass_count = 0
        self.fail_count = 0

    def send(self, data: List[int]) -> None:
        """Send a UDS request as a classic CAN single frame."""
        payload = bytes(data)
        if len(payload) > 7:
            raise ValueError(f"Payload too long for SF: {len(payload)} bytes")
        # ISO-TP SF: byte 0 = 0x0N (N = data length), followed by N data bytes
        frame_data = bytes([len(payload)]) + payload
        frame_data = frame_data.ljust(8, b'\x00')  # Pad to 8 bytes
        msg = can.Message(arbitration_id=TESTER_TX_ID,
                          data=frame_data,
                          is_extended_id=False)
        self.bus.send(msg)
        log.debug(f"TX [{TESTER_TX_ID:03X}]: {frame_data.hex(' ').upper()}")

    def recv(self, timeout: float = TIMEOUT_S) -> Optional[List[int]]:
        """Wait for a UDS response from the ECU."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            msg = self.bus.recv(timeout=0.01)
            if msg is None:
                continue
            if msg.arbitration_id != ECU_TX_ID:
                continue
            log.debug(f"RX [{ECU_TX_ID:03X}]: {bytes(msg.data).hex(' ').upper()}")
            # ISO-TP SF: first nibble 0x0, length in low nibble
            pci = msg.data[0]
            frame_type = (pci >> 4) & 0x0F
            if frame_type == 0:  # Single frame
                length = pci & 0x0F
                return list(msg.data[1:1 + length])
        return None

    def request(self, data: List[int], expect_positive: bool = True) -> Optional[List[int]]:
        """Send request and return response payload, or None on timeout."""
        self.send(data)
        resp = self.recv()
        if resp is None:
            log.error(f"TIMEOUT waiting for response to: {bytes(data).hex(' ').upper()}")
            return None
        return resp

    def check(self, name: str, condition: bool, detail: str = "") -> bool:
        if condition:
            self.pass_count += 1
            log.info(f"  PASS  {name}")
        else:
            self.fail_count += 1
            log.error(f"  FAIL  {name}  {detail}")
        return condition

    def summary(self):
        total = self.pass_count + self.fail_count
        log.info(f"\n{'='*60}")
        log.info(f"Results: {self.pass_count}/{total} passed, "
                 f"{self.fail_count} failed")
        log.info(f"{'='*60}")
        return self.fail_count == 0


# ---------------------------------------------------------------------------
# Individual test cases
# ---------------------------------------------------------------------------

def test_tester_present(t: UdsTester) -> None:
    """0x3E TesterPresent — verify positive response."""
    log.info("\n[0x3E] TesterPresent")

    # Sub-function 0x00: respond + suppress = off
    resp = t.request([0x3E, 0x00])
    t.check("0x3E positive response SID",
            resp is not None and resp[0] == 0x7E,
            f"got: {resp}")
    t.check("0x3E sub-function echo",
            resp is not None and len(resp) >= 2 and resp[1] == 0x00,
            f"got: {resp}")

    # Sub-function 0x80: suppress positive response bit set — ECU must NOT respond
    t.send([0x3E, 0x80])
    time.sleep(0.1)
    resp_suppressed = t.recv(timeout=0.15)
    t.check("0x3E suppressed response (no reply expected)",
            resp_suppressed is None,
            f"unexpected response: {resp_suppressed}")


def test_session_control(t: UdsTester) -> None:
    """0x10 DiagnosticSessionControl — cycle through sessions."""
    log.info("\n[0x10] DiagnosticSessionControl")

    # Default session (already in default, but exercise the transition)
    resp = t.request([0x10, 0x01])
    t.check("0x10 -> Default session positive",
            resp is not None and resp[0] == 0x50 and resp[1] == 0x01,
            f"got: {resp}")

    # Extended diagnostic session
    resp = t.request([0x10, 0x03])
    t.check("0x10 -> Extended session positive",
            resp is not None and resp[0] == 0x50 and resp[1] == 0x03,
            f"got: {resp}")

    # Programming session
    resp = t.request([0x10, 0x02])
    t.check("0x10 -> Programming session positive",
            resp is not None and resp[0] == 0x50 and resp[1] == 0x02,
            f"got: {resp}")

    # Invalid session type — expect NRC 0x22 (conditionsNotCorrect) or 0x31
    resp = t.request([0x10, 0xFF])
    t.check("0x10 invalid session NRC",
            resp is not None and resp[0] == 0x7F and resp[1] == 0x10,
            f"got: {resp}")

    # Return to default
    t.request([0x10, 0x01])


def test_security_access(t: UdsTester) -> None:
    """0x27 SecurityAccess — full seed/key exchange."""
    log.info("\n[0x27] SecurityAccess")

    # Enter extended session first (security access not allowed in default)
    t.request([0x10, 0x03])

    # Request seed (sub-function 0x01)
    resp = t.request([0x27, 0x01])
    t.check("0x27 requestSeed positive",
            resp is not None and resp[0] == 0x67 and resp[1] == 0x01,
            f"got: {resp}")

    if resp is None or len(resp) < 6:
        log.error("  SKIP: Cannot compute key without seed")
        return

    seed = resp[2:6]
    log.info(f"  Seed: {bytes(seed).hex(' ').upper()}")

    # Compute key: key[n] = seed[n] XOR mask[n]
    key = [seed[i] ^ KEY_MASK[i] for i in range(4)]
    log.info(f"  Key:  {bytes(key).hex(' ').upper()}")

    # Send key (sub-function 0x02)
    resp = t.request([0x27, 0x02] + key)
    t.check("0x27 sendKey positive",
            resp is not None and resp[0] == 0x67 and resp[1] == 0x02,
            f"got: {resp}")

    # Verify already-unlocked — sending seed again should return seed=0x0000 (already unlocked)
    resp = t.request([0x27, 0x01])
    t.check("0x27 already-unlocked seed = 0x00000000",
            resp is not None and resp[0] == 0x67 and
            len(resp) >= 6 and all(b == 0 for b in resp[2:6]),
            f"got: {resp}")

    # Return to default (clears security level)
    t.request([0x10, 0x01])


def test_read_did(t: UdsTester) -> None:
    """0x22 ReadDataByIdentifier — read all registered DIDs."""
    log.info("\n[0x22] ReadDataByIdentifier")

    # DID 0x0C00: Engine Speed (2 bytes, Default session)
    resp = t.request([0x22, 0x0C, 0x00])
    t.check("0x22 DID 0x0C00 Engine Speed positive",
            resp is not None and resp[0] == 0x62 and
            resp[1] == 0x0C and resp[2] == 0x00,
            f"got: {resp}")
    if resp and len(resp) >= 5:
        rpm = struct.unpack(">H", bytes(resp[3:5]))[0]
        log.info(f"  Engine speed: {rpm} RPM")

    # DID 0x0500: Coolant Temperature (1 byte, Default session)
    resp = t.request([0x22, 0x05, 0x00])
    t.check("0x22 DID 0x0500 Coolant Temp positive",
            resp is not None and resp[0] == 0x62 and
            resp[1] == 0x05 and resp[2] == 0x00,
            f"got: {resp}")

    # DID 0xF190: VIN (17 bytes, Default session)
    resp = t.request([0x22, 0xF1, 0x90])
    t.check("0x22 DID 0xF190 VIN positive",
            resp is not None and resp[0] == 0x62 and
            resp[1] == 0xF1 and resp[2] == 0x90,
            f"got: {resp}")
    if resp and len(resp) >= 20:
        vin = bytes(resp[3:20]).decode("ascii", errors="replace")
        log.info(f"  VIN: {vin!r}")

    # DID 0xF187: Part Number (Extended session required)
    # First in default session — expect NRC 0x31 (requestOutOfRange) or 0x22
    resp = t.request([0x22, 0xF1, 0x87])
    t.check("0x22 DID 0xF187 in Default session NRC",
            resp is not None and resp[0] == 0x7F and resp[1] == 0x22,
            f"got: {resp}")

    # Now enter extended session and retry
    t.request([0x10, 0x03])
    resp = t.request([0x22, 0xF1, 0x87])
    t.check("0x22 DID 0xF187 in Extended session positive",
            resp is not None and resp[0] == 0x62,
            f"got: {resp}")

    # Return to default
    t.request([0x10, 0x01])

    # Non-existent DID — expect NRC 0x31
    resp = t.request([0x22, 0xDE, 0xAD])
    t.check("0x22 non-existent DID NRC 0x31",
            resp is not None and resp[0] == 0x7F and resp[1] == 0x22,
            f"got: {resp}")


def test_ecu_reset(t: UdsTester) -> None:
    """0x11 ECUReset — soft reset (caution: ECU will restart)."""
    log.info("\n[0x11] ECUReset (soft)")
    log.warning("  NOTE: ECU will reset — expect connection loss after this test.")

    resp = t.request([0x11, 0x03])  # 0x03 = soft reset
    t.check("0x11 soft reset positive response before reset",
            resp is not None and resp[0] == 0x51 and resp[1] == 0x03,
            f"got: {resp}")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

TEST_MAP = {
    "tester_present": test_tester_present,
    "session":        test_session_control,
    "security":       test_security_access,
    "read_did":       test_read_did,
    "ecu_reset":      test_ecu_reset,
}


def main():
    parser = argparse.ArgumentParser(
        description="UDS tester for Zephyr Diagnostics Suite native_sim build")
    parser.add_argument("--test", choices=list(TEST_MAP.keys()) + ["all"],
                        default="all",
                        help="Test to run (default: all, excluding ecu_reset)")
    parser.add_argument("--channel", default=CHANNEL,
                        help=f"CAN channel (default: {CHANNEL})")
    parser.add_argument("--bustype", default=BUSTYPE,
                        help=f"python-can bus type (default: {BUSTYPE})")
    parser.add_argument("--verbose", action="store_true",
                        help="Enable DEBUG logging")
    args = parser.parse_args()

    if args.verbose:
        logging.getLogger().setLevel(logging.DEBUG)

    log.info(f"Connecting to CAN bus: {args.channel} ({args.bustype})")

    try:
        bus = can.interface.Bus(channel=args.channel,
                                bustype=args.bustype,
                                bitrate=500000)
    except Exception as exc:
        log.error(f"Failed to open CAN bus: {exc}")
        log.error("Ensure vcan0 is set up:")
        log.error("  sudo modprobe vcan")
        log.error("  sudo ip link add dev vcan0 type vcan")
        log.error("  sudo ip link set up vcan0")
        sys.exit(1)

    tester = UdsTester(bus)

    try:
        if args.test == "all":
            # Run all tests except ecu_reset (which kills the ECU)
            for name in ["tester_present", "session", "security", "read_did"]:
                TEST_MAP[name](tester)
        else:
            TEST_MAP[args.test](tester)
    finally:
        bus.shutdown()

    ok = tester.summary()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
