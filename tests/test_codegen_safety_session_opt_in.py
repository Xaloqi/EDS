"""safetySystemDiagnosticSession (0x04) is opt-in per ECU (EDS#494).

ISO 14229-1:2026 makes 0x04 an optional SubFunction. The stack answers it
NRC 0x12 unless the generated init enables it, which codegen does only for
'ecu: {sessions: {safety_system: true}}'. A DID or routine gated on 0x04 while
it is off could never be reached, so codegen refuses that configuration.
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
sys.path.insert(0, str(_REPO_ROOT / "tools"))

_BASE = textwrap.dedent(
    """\
    schema_version: 1
    metadata:
      ecu_name: "SafetySessionECU"
      version: "1.0.0"
    timing:
      p2_server_max_ms: 25
      p2_star_server_max_ms: 5000
      s3_server_timeout_ms: 5000
    can:
      interface: vcan0
      rx_can_id: "0x7E0"
      tx_can_id: "0x7E8"
    {ecu}
    dids:
      - id: "0x4100"
        name: "SafetyStatus"
        data_length: 2
        access: ["read"]
        min_session: {session}
        read_security_level: 0
    """
)


def _codegen(tmp_path: Path, ecu: str, session: str) -> subprocess.CompletedProcess:
    cfg = tmp_path / "diagnostics_config.yaml"
    cfg.write_text(_BASE.format(ecu=ecu, session=session), encoding="utf-8")
    env = dict(os.environ, XALOQI_LICENSE_SKIP="1")   # public checkout, see test_codegen_write_access_reachable
    return subprocess.run(
        [sys.executable, str(_CODEGEN), "--config", str(cfg),
         "--out", str(tmp_path / "out"), "--dry-run", "--no-manifest"],
        capture_output=True, text=True, cwd=str(_REPO_ROOT), timeout=120, env=env,
    )


def test_safety_gated_did_without_the_session_is_rejected(tmp_path: Path) -> None:
    r = _codegen(tmp_path, "", "safety_system")
    out = r.stdout + r.stderr
    assert r.returncode == 1, f"expected exit 1, got {r.returncode}\n{out}"
    assert "safety-system session (0x04) is not enabled" in out, out


def test_safety_gated_did_with_the_session_is_accepted(tmp_path: Path) -> None:
    r = _codegen(tmp_path, "ecu:\n  sessions:\n    safety_system: true", "safety_system")
    out = r.stdout + r.stderr
    assert r.returncode == 0, f"expected exit 0, got {r.returncode}\n{out}"


@pytest.mark.parametrize("ecu, message", [
    ("ecu:\n  sessions:\n    safety_system: yes_please", "must be true or false"),
    ("ecu:\n  sessions:\n    extended: true", "unknown 'ecu.sessions.extended'"),
    ("ecu:\n  sessions: [safety_system]", "'ecu.sessions' must be a mapping"),
])
def test_malformed_ecu_sessions_is_rejected(tmp_path: Path, ecu: str, message: str) -> None:
    r = _codegen(tmp_path, ecu, "extended")
    out = r.stdout + r.stderr
    assert r.returncode == 1, f"expected exit 1, got {r.returncode}\n{out}"
    assert message in out, out


def test_template_context_carries_the_flag() -> None:
    import codegen
    base = yaml.safe_load(_BASE.format(ecu="", session="extended"))
    assert codegen.build_uds_init_context(base)["safety_system_session"] is False
    base["ecu"] = {"sessions": {"safety_system": True}}
    assert codegen.build_uds_init_context(base)["safety_system_session"] is True


def test_no_shipped_example_enables_the_safety_session() -> None:
    """Pins today's state, so enabling 0x04 in an example is a visible decision."""
    enabled = [cfg.parent.name for cfg in sorted((_REPO_ROOT / "examples").glob("*/diagnostics_config.yaml"))
               if ((yaml.safe_load(cfg.read_text(encoding="utf-8")).get("ecu") or {})
                   .get("sessions") or {}).get("safety_system")]
    assert not enabled, f"examples enabling 0x04: {enabled}"
