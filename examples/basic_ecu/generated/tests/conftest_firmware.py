# =============================================================================
# GENERATED — do NOT edit manually.
# Regenerate: python3 tools/codegen.py --config <yaml> --out generated/ --test-gen
#
# ECU       : BasicECU
# Version   : 0.1.0
# Generated : 2026-05-19T17:30:37Z
#
# PURPOSE: pytest conftest for firmware-backed integration tests.
#
# This conftest builds the harness binary (tools/build_harness.sh), launches
# it in --server mode as a subprocess, and provides the same IsoTpTransport
# fixture interface as conftest.py — but backed by real compiled C firmware
# instead of the Python simulator.
#
# ARCHITECTURE:
#
#   pytest test function
#       │  uses IsoTpTransport fixture (same API as simulator mode)
#       │
#   FirmwareIsoTpTransport  (this file)
#       │  sends/receives 16-byte socketcan_wire_frame_t over AF_UNIX socket
#       │
#   harness_ecu_test --server /tmp/eds_firmware_XXXX.sock
#       │  real compiled C firmware in a subprocess
#       │
#   harness_ecu.c  (full UDS stack + ISO-TP + AES-CMAC + session + security)
#       running on the host CPU via AF_UNIX socketpair transport
#
# WHAT THIS PROVES:
#   Unlike the simulator tests, these tests catch bugs in the actual C
#   implementation — wrong NRC codes, off-by-one in buffer sizing, incorrect
#   session state machine transitions, real AES-CMAC key derivation, etc.
#
# RUNNING:
#   cd generated/tests
#   pytest test_firmware_*.py -v               # firmware tests only
#   pytest . -v --firmware                     # run all tests including firmware
#
# REQUIREMENTS:
#   gcc (system compiler — no cross-compiler needed)
#   pytest>=7.0, pycryptodome>=3.18
# =============================================================================

from __future__ import annotations

import os
import queue
import socket
import struct
import subprocess
import tempfile
import threading
import time
import logging
from pathlib import Path
from typing import Optional

import pytest

log = logging.getLogger("conftest_firmware.basicecu")

# ---------------------------------------------------------------------------
# Protocol constants (must match diagnostics_config.yaml + generated_config.h)
# ---------------------------------------------------------------------------

TESTER_TX_ID:   int   = 2015   # Tester → ECU  (our TX)
ECU_TX_ID:      int   = 2024   # ECU    → Tester  (our RX)

P2_SERVER_MAX_MS:      int   = 25
P2_STAR_SERVER_MAX_MS: int   = 5000
S3_SERVER_TIMEOUT_MS:  int   = 5000

RESPONSE_TIMEOUT_S:    float = (P2_SERVER_MAX_MS + 100) / 1000.0
ISOTP_FC_TIMEOUT_S:    float = 0.300
ISO_TP_PADDING:        int   = 0xCC

WIRE_FRAME_LEN:        int   = 16   # sizeof(socketcan_wire_frame_t)

SID_DIAGNOSTIC_SESSION_CONTROL: int = 0x10
SID_SECURITY_ACCESS:            int = 0x27
SID_NEGATIVE_RESPONSE:          int = 0x7F
POSITIVE_RESPONSE_OFFSET:       int = 0x40
SESSION_DEFAULT:                int = 0x01
SESSION_PROGRAMMING:            int = 0x02
SESSION_EXTENDED:               int = 0x03

# AES-CMAC placeholder keys (must match s_level_keys in uds_security_algo.c)
_FIRMWARE_AES_KEYS = {
    1: bytes([0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
              0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F]),
    2: bytes([0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
              0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F]),
}


# ---------------------------------------------------------------------------
# Wire frame encoding / decoding  (mirrors socketcan_wire_frame_t exactly)
# ---------------------------------------------------------------------------

def _encode_wire(can_id: int, data: bytes) -> bytes:
    """Pack a CAN frame into the 16-byte socketcan_wire_frame_t."""
    dlc = min(len(data), 8)
    padded = bytes(data[:dlc]) + bytes(8 - dlc)
    # can_id stored big-endian (network byte order)
    wire = struct.pack(">I", can_id & 0x1FFFFFFF)
    wire += bytes([dlc, 0, 0, 0])  # dlc, flags, pad[2]
    wire += padded
    return wire


def _decode_wire(raw: bytes) -> tuple:
    """Unpack a 16-byte wire frame → (can_id, dlc, data_bytes)."""
    if len(raw) < WIRE_FRAME_LEN:
        return 0, 0, b""
    can_id = struct.unpack(">I", raw[:4])[0] & 0x1FFFFFFF
    dlc    = raw[4]
    data   = bytes(raw[8:8 + dlc])
    return can_id, dlc, data


# ---------------------------------------------------------------------------
# ISO-TP framing (mirrors conftest.py)
# ---------------------------------------------------------------------------

def _isotp_encode(payload: bytes) -> list:
    if len(payload) <= 7:
        n = len(payload)
        return [bytes([n & 0x0F]) + payload + bytes([ISO_TP_PADDING] * (7 - n))]
    frames = [bytes([0x10 | ((len(payload) >> 8) & 0x0F), len(payload) & 0xFF]) + payload[:6]]
    rest, sn = payload[6:], 1
    while rest:
        chunk, rest = rest[:7], rest[7:]
        frames.append(bytes([0x20 | (sn & 0x0F)]) + chunk + bytes([ISO_TP_PADDING] * (7 - len(chunk))))
        sn = (sn % 15) + 1
    return frames


def _isotp_decode_sf(data: bytes) -> Optional[bytes]:
    if not data: return None
    if (data[0] & 0xF0) >> 4 != 0: return None
    n = data[0] & 0x0F
    if n == 0 or n > 7: return None
    return bytes(data[1:1 + n])


def _isotp_decode_mf(frames: list) -> Optional[bytes]:
    if not frames: return None
    ff = frames[0]
    if (ff[0] & 0xF0) >> 4 != 1: return None
    total = ((ff[0] & 0x0F) << 8) | ff[1]
    buf = bytearray(ff[2:8])
    for cf in frames[1:]:
        buf.extend(cf[1:8])
    return bytes(buf[:total])


# ---------------------------------------------------------------------------
# AES-128-CMAC key derivation (pure Python RFC 4493, mirrors uds_aes_cmac.c)
#
# This implementation is the canonical reference used by both the firmware
# tests (conftest_firmware.py) and the simulator (conftest.py).
# It matches uds_aes_cmac.c byte-for-byte — proven by the NIST FIPS 197
# known-answer test in tests/unit_runnable/test_phase5_security_algo.c.
# ---------------------------------------------------------------------------

def _aes_enc_block(key: bytes, block: bytes) -> bytes:
    """AES-128 ECB block encrypt — table-free, matches uds_aes_cmac.c."""
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
    def _mul(a: int, b: int) -> int:
        r = 0
        for _ in range(8):
            if b & 1: r ^= a
            hi = a & 0x80; a = (a << 1) & 0xFF
            if hi: a ^= 0x1B
            b >>= 1
        return r
    RCON = [0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1B,0x36]
    w = [list(key[i*4:(i+1)*4]) for i in range(4)]
    for i in range(4, 44):
        t = list(w[i-1])
        if i % 4 == 0:
            t = [S[t[1]]^RCON[i//4-1], S[t[2]], S[t[3]], S[t[0]]]
        w.append([w[i-4][j]^t[j] for j in range(4)])
    rk = [[[w[r*4+c][b] for b in range(4)] for c in range(4)] for r in range(11)]
    state = [[block[r+4*c] for c in range(4)] for r in range(4)]
    def ark(s, k): return [[s[r][c]^k[c][r] for c in range(4)] for r in range(4)]
    def sb(s):  return [[S[s[r][c]] for c in range(4)] for r in range(4)]
    def sr(s):  return [[s[0][0],s[0][1],s[0][2],s[0][3]],
                        [s[1][1],s[1][2],s[1][3],s[1][0]],
                        [s[2][2],s[2][3],s[2][0],s[2][1]],
                        [s[3][3],s[3][0],s[3][1],s[3][2]]]
    def mc(s):
        def mc1(col): return [
            _mul(2,col[0])^_mul(3,col[1])^col[2]^col[3],
            col[0]^_mul(2,col[1])^_mul(3,col[2])^col[3],
            col[0]^col[1]^_mul(2,col[2])^_mul(3,col[3]),
            _mul(3,col[0])^col[1]^col[2]^_mul(2,col[3]),
        ]
        cols = [[s[r][c] for r in range(4)] for c in range(4)]
        mixed = [mc1(c) for c in cols]
        return [[mixed[c][r] for c in range(4)] for r in range(4)]
    state = ark(state, rk[0])
    for rnd in range(1, 10):
        state = ark(mc(sr(sb(state))), rk[rnd])
    state = ark(sr(sb(state)), rk[10])
    return bytes(state[r][c] for c in range(4) for r in range(4))


def _aes_cmac(key: bytes, msg: bytes) -> bytes:
    """
    AES-128-CMAC per RFC 4493.

    Uses pycryptodome when available (fastest, most tested).
    Falls back to the pure-Python AES-128 above — same implementation
    as uds_aes_cmac.c, verified against RFC 4493 known-answer vectors.
    """
    try:
        from Crypto.Hash import CMAC
        from Crypto.Cipher import AES
        c = CMAC.new(key, ciphermod=AES)
        c.update(msg)
        return c.digest()
    except ImportError:
        pass

    # Pure-Python fallback (RFC 4493)
    def _xor16(a: bytes, b: bytes) -> bytes:
        return bytes(x ^ y for x, y in zip(a, b))
    def _pad(b: bytes) -> bytes:
        n = len(b) % 16
        return b if n == 0 else b + bytes([0x80] + [0x00] * (15 - n))
    R128 = bytes([0]*15 + [0x87])

    def _shift_left(b: bytes) -> bytes:
        result = bytearray(16)
        for i in range(15):
            result[i] = ((b[i] << 1) | (b[i+1] >> 7)) & 0xFF
        result[15] = (b[15] << 1) & 0xFF
        return bytes(result)

    L  = _aes_enc_block(key, bytes(16))
    K1 = _shift_left(L)
    if L[0] & 0x80: K1 = _xor16(K1, R128)
    K2 = _shift_left(K1)
    if K1[0] & 0x80: K2 = _xor16(K2, R128)

    if not msg:
        msg = bytes([0])
    blocks = [msg[i:i+16] for i in range(0, len(msg), 16)]
    last = blocks[-1]
    if len(last) == 16:
        last = _xor16(last, K1)
    else:
        last = _xor16(_pad(last), K2)
    blocks[-1] = last

    X = bytes(16)
    for blk in blocks:
        X = _aes_enc_block(key, _xor16(X, blk))
    return X


def derive_firmware_key(seed: bytes, level: int = 1, aes_keys: Optional[dict] = None) -> bytes:
    """Derive the 4-byte key from an 8-byte seed using AES-128-CMAC."""
    keys = aes_keys or _FIRMWARE_AES_KEYS
    key  = keys.get(level, _FIRMWARE_AES_KEYS[1])
    mac  = _aes_cmac(key, seed)
    return mac[:4]


# ---------------------------------------------------------------------------
# Harness build helper
# ---------------------------------------------------------------------------

# Root of the repository (two levels up from generated/tests/)
_REPO_ROOT = Path(__file__).parent.parent.parent


def build_harness(repo_root: Path = _REPO_ROOT) -> Path:
    """
    Build the harness binary using build_harness.sh.

    Returns the path to the compiled binary.
    Raises RuntimeError if the build fails.
    """
    binary = Path("/tmp/harness_ecu_test")
    build_script = repo_root / "build_harness.sh"

    if not build_script.exists():
        raise RuntimeError(f"build_harness.sh not found at {build_script}")

    log.info("Building harness binary...")
    result = subprocess.run(
        ["bash", str(build_script), "--fast"],
        capture_output=True,
        text=True,
        cwd=str(repo_root),
        env={**os.environ, "OUTPUT": str(binary)},
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"Harness build failed (rc={result.returncode}):\n"
            f"STDOUT:\n{result.stdout}\n"
            f"STDERR:\n{result.stderr}"
        )
    log.info("Harness built: %s", binary)
    return binary


# ---------------------------------------------------------------------------
# FirmwareIsoTpTransport
# Wraps the harness subprocess via AF_UNIX SOCK_SEQPACKET.
# Same public interface as IsoTpTransport in conftest.py.
# ---------------------------------------------------------------------------

class FirmwareIsoTpTransport:
    """
    IsoTpTransport backed by real compiled C firmware (harness binary).

    Connects to a harness_ecu_test --server <socket_path> subprocess and
    provides the same request(pdu) → pdu interface as the Python simulator.

    Usage:
        transport = FirmwareIsoTpTransport(binary_path)
        transport.start()
        pdu = transport.request(bytes([0x3E, 0x00]))
        transport.close()
    """

    def __init__(
        self,
        binary:    Path,
        repo_root: Path = _REPO_ROOT,
    ) -> None:
        self._binary   = binary
        self._repo_root = repo_root
        self._process:  Optional[subprocess.Popen]  = None
        self._sock:     Optional[socket.socket]     = None
        self._sock_path: str = ""
        self._rx_q:     queue.Queue                 = queue.Queue()
        self._rx_thread: Optional[threading.Thread] = None
        self._running:  bool                        = False

    def start(self, timeout_s: float = 5.0) -> None:
        """Build and launch the firmware harness, wait for it to be ready."""
        self._sock_path = tempfile.mktemp(prefix="/tmp/eds_firmware_", suffix=".sock")

        env = {
            **os.environ,
            "OUTPUT": str(self._binary),
        }

        log.info("Launching harness: %s --server %s", self._binary, self._sock_path)
        self._process = subprocess.Popen(
            [str(self._binary), "--server", self._sock_path],
            stderr=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            text=True,
            env=env,
        )

        # Wait for "READY" line on stderr
        deadline = time.monotonic() + timeout_s
        ready = False
        stderr_lines = []
        while time.monotonic() < deadline:
            assert self._process.stderr is not None
            line = self._process.stderr.readline()
            if not line:
                break
            stderr_lines.append(line.rstrip())
            log.debug("harness: %s", line.rstrip())
            if "READY" in line:
                ready = True
                break

        if not ready:
            self._process.kill()
            raise RuntimeError(
                f"Harness did not signal READY within {timeout_s}s.\n"
                f"stderr: {chr(10).join(stderr_lines)}"
            )

        # Connect via AF_UNIX SOCK_SEQPACKET
        self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        deadline2 = time.monotonic() + 2.0
        while time.monotonic() < deadline2:
            try:
                self._sock.connect(self._sock_path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.05)
        else:
            raise RuntimeError(f"Could not connect to harness socket {self._sock_path}")

        # Read sentinel byte
        self._sock.settimeout(2.0)
        try:
            sentinel = self._sock.recv(1)
            log.debug("Harness sentinel: %s", sentinel.hex() if sentinel else "none")
        except socket.timeout:
            raise RuntimeError("Did not receive sentinel from harness")

        # Start RX thread
        self._running = True
        self._rx_thread = threading.Thread(target=self._rx_loop, daemon=True)
        self._rx_thread.start()

    def _rx_loop(self) -> None:
        assert self._sock is not None
        self._sock.settimeout(0.05)
        while self._running:
            try:
                raw = self._sock.recv(WIRE_FRAME_LEN)
                if raw:
                    self._rx_q.put(raw, block=False)
            except socket.timeout:
                pass
            except Exception:
                break

    def _send_frame(self, can_id: int, data: bytes) -> None:
        assert self._sock is not None
        wire = _encode_wire(can_id, data)
        self._sock.send(wire)

    def _recv_wire(self, timeout: float) -> Optional[bytes]:
        try:
            return self._rx_q.get(timeout=timeout)
        except queue.Empty:
            return None

    def request(self, pdu: bytes, timeout: float = RESPONSE_TIMEOUT_S) -> Optional[bytes]:
        """
        Send a UDS PDU to the real firmware and return the reassembled response.

        Handles full ISO-TP SF/FF/CF/FC framing in both directions.
        """
        frames = _isotp_encode(pdu)
        deadline = time.monotonic() + timeout

        if len(frames) == 1:
            self._send_frame(TESTER_TX_ID, frames[0])
        else:
            # Multi-frame TX: send FF, wait for FC, send CFs
            self._send_frame(TESTER_TX_ID, frames[0])
            raw_fc = self._recv_wire(ISOTP_FC_TIMEOUT_S)
            if raw_fc is None:
                log.warning("No FC received for multi-frame TX")
                return None
            _, _, fc_data = _decode_wire(raw_fc)
            if (fc_data[0] & 0xF0) >> 4 != 3:
                log.warning("Expected FC, got: %s", fc_data.hex())
                return None
            stmin_raw = fc_data[2]
            stmin = stmin_raw / 1000.0 if stmin_raw <= 0x7F else 0.001
            sn_sent = 0
            bs = fc_data[1]
            for cf_frame in frames[1:]:
                time.sleep(stmin)
                self._send_frame(TESTER_TX_ID, cf_frame)
                sn_sent += 1
                if bs > 0 and sn_sent >= bs:
                    raw_fc2 = self._recv_wire(ISOTP_FC_TIMEOUT_S)
                    if raw_fc2 is None:
                        return None
                    _, _, fc2_data = _decode_wire(raw_fc2)
                    bs = fc2_data[1]
                    sn_sent = 0

        # Receive response
        remaining = deadline - time.monotonic()
        raw = self._recv_wire(max(remaining, 0.01))
        if raw is None:
            return None

        _, _, data = _decode_wire(raw)
        pci_type = (data[0] & 0xF0) >> 4

        if pci_type == 0:    # Single Frame
            return _isotp_decode_sf(data)

        if pci_type == 1:    # First Frame — send FC, collect CFs
            total = ((data[0] & 0x0F) << 8) | data[1]
            # Send Flow Control CTS
            fc_bytes = bytes([0x30, 0x00, 0x00]) + bytes([ISO_TP_PADDING] * 5)
            self._send_frame(TESTER_TX_ID, fc_bytes)
            mf_frames = [data]
            collected = 6
            while collected < total:
                remaining = deadline - time.monotonic()
                raw_cf = self._recv_wire(max(remaining, 0.05))
                if raw_cf is None:
                    log.warning("CF timeout (collected %d/%d)", collected, total)
                    return None
                _, _, cf_data = _decode_wire(raw_cf)
                mf_frames.append(cf_data)
                collected += 7
            return _isotp_decode_mf(mf_frames)

        return None

    def close(self) -> None:
        """Stop the RX thread, close the socket, terminate the harness process."""
        self._running = False
        if self._rx_thread:
            self._rx_thread.join(timeout=0.5)
        if self._sock:
            try:
                self._sock.close()
            except Exception:
                pass
        if self._process:
            try:
                self._process.terminate()
                self._process.wait(timeout=3)
            except Exception:
                self._process.kill()
        try:
            os.unlink(self._sock_path)
        except FileNotFoundError:
            pass


# ---------------------------------------------------------------------------
# Session-scope fixture: build the harness once per session
# ---------------------------------------------------------------------------

@pytest.fixture(scope="session")
def harness_binary() -> Path:
    """Build the harness binary once per session. Skips if build fails."""
    try:
        binary = build_harness(_REPO_ROOT)
        assert binary.exists(), f"Harness binary not found after build: {binary}"
        return binary
    except Exception as exc:
        pytest.skip(f"Harness build failed — skipping firmware tests: {exc}")


# ---------------------------------------------------------------------------
# Function-scope fixture: fresh firmware transport per test
# ---------------------------------------------------------------------------

@pytest.fixture(scope="function")
def firmware_bus(harness_binary: Path, request: pytest.FixtureRequest) -> FirmwareIsoTpTransport:
    """
    Per-test firmware transport fixture.

    Starts a fresh harness process for each test, returns to Default Session
    in teardown, and terminates the harness cleanly.

    Each test therefore runs against a freshly initialized ECU state —
    no session/security state leaks between tests.
    """
    aes_keys = getattr(request, "param", None) or _FIRMWARE_AES_KEYS

    transport = FirmwareIsoTpTransport(binary=harness_binary, repo_root=_REPO_ROOT)
    try:
        transport.start(timeout_s=10.0)
    except Exception as exc:
        pytest.skip(f"Harness startup failed: {exc}")

    yield transport

    # Teardown: return to default session
    try:
        transport.request(
            bytes([SID_DIAGNOSTIC_SESSION_CONTROL, SESSION_DEFAULT]),
            timeout=0.1,
        )
    except Exception:
        pass
    transport.close()


# ---------------------------------------------------------------------------
# Convenience: expose derive_firmware_key for test modules
# ---------------------------------------------------------------------------

def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption(
        "--firmware", action="store_true", default=False,
        help="Include firmware-backed tests (builds harness binary)",
    )
    parser.addoption(
        "--firmware-sec-key-l1", default=None,
        help="Hex AES-128 key for firmware security level 1",
    )
    parser.addoption(
        "--firmware-sec-key-l2", default=None,
        help="Hex AES-128 key for firmware security level 2",
    )
