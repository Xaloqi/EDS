"""
tests/test_codegen_dtc_severity.py — DTC `severity` -> DTCSeverity byte (EDS#145).

The byte is what ReadDTCInformation 0x08/0x09 report and 0x07/0x08 match
(ISO 14229-1:2013 Annex D.3: bits 7-5 severity, bit 0 DTCClass_0). Before
EDS#145 the templates wrote 0x20 for check_at_next_halt and 0x40 for any other
value, so every value but one was mislabelled and a typo was invisible.

Runs without the commercial templates: validation happens before rendering.
The templates' use of `severity_byte` is tested in EDS-toolchain.

Run:  pytest tests/test_codegen_dtc_severity.py -v
"""

from __future__ import annotations

import os
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parent.parent
_CODEGEN = _REPO_ROOT / "tools" / "codegen.py"

sys.path.insert(0, str(_REPO_ROOT / "tools"))
import codegen  # noqa: E402
import config_parser  # noqa: E402

_BASE = textwrap.dedent(
    """\
    schema_version: 1
    metadata:
      ecu_name: "SeverityECU"
      version: "1.0.0"
    timing:
      p2_server_max_ms: 25
      p2_star_server_max_ms: 5000
      s3_server_timeout_ms: 5000
    can:
      interface: vcan0
      rx_can_id: "0x7E0"
      tx_can_id: "0x7E8"
    dids:
      - id: "0x4200"
        name: "SupplyVoltage_mV"
        data_length: 2
        access: [read]
        min_session: default
        read_security_level: 0
    dtcs:
      - code: "0xC00100"
        description: "CAN bus off"
    {extra}
    """
)

# ISO 14229-1:2013 Table D.12 severity bit | Table D.13 DTCClass_0.
_EXPECTED = {
    "none":                         0x01,
    "maintenance_only":             0x21,
    "check_at_next_halt":           0x41,
    "check_immediately":            0x81,
    "immediate_display":            0x81,
    "check_at_next_halt_ecu_reset": 0x41,
    "critical":                     0x81,
    "high":                         0x41,
    "medium":                       0x21,
    "low":                          0x01,
    "no_sub_function":              0x01,
}


def _codegen(tmp_path: Path, extra: str) -> subprocess.CompletedProcess:
    cfg = tmp_path / "diagnostics_config.yaml"
    cfg.write_text(_BASE.format(extra=extra), encoding="utf-8")
    env = dict(os.environ, XALOQI_LICENSE_SKIP="1")
    return subprocess.run(
        [sys.executable, str(_CODEGEN), "--config", str(cfg),
         "--out", str(tmp_path / "out"), "--dry-run", "--no-manifest"],
        capture_output=True, text=True, cwd=str(_REPO_ROOT), timeout=120, env=env,
    )


@pytest.mark.parametrize("value,byte", sorted(_EXPECTED.items()))
def test_severity_byte(value: str, byte: int) -> None:
    dtcs = codegen._build_dtc_list({"dtcs": [{"code": "0xC00100", "severity": value}]})
    assert dtcs[0]["severity_byte"] == f"0x{byte:02X}U"


def test_default_is_check_at_next_halt() -> None:
    dtcs = codegen._build_dtc_list({"dtcs": [{"code": "0xC00100"}]})
    assert dtcs[0]["severity_byte"] == "0x41U"


def test_every_byte_has_dtc_class_0_and_one_severity_bit_at_most() -> None:
    for value, byte in codegen._DTC_SEVERITY_BYTES.items():
        assert byte & 0x1F == 0x01, value
        assert bin(byte & 0xE0).count("1") <= 1, value


@pytest.mark.parametrize("value", sorted(_EXPECTED))
def test_accepted_values_pass_validation(tmp_path: Path, value: str) -> None:
    r = _codegen(tmp_path, f"    severity: {value}")
    assert r.returncode == 0, r.stdout + r.stderr


def test_unknown_severity_is_rejected(tmp_path: Path) -> None:
    r = _codegen(tmp_path, "    severity: check_at_next_hault")
    out = r.stdout + r.stderr
    assert r.returncode == 1, out
    assert "'severity'" in out and "check_at_next_hault" in out, out


def test_config_parser_schema_matches_codegen() -> None:
    enum = config_parser.DTC_ENTRY_SCHEMA["properties"]["severity"]["enum"]
    assert set(enum) == set(codegen._DTC_SEVERITY_BYTES) | set(codegen._DTC_SEVERITY_ALIASES)
