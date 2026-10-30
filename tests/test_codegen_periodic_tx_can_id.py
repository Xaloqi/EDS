"""
tests/test_codegen_periodic_tx_can_id.py -- can.periodic_tx_can_id (EDS#546).

SID 0x2A's periodic messages are UUDT frames on a CAN ID of their own (ISO
14229-3:2022 §8.5, Table 3: "a different CAN identifier for periodic
transmission"; no PCI, no SID, one unsegmented frame). There is no default ID:

  - a config with a periodic DID (0xF200-0xF2FF) and no can.periodic_tx_can_id
    is refused at codegen time,
  - the ID must differ from the physical, response and functional IDs,
  - a periodic DID's record must fit one classic CAN frame with the pDID (<= 7).

Run:  pytest tests/test_codegen_periodic_tx_can_id.py -v
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest

_TOOLS = Path(__file__).resolve().parent.parent / "tools"
sys.path.insert(0, str(_TOOLS))
_spec = importlib.util.spec_from_file_location("codegen_ptx_under_test", _TOOLS / "codegen.py")
codegen = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(codegen)

_CAN = {"rx_can_id": "0x7E0", "tx_can_id": "0x7E8"}


def _cfg(can, did_id="0xF190", length=4):
    return {
        "schema_version": 1,
        "metadata": {"ecu_name": "PtxECU", "version": "1.0.0"},
        "timing": {"p2_server_max_ms": 25, "p2_star_server_max_ms": 5000,
                   "s3_server_timeout_ms": 5000},
        "can": can,
        "dids": [{"id": did_id, "name": "D", "data_length": length, "access": ["read"],
                  "min_session": "default", "read_security_level": 0,
                  "write_security_level": 0}],
        "dtcs": [],
    }


def test_context_has_no_default():
    assert codegen._build_can_context({"can": _CAN})["periodic_tx_can_id_int"] == 0
    assert codegen._build_can_context({"can": _CAN})["periodic_tx_can_id"] == "none"
    assert codegen._build_can_context({})["periodic_tx_can_id_int"] == 0


def test_context_carries_the_configured_id():
    ctx = codegen._build_can_context({"can": {**_CAN, "periodic_tx_can_id": "0x5E8"}})
    assert ctx["periodic_tx_can_id_int"] == 0x5E8


def test_a_config_without_a_periodic_did_needs_no_id():
    codegen.validate_config(_cfg(_CAN))                      # 0xF190: not periodic
    codegen.validate_config(_cfg({**_CAN, "periodic_tx_can_id": "none"}))


@pytest.mark.parametrize("can", [_CAN, {**_CAN, "periodic_tx_can_id": "none"}])
def test_a_periodic_did_without_an_id_is_refused(can, capsys):
    with pytest.raises(SystemExit):
        codegen.validate_config(_cfg(can, did_id="0xF200"))
    err = capsys.readouterr().err
    assert "needs can.periodic_tx_can_id" in err and "no default" in err


def test_a_periodic_did_with_an_id_passes():
    codegen.validate_config(_cfg({**_CAN, "periodic_tx_can_id": "0x5E8"}, did_id="0xF200", length=7))


def test_a_periodic_record_over_7_bytes_is_refused(capsys):
    with pytest.raises(SystemExit):
        codegen.validate_config(_cfg({**_CAN, "periodic_tx_can_id": "0x5E8"},
                                     did_id="0xF2AB", length=8))
    assert "at most 7 bytes" in capsys.readouterr().err


@pytest.mark.parametrize("clash", ["rx_can_id", "tx_can_id", "functional_rx_can_id"])
def test_the_periodic_id_must_be_its_own(clash, capsys):
    can = {**_CAN, "functional_rx_can_id": "0x7DF"}
    can["periodic_tx_can_id"] = can[clash]
    with pytest.raises(SystemExit):
        codegen.validate_config(_cfg(can))
    assert "periodic_tx_can_id must differ" in capsys.readouterr().err
