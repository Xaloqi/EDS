"""
tests/test_codegen_null_sections.py — a section that is YAML null (EDS#532).

`dtcs:` followed only by comments parses as `dtcs: null`. The schema types
dtcs/routines/dids as arrays, and validate_config() already says "must be a
list (can be empty)", so null is a validation error (exit 1, message), not an
empty list and never a traceback. main() used to call len() on the raw value
before validate_config() ran.

Run:  pytest tests/test_codegen_null_sections.py -v
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

_HEAD = textwrap.dedent(
    """\
    schema_version: 1
    metadata:
      ecu_name: "NullECU"
      version: "1.0.0"
    timing:
      p2_server_max_ms: 25
      p2_star_server_max_ms: 5000
      s3_server_timeout_ms: 5000
    can:
      interface: vcan0
      rx_can_id: "0x7E0"
      tx_can_id: "0x7E8"
    """
)

_DIDS = textwrap.dedent(
    """\
    dids:
      - id: "0x4200"
        name: "SupplyVoltage_mV"
        data_length: 2
        access: ["read"]
        min_session: default
        read_security_level: 0
        write_security_level: 0
    """
)


def _codegen(tmp_path: Path, body: str) -> subprocess.CompletedProcess:
    cfg = tmp_path / "diagnostics_config.yaml"
    cfg.write_text(_HEAD + body, encoding="utf-8")
    env = dict(os.environ, XALOQI_LICENSE_SKIP="1")
    return subprocess.run(
        [sys.executable, str(_CODEGEN), "--config", str(cfg),
         "--out", str(tmp_path / "out"), "--dry-run", "--no-manifest"],
        capture_output=True, text=True, cwd=str(_REPO_ROOT), timeout=120, env=env,
    )


@pytest.mark.parametrize("section", ["dtcs", "routines"])
def test_comment_only_section_is_a_validation_error(tmp_path: Path, section: str) -> None:
    r = _codegen(tmp_path, _DIDS + f"{section}:\n  # none yet\n")
    assert "Traceback" not in r.stderr, r.stderr
    assert r.returncode == 1, r.stdout + r.stderr
    assert f"'{section}' must be a list" in r.stderr


@pytest.mark.parametrize("section", ["dtcs", "routines"])
def test_explicit_empty_list_is_accepted(tmp_path: Path, section: str) -> None:
    r = _codegen(tmp_path, _DIDS + f"{section}: []\n")
    assert r.returncode == 0, r.stdout + r.stderr


def test_comment_only_dids_is_a_validation_error(tmp_path: Path) -> None:
    r = _codegen(tmp_path, "dids:\n  # none\n")
    assert "Traceback" not in r.stderr, r.stderr
    assert r.returncode == 1, r.stdout + r.stderr
    assert "'dids' must be a non-empty list" in r.stderr
