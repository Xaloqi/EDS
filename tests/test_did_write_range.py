"""
tests/test_did_write_range.py -- `write_range` on a DID (EDS-toolchain#253).

An app-owned writable DID whose handler enforces a range had no way to say so:
the range lived in a YAML comment, so the generated write tests sent an
arbitrary pattern and got NRC 0x31. `write_range: {min, max}` declares it.
Validation only; what testgen does with it is tested in EDS-toolchain.

Run:  pytest tests/test_did_write_range.py -v
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest
import yaml

_REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_REPO_ROOT / "tools"))
import config_parser  # noqa: E402


def _did(**kw):
    d = {"id": "0x4200", "name": "A", "access": ["read", "write"], "data_length": 2,
         "min_session": "default", "read_security_level": 0, "write_security_level": 0,
         "source": "app", "write_range": {"min": 10, "max": 20}}
    d.update(kw)
    return {
        "schema_version": 1,
        "metadata": {"ecu_name": "RangeECU", "version": "1.0.0"},
        "timing": {"p2_server_max_ms": 25, "p2_star_server_max_ms": 5000,
                   "s3_server_timeout_ms": 5000},
        "can": {"interface": "vcan0", "rx_can_id": "0x7E0", "tx_can_id": "0x7E8"},
        "dids": [d],
    }


def _errors(**kw):
    return config_parser.validate_config(_did(**kw))


def test_valid_range_is_accepted() -> None:
    assert _errors() == []


def test_range_may_be_a_single_value() -> None:
    assert _errors(write_range={"min": 7, "max": 7}) == []


@pytest.mark.parametrize("bad", [
    {"min": 10}, {"max": 20}, {"min": 1, "max": 2, "step": 1},
    {"min": -1, "max": 2}, {"min": "a", "max": 2}, [10, 20],
])
def test_malformed_range_is_rejected(bad) -> None:
    assert _errors(write_range=bad)


def test_min_above_max_is_rejected() -> None:
    assert any("above max" in e for e in _errors(write_range={"min": 21, "max": 20}))


def test_max_must_fit_data_length() -> None:
    assert any("does not fit" in e for e in _errors(write_range={"min": 0, "max": 65536}))
    assert _errors(write_range={"min": 0, "max": 65535}) == []


def test_range_needs_write_access() -> None:
    assert any("'write' in 'access'" in e for e in _errors(access=["read"]))


def test_range_is_for_app_owned_dids_only() -> None:
    # A generated handler stores any value; a declared range would be a promise
    # nothing keeps.
    assert any("source: app" in e for e in _errors(source="stub"))
    cfg = _did()
    del cfg["dids"][0]["source"]
    assert any("source: app" in e for e in config_parser.validate_config(cfg))


def test_bms_ecu_declares_the_ranges_its_handlers_enforce() -> None:
    cfg = yaml.safe_load((_REPO_ROOT / "examples/bms_ecu/diagnostics_config.yaml").read_text())
    got = {d["id"]: d["write_range"] for d in cfg["dids"] if "write_range" in d}
    src = (_REPO_ROOT / "examples/bms_ecu/src/bms_sim.c").read_text()

    def const(name: str) -> int:
        import re
        return int(re.search(rf"#define {name}\s+\((\d+)U\)", src).group(1))

    assert got == {
        "0xD910": {"min": const("BMS_OV_MIN_MV"), "max": const("BMS_OV_MAX_MV")},
        "0xD911": {"min": const("BMS_UV_MIN_MV"), "max": const("BMS_UV_MAX_MV")},
    }
