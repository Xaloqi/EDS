"""
tests/test_codegen_write_access_reachable.py — a writable DID must be writable.

WriteDataByIdentifier (0x2E) is gated at *service* level before any DID check
(core/uds_access_table.c, row [7]): it needs a non-default session and
SecurityAccess level 1 as the active level (uds_security_is_unlocked compares
active_level == 1). So a DID that declares

  * write access with ``min_session: default``  (F7), or
  * ``write_security_level`` above 1             (F14)

can never be written on EDS — the stack answers 0x7F or 0x33 every time. Until
2026-10-16 codegen accepted both silently; examples/vehicle_state_ecu shipped
two F7 DIDs and docs/AI_CONTEXT.md recommended F14 three times. Founder
decision 2026-10-16 (validation campaign): a hard error, exit 1.

Runs without the commercial templates: validation happens before rendering.

Run:  pytest tests/test_codegen_write_access_reachable.py -v
"""

from __future__ import annotations

import os
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest
import yaml

_REPO_ROOT = Path(__file__).resolve().parent.parent
_CODEGEN = _REPO_ROOT / "tools" / "codegen.py"

_BASE = textwrap.dedent(
    """\
    schema_version: 1
    metadata:
      ecu_name: "WriteGateECU"
      version: "1.0.0"
    timing:
      p2_server_max_ms: 25
      p2_star_server_max_ms: 5000
      s3_server_timeout_ms: 5000
    can:
      interface: vcan0
      rx_can_id: "0x7E0"
      tx_can_id: "0x7E8"
    sessions:
      - name: default
        id: "0x01"
      - name: extended
        id: "0x03"
    dids:
      - id: "0x4100"
        name: "WriteGateCal"
        data_length: 2
        access: ["read", "write"]
        min_session: {session}
        read_security_level: 0
        write_security_level: {wlevel}
    """
)


def _codegen(tmp_path: Path, session: str, wlevel: int) -> subprocess.CompletedProcess:
    cfg = tmp_path / "diagnostics_config.yaml"
    cfg.write_text(_BASE.format(session=session, wlevel=wlevel), encoding="utf-8")
    # A public checkout (no commercial templates, no _license.py) runs codegen
    # only under this narrow allowance — see the licence gate in codegen.py.
    # Without it every run exits 1 on licensing, and an exit-code assertion
    # would pass for the wrong reason (run-014); the message checks below are
    # what pin the refusal to this rule.
    env = dict(os.environ, XALOQI_LICENSE_SKIP="1")
    return subprocess.run(
        [sys.executable, str(_CODEGEN), "--config", str(cfg),
         "--out", str(tmp_path / "out"), "--dry-run", "--no-manifest"],
        capture_output=True, text=True, cwd=str(_REPO_ROOT), timeout=120, env=env,
    )


def test_default_session_write_is_rejected(tmp_path: Path) -> None:
    r = _codegen(tmp_path, "default", 1)
    out = r.stdout + r.stderr
    assert r.returncode == 1, f"expected exit 1, got {r.returncode}\n{out}"
    assert "min_session 'default'" in out and "0x2E" in out, out


def test_write_level_above_one_is_rejected(tmp_path: Path) -> None:
    r = _codegen(tmp_path, "extended", 3)
    out = r.stdout + r.stderr
    assert r.returncode == 1, f"expected exit 1, got {r.returncode}\n{out}"
    assert "write_security_level = 3" in out, out


@pytest.mark.parametrize("wlevel", [0, 1])
def test_reachable_write_is_accepted(tmp_path: Path, wlevel: int) -> None:
    """The rule must not reject the configurations the stack does accept."""
    r = _codegen(tmp_path, "extended", wlevel)
    out = r.stdout + r.stderr
    assert r.returncode == 0, f"expected exit 0, got {r.returncode}\n{out}"
    assert "can never be" not in out, out


def test_no_shipped_example_declares_an_unwritable_did() -> None:
    """Every example config obeys the rule (vehicle_state_ecu did not)."""
    bad = []
    for cfg in sorted((_REPO_ROOT / "examples").glob("*/diagnostics_config.yaml")):
        for did in yaml.safe_load(cfg.read_text(encoding="utf-8")).get("dids", []):
            if "write" not in did.get("access", []):
                continue
            if str(did.get("min_session", "default")) == "default" \
                    or int(did.get("write_security_level", 0)) > 1:
                bad.append(f"{cfg.parent.name}: {did['id']}")
    assert not bad, f"unwritable writable DIDs: {bad}"
