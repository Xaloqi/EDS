"""
tests/test_codegen_routine_source.py — `source` on a routine (EDS#613).

As for DIDs (tests/test_codegen_did_source.py): `source: app` means the
application defines routine_start_<name>() and, if supported, the stop and
results callbacks; codegen emits their prototypes and the registration row,
and no body, so a missing definition is a link error. `stub` (the default)
keeps the generated stub bodies. Before this a routine with real behaviour
needed hand edits in generated/routine_handlers.c, which regeneration lost.

routine_handlers.{h,c} are generated inline by codegen.py (there is no
template), so the emitted C is tested here.

Run:  pytest tests/test_codegen_routine_source.py -v
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

_BASE = textwrap.dedent(
    """\
    schema_version: 1
    metadata:
      ecu_name: "RoutineECU"
      version: "1.0.0"
    timing:
      p2_server_max_ms: 25
      p2_star_server_max_ms: 5000
      s3_server_timeout_ms: 5000
    can:
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
    routines:
      - id: "0xC100"
        name: "SelfTest"
        min_session: extended
        security_level: 0
        support: ["start", "results"]
    {extra}
      - id: "0xC101"
        name: "LampCheck"
        min_session: extended
        security_level: 1
        support: ["start", "stop"]
    """
)


def _cfg(extra: str = "") -> dict:
    import yaml
    return yaml.safe_load(_BASE.format(extra=extra))


def _dry_run(tmp_path: Path, extra: str) -> subprocess.CompletedProcess:
    cfg = tmp_path / "diagnostics_config.yaml"
    cfg.write_text(_BASE.format(extra=extra), encoding="utf-8")
    env = dict(os.environ, XALOQI_LICENSE_SKIP="1")
    return subprocess.run(
        [sys.executable, str(_CODEGEN), "--config", str(cfg),
         "--out", str(tmp_path / "out"), "--dry-run", "--no-manifest"],
        capture_output=True, text=True, cwd=str(_REPO_ROOT), timeout=120, env=env,
    )


def _render(tmp_path: Path, extra: str) -> tuple[str, str]:
    routines = codegen._build_routine_list(_cfg(extra))
    for name in ("routine_handlers.h", "routine_handlers.c"):
        codegen._write_routine_handlers_inline(
            tmp_path / name, name, routines, "RoutineECU", "1.0.0", "T")
    return ((tmp_path / "routine_handlers.h").read_text(encoding="utf-8"),
            (tmp_path / "routine_handlers.c").read_text(encoding="utf-8"))


@pytest.mark.parametrize("value", ["app", "stub"])
def test_valid_source_is_accepted(tmp_path: Path, value: str) -> None:
    r = _dry_run(tmp_path, f"    source: {value}")
    assert r.returncode == 0, r.stdout + r.stderr


def test_unknown_source_is_rejected(tmp_path: Path) -> None:
    r = _dry_run(tmp_path, "    source: application")
    assert r.returncode == 1
    assert "routines[0]: invalid 'source' value 'application'" in r.stderr


def test_context_marks_app_owned_routines() -> None:
    routines = codegen._build_routine_list(_cfg("    source: app"))
    assert [r["app_owned"] for r in routines] == [True, False]


def test_app_routine_has_prototypes_and_registration_but_no_body(tmp_path: Path) -> None:
    h, c = _render(tmp_path, "    source: app")
    # Declared, for the application to define.
    assert "uds_status_t routine_start_selftest(" in h
    assert "uds_status_t routine_results_selftest(" in h
    # Not defined here: only named in the comment, and registered.
    assert "uds_status_t routine_start_selftest(" not in c
    assert "uds_status_t routine_results_selftest(" not in c
    assert "routine_start_selftest(), routine_results_selftest()" in c
    assert "entry.start_cb       = routine_start_selftest;" in c
    assert "entry.results_cb     = routine_results_selftest;" in c
    # The stub routine next to it is unchanged.
    assert "uds_status_t routine_start_lampcheck(" in c
    assert "uds_status_t routine_stop_lampcheck(" in c


def test_stub_output_is_unchanged_by_default(tmp_path: Path) -> None:
    (tmp_path / "a").mkdir()
    (tmp_path / "b").mkdir()
    assert _render(tmp_path / "a", "") == _render(tmp_path / "b", "    source: stub")
    _, c = _render(tmp_path / "a", "")
    assert "Defined by the application" not in c


def test_all_app_routines_drop_the_stub_banner(tmp_path: Path) -> None:
    routines = codegen._build_routine_list(_cfg("    source: app"))
    for r in routines:
        r["app_owned"] = True
    codegen._write_routine_handlers_inline(
        tmp_path / "routine_handlers.c", "routine_handlers.c", routines, "E", "1", "T")
    c = (tmp_path / "routine_handlers.c").read_text(encoding="utf-8")
    assert "Replace each stub body" not in c
    assert "TODO" not in c


def test_empty_routine_list_output_is_unchanged(tmp_path: Path) -> None:
    """`routines: []` (canfd_ecu) keeps the stub banner it always had."""
    codegen._write_routine_handlers_inline(
        tmp_path / "routine_handlers.c", "routine_handlers.c", [], "E", "1", "T")
    c = (tmp_path / "routine_handlers.c").read_text(encoding="utf-8")
    assert "Routine callback stubs" in c
    assert "Defined by the application" not in c
