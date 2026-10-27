"""
tests/test_codegen_did_source.py — `source` on a DID (EDS-toolchain#201).

`source: app` means the application implements did_read_<name>() (and
did_write_<name>() for a writable DID); codegen emits no backing store or
handler body for it. `stub` (the default) is the behaviour every existing
config has. Validation here; what the templates emit, and that the result
links and reads through the 5-step chain, is tested in EDS-toolchain
(tests/test_app_owned_did.py), where the templates live.

Runs without the commercial templates: validation happens before rendering.

Run:  pytest tests/test_codegen_did_source.py -v
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
      ecu_name: "SourceECU"
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
        access: ["read"]
        min_session: default
        read_security_level: 0
        write_security_level: 0
    {extra}
    """
)


def _codegen(tmp_path: Path, extra: str) -> subprocess.CompletedProcess:
    cfg = tmp_path / "diagnostics_config.yaml"
    cfg.write_text(_BASE.format(extra=extra), encoding="utf-8")
    # Same narrow public-checkout allowance as test_codegen_write_access_reachable.py;
    # the message checks pin each refusal to the source rule (run-014).
    env = dict(os.environ, XALOQI_LICENSE_SKIP="1")
    return subprocess.run(
        [sys.executable, str(_CODEGEN), "--config", str(cfg),
         "--out", str(tmp_path / "out"), "--dry-run", "--no-manifest"],
        capture_output=True, text=True, cwd=str(_REPO_ROOT), timeout=120, env=env,
    )


@pytest.mark.parametrize("value", ["app", "stub"])
def test_valid_source_is_accepted(tmp_path: Path, value: str) -> None:
    r = _codegen(tmp_path, f"    source: {value}")
    assert r.returncode == 0, r.stdout + r.stderr


def test_unknown_source_is_rejected(tmp_path: Path) -> None:
    r = _codegen(tmp_path, "    source: application")
    out = r.stdout + r.stderr
    assert r.returncode == 1, out
    assert "'source'" in out and "'application'" in out, out


def test_app_source_with_default_value_is_rejected(tmp_path: Path) -> None:
    # An app-owned DID has no generated backing store, so a default_value
    # would be silently dropped.
    r = _codegen(tmp_path, '    source: app\n    default_value: "0x0C80"')
    out = r.stdout + r.stderr
    assert r.returncode == 1, out
    assert "default_value" in out and "source: app" in out, out


def test_context_marks_app_owned_dids() -> None:
    cfg = {"dids": [
        {"id": "0x4200", "name": "A", "access": ["read"], "source": "app"},
        {"id": "0x4201", "name": "B", "access": ["read"], "source": "stub"},
        {"id": "0x4202", "name": "C", "access": ["read"]},
    ]}
    assert [d["app_owned"] for d in codegen._build_did_list(cfg)] == [True, False, False]


def test_config_parser_schema_accepts_source() -> None:
    props = config_parser.DID_ENTRY_SCHEMA["properties"]
    assert props["source"]["enum"] == ["stub", "app"]


def test_config_parser_rejects_app_source_with_default_value() -> None:
    cfg = {"dids": [{"id": "0x4200", "name": "A", "access": ["read"],
                     "data_length": 2, "source": "app", "default_value": "0x0C80"}]}
    errors = config_parser.validate_config(cfg)
    assert any("source: app" in e for e in errors), errors
