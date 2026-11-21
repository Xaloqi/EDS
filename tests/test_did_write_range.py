"""
tests/test_did_write_range.py -- `write_range` on a DID (EDS-toolchain#253, #257).

An app-owned writable DID whose handler enforces a range had no way to say so:
the range lived in a YAML comment, so the generated write tests sent an
arbitrary pattern and got NRC 0x31. `write_range: {min, max}` declares it, and
since EDS-toolchain#257 the stack enforces it (uds_safety_check_write_range(),
REQ-SAFE-008) for generated and app-owned handlers alike. Validation and the
codegen context here; the C check is tested in tests/unit_runnable, what testgen
does with the range in EDS-toolchain.

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


def test_range_is_allowed_on_generated_handlers() -> None:
    # [EDS-toolchain#257] the stack enforces the range, so a stub DID may carry one.
    assert _errors(source="stub") == []
    cfg = _did()
    del cfg["dids"][0]["source"]
    assert config_parser.validate_config(cfg) == []


def test_range_needs_data_length_up_to_4() -> None:
    # uds_safety_check_write_range() reads the value as a uint32_t.
    assert _errors(data_length=4, write_range={"min": 0, "max": 2**32 - 1}) == []
    assert any("data_length 1 to 4" in e
               for e in _errors(data_length=5, write_range={"min": 0, "max": 1}))


def test_codegen_passes_the_range_to_the_templates() -> None:
    import codegen
    by_id = {d["id"]: d for d in codegen._build_did_list(_did())}
    assert by_id["0x4200"]["write_range"] == {"min": 10, "max": 20}
    cfg = _did()
    del cfg["dids"][0]["write_range"]
    assert codegen._build_did_list(cfg)[0]["write_range"] is None


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


# [EDS-toolchain#257] codegen emits the range into the DID table, so its own
# validate_config() must refuse what uds_safety_check_write_range() cannot hold.
@pytest.mark.parametrize("kw,message", [
    ({"data_length": 5, "write_range": {"min": 0, "max": 1}}, "data_length 1 to 4"),
    ({"write_range": {"min": 21, "max": 20}}, "above max"),
    ({"write_range": {"min": 0, "max": 65536}}, "does not fit"),
    ({"write_range": {"min": -1, "max": 2}}, "must be {min"),
    ({"access": ["read"]}, "'write' in 'access'"),
])
def test_codegen_refuses_a_range_the_stack_cannot_enforce(kw, message, capsys) -> None:
    import codegen
    with pytest.raises(SystemExit):
        codegen.validate_config(_did(**{"min_session": "extended",
                                        "write_security_level": 1, **kw}))
    assert message in capsys.readouterr().err


def test_codegen_accepts_a_range_on_stub_and_app_dids() -> None:
    import codegen
    for source in ("stub", "app"):
        cfg = _did(source=source, min_session="extended", write_security_level=1)
        codegen.validate_config(cfg)
