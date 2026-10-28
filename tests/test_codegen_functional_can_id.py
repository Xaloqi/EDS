"""
tests/test_codegen_functional_can_id.py -- can.functional_rx_can_id (EDS-toolchain#165).

`can.rx_can_id` is the physical request ID (default 0x7E0). The functional one
(ISO 15765-4, default 0x7DF) is its own key and becomes
isotp_cfg_t.functional_rx_can_id. An older config whose rx_can_id is still
0x7DF keeps it as the physical ID and gets no functional ID.

Run:  pytest tests/test_codegen_functional_can_id.py -v
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest

_TOOLS = Path(__file__).resolve().parent.parent / "tools"
sys.path.insert(0, str(_TOOLS))
_spec = importlib.util.spec_from_file_location("codegen_fn_under_test", _TOOLS / "codegen.py")
codegen = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(codegen)


@pytest.mark.parametrize("can,expected_rx,expected_fn", [
    ({"rx_can_id": "0x7E0", "tx_can_id": "0x7E8"}, 0x7E0, 0x7DF),
    ({}, 0x7E0, 0x7DF),
    ({"rx_can_id": "0x7E1", "tx_can_id": "0x7E9"}, 0x7E1, 0x7DF),
    ({"rx_can_id": "0x7E0", "tx_can_id": "0x7E8",
      "functional_rx_can_id": "0x7DE"}, 0x7E0, 0x7DE),
    ({"rx_can_id": "0x7E0", "tx_can_id": "0x7E8",
      "functional_rx_can_id": "none"}, 0x7E0, 0),
    ({"rx_can_id": "0x7DF", "tx_can_id": "0x7E8"}, 0x7DF, 0),            # legacy config
    ({"rx_can_id": "0x18DA00F1", "tx_can_id": "0x18DAF100"}, 0x18DA00F1, 0),  # 29-bit
])
def test_context(can, expected_rx, expected_fn):
    ctx = codegen._build_can_context({"can": can})
    assert ctx["rx_can_id_int"] == expected_rx
    assert ctx["functional_rx_can_id_int"] == expected_fn


_VALID = {
    "schema_version": 1,
    "metadata": {"ecu_name": "FnECU", "version": "1.0.0"},
    "timing": {"p2_server_max_ms": 25, "p2_star_server_max_ms": 5000,
               "s3_server_timeout_ms": 5000},
    "dids": [{"id": "0xF190", "name": "VIN", "data_length": 17, "access": ["read"],
              "min_session": "default", "read_security_level": 0,
              "write_security_level": 0}],
    "dtcs": [],
}


def test_valid_config_passes_validation():
    # Guards the refusal test below: it must fail for the CAN ID, not for
    # a hole in this fixture.
    codegen.validate_config({**_VALID, "can": {"rx_can_id": "0x7E0", "tx_can_id": "0x7E8"}})


@pytest.mark.parametrize("clash", ["rx_can_id", "tx_can_id"])
def test_functional_id_equal_to_a_physical_id_is_refused(clash, capsys):
    can = {"rx_can_id": "0x7E0", "tx_can_id": "0x7E8"}
    can["functional_rx_can_id"] = can[clash]
    with pytest.raises(SystemExit):
        codegen.validate_config({**_VALID, "can": can})
    assert "functional_rx_can_id must differ" in capsys.readouterr().err
