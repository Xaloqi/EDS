"""
tests/test_codegen_canfd.py -- can.fd / can.fd_tx_dl / can.fd_brs (EDS#557).

A CAN FD channel is configured in the YAML, not by hand in uds_init.c:

  - can.fd (bool), can.fd_tx_dl (a CAN FD data length, default 64) and
    can.fd_brs (bool, default false) reach the template context;
  - fd_tx_dl / fd_brs without fd: true, a non-bool fd, and a TX_DL that is not
    a CAN FD data length are refused;
  - with fd, a periodic DID's record may be TX_DL - 1 bytes (one unsegmented
    FD frame with the pDID); 7 on classic CAN, as before;
  - without can.fd the context says classic (the templates then emit nothing
    FD-specific: the toolchain checks the rendered output).

Run:  pytest tests/test_codegen_canfd.py -v
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest

_TOOLS = Path(__file__).resolve().parent.parent / "tools"
sys.path.insert(0, str(_TOOLS))
_spec = importlib.util.spec_from_file_location("codegen_canfd_under_test", _TOOLS / "codegen.py")
codegen = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(codegen)

_CAN = {"rx_can_id": "0x7E0", "tx_can_id": "0x7E8", "periodic_tx_can_id": "0x5E8"}


def _cfg(can, did_id="0xF190", length=4):
    return {
        "schema_version": 1,
        "metadata": {"ecu_name": "FdECU", "version": "1.0.0"},
        "timing": {"p2_server_max_ms": 25, "p2_star_server_max_ms": 5000,
                   "s3_server_timeout_ms": 5000},
        "can": can,
        "dids": [{"id": did_id, "name": "D", "data_length": length, "access": ["read"],
                  "min_session": "default", "read_security_level": 0,
                  "write_security_level": 0}],
        "dtcs": [],
    }


def test_classic_by_default():
    ctx = codegen._build_can_context({"can": _CAN})
    assert (ctx["fd"], ctx["fd_tx_dl"], ctx["fd_brs"]) == (False, 8, False)
    assert codegen._build_can_context({})["fd"] is False


def test_fd_defaults_and_values():
    ctx = codegen._build_can_context({"can": {**_CAN, "fd": True}})
    assert (ctx["fd"], ctx["fd_tx_dl"], ctx["fd_brs"]) == (True, 64, False)
    ctx = codegen._build_can_context({"can": {**_CAN, "fd": True, "fd_tx_dl": 16, "fd_brs": True}})
    assert (ctx["fd"], ctx["fd_tx_dl"], ctx["fd_brs"]) == (True, 16, True)


@pytest.mark.parametrize("tx_dl", [8, 12, 16, 20, 24, 32, 48, 64])
def test_every_fd_data_length_is_accepted(tx_dl):
    codegen.validate_config(_cfg({**_CAN, "fd": True, "fd_tx_dl": tx_dl}))


@pytest.mark.parametrize("can", [
    {**_CAN, "fd": "yes"},
    {**_CAN, "fd_tx_dl": 64},
    {**_CAN, "fd_brs": True},
    {**_CAN, "fd": False, "fd_brs": True},
    {**_CAN, "fd": True, "fd_tx_dl": 10},
    {**_CAN, "fd": True, "fd_tx_dl": 65},
    {**_CAN, "fd": True, "fd_tx_dl": True},
    {**_CAN, "fd": True, "fd_brs": "on"},
])
def test_bad_fd_keys_are_refused(can):
    with pytest.raises(SystemExit):
        codegen.validate_config(_cfg(can))


def test_periodic_record_limit_follows_tx_dl():
    codegen.validate_config(_cfg(_CAN, "0xF201", 7))
    with pytest.raises(SystemExit):
        codegen.validate_config(_cfg(_CAN, "0xF201", 8))              # classic: 7
    codegen.validate_config(_cfg({**_CAN, "fd": True}, "0xF201", 63))
    with pytest.raises(SystemExit):
        codegen.validate_config(_cfg({**_CAN, "fd": True}, "0xF201", 64))
    codegen.validate_config(_cfg({**_CAN, "fd": True, "fd_tx_dl": 24}, "0xF201", 23))
    with pytest.raises(SystemExit):
        codegen.validate_config(_cfg({**_CAN, "fd": True, "fd_tx_dl": 24}, "0xF201", 24))
