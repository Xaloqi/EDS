# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Xaloqi
"""
[EDS#641] scripts/e2e_isotp_burst.py waits and retries a frame that the
SocketCAN transmit queue refuses (ENOBUFS).

slcand creates can0 with txqueuelen 10 and one request is about 570 frames, so
on a CANable the script used to die in round 1 with "No buffer space
available" before the ECU saw a complete request. The bus here is a stub that
refuses the first frames the way the kernel does.
"""

import errno
import importlib.util
from pathlib import Path

import pytest

can = pytest.importorskip("can")

SCRIPT = Path(__file__).resolve().parent.parent / "scripts" / "e2e_isotp_burst.py"


def _load():
    spec = importlib.util.spec_from_file_location("e2e_isotp_burst", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class _BusyBus:
    """Refuses `busy` sends with ENOBUFS, then accepts."""

    def __init__(self, busy, code=errno.ENOBUFS):
        self.busy, self.code, self.sent, self.attempts = busy, code, [], 0

    def send(self, msg):
        self.attempts += 1
        if self.attempts <= self.busy:
            raise can.CanOperationError(
                "Failed to transmit: No buffer space available", self.code)
        self.sent.append(msg)


def test_frame_is_retried_until_the_queue_drains():
    mod = _load()
    bus = _BusyBus(busy=5)
    mod.IsoTpSender(bus, fd=False)._send(bytes([0x21, 1, 2, 3]))
    assert bus.attempts == 6 and len(bus.sent) == 1


def test_gives_up_when_the_queue_never_drains(monkeypatch):
    mod = _load()
    monkeypatch.setattr(mod, "SEND_BUSY_TIMEOUT_S", 0.05)
    with pytest.raises(can.CanOperationError):
        mod.IsoTpSender(_BusyBus(busy=10**9), fd=False)._send(bytes([0x21, 1]))


def test_other_send_errors_are_not_retried():
    mod = _load()
    bus = _BusyBus(busy=10**9, code=errno.EIO)
    with pytest.raises(can.CanOperationError):
        mod.IsoTpSender(bus, fd=False)._send(bytes([0x21, 1]))
    assert bus.attempts == 1
