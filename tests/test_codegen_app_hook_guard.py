"""
tests/test_codegen_app_hook_guard.py — codegen never overwrites [APP HOOK] code (EDS#613).

sensor_ecu and vehicle_state_ecu used to keep 13 hand-written handler bodies
in generated/, marked [APP HOOK]; one codegen run replaced them with TODO
stubs and printed "Generation complete." Codegen now refuses (exit 1, the
files named) when a file it would write or prune carries the marker, unless
--force. The bodies themselves moved to src/diag_app.c with 'source: app'.

Runs without the commercial templates: the check comes before rendering.

Run:  pytest tests/test_codegen_app_hook_guard.py -v
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import textwrap
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parent.parent
_CODEGEN = _REPO_ROOT / "tools" / "codegen.py"

sys.path.insert(0, str(_REPO_ROOT / "tools"))
import codegen  # noqa: E402

_CONFIG = textwrap.dedent(
    """\
    schema_version: 1
    metadata:
      ecu_name: "HookECU"
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
    """
)

_HOOKED = "/* [APP HOOK] hand-written body */\nint x;\n"


def _run(tmp_path: Path, *extra: str) -> subprocess.CompletedProcess:
    cfg = tmp_path / "diagnostics_config.yaml"
    cfg.write_text(_CONFIG, encoding="utf-8")
    # Same narrow public-checkout allowance as test_codegen_did_source.py.
    env = dict(os.environ, XALOQI_LICENSE_SKIP="1")
    return subprocess.run(
        [sys.executable, str(_CODEGEN), "--config", str(cfg),
         "--out", str(tmp_path / "out"), "--no-manifest", *extra],
        capture_output=True, text=True, cwd=str(_REPO_ROOT), timeout=120, env=env,
    )


def test_refuses_to_overwrite_an_app_hook_file(tmp_path: Path) -> None:
    out = tmp_path / "out"
    out.mkdir()
    (out / "did_handlers.c").write_text(_HOOKED, encoding="utf-8")
    r = _run(tmp_path)
    assert r.returncode == 1, r.stdout + r.stderr
    assert "refusing to overwrite hand-written code" in r.stderr
    assert str((out / "did_handlers.c").resolve()) in r.stderr
    assert "source: app" in r.stderr and "--force" in r.stderr
    assert (out / "did_handlers.c").read_text(encoding="utf-8") == _HOOKED
    assert "[3/5]" not in r.stdout          # nothing rendered


def test_force_gets_past_the_check(tmp_path: Path) -> None:
    out = tmp_path / "out"
    out.mkdir()
    (out / "routine_handlers.c").write_text(_HOOKED, encoding="utf-8")
    r = _run(tmp_path, "--force")
    assert "refusing" not in r.stderr
    assert "--force: overwriting 1 file(s)" in r.stderr
    # Without the commercial templates rendering stops later; with them it
    # writes. Either way the check no longer blocks.
    assert "[3/5]" in r.stdout


def test_dry_run_is_not_blocked(tmp_path: Path) -> None:
    out = tmp_path / "out"
    out.mkdir()
    (out / "did_handlers.c").write_text(_HOOKED, encoding="utf-8")
    r = _run(tmp_path, "--dry-run")
    assert r.returncode == 0, r.stdout + r.stderr


def test_marker_in_a_file_codegen_never_writes_is_ignored(tmp_path: Path) -> None:
    out = tmp_path / "out"
    out.mkdir()
    (out / "my_notes.c").write_text(_HOOKED, encoding="utf-8")
    assert codegen.find_app_hook_targets(out) == []


def test_targets_cover_every_written_file_tests_and_manifest(tmp_path: Path) -> None:
    out = tmp_path / "out"
    (out / "tests").mkdir(parents=True)
    for name in ("generated_config.h", "uds_init.c", "safety_config.h",
                 "did_safety_wrappers.c", "routine_handlers.h"):
        (out / name).write_text(_HOOKED, encoding="utf-8")
    (out / "tests" / "test_did_4200.py").write_text("# [APP HOOK]\n", encoding="utf-8")
    (out / "sovd_cda.json").write_text('{"x": "[APP HOOK]"}', encoding="utf-8")
    (out / "old_output.c").write_text(_HOOKED, encoding="utf-8")
    (out / "generated_files_phase3.json").write_text(
        json.dumps({"files": ["old_output.c"]}), encoding="utf-8")

    base = {p.name for p in codegen.find_app_hook_targets(out)}
    assert base == {"generated_config.h", "uds_init.c", "safety_config.h",
                    "did_safety_wrappers.c", "routine_handlers.h", "old_output.c"}
    full = {p.name for p in codegen.find_app_hook_targets(out, test_gen=True, sovd=True)}
    assert full == base | {"test_did_4200.py", "sovd_cda.json"}


def test_no_committed_example_keeps_app_hook_code_in_generated() -> None:
    hooked = [
        str(p.relative_to(_REPO_ROOT))
        for ex in sorted((_REPO_ROOT / "examples").iterdir()) if (ex / "generated").is_dir()
        for p in codegen.find_app_hook_targets(ex / "generated", test_gen=True, sovd=True)
    ]
    assert hooked == []


def test_step_labels_are_unique() -> None:
    """Each printed step label belongs to one step: [4C] was printed by both the
    routine-handler step and the GUI-catalog step (EDS#614)."""
    import re
    owner: dict = {}
    step = None
    for line in _CODEGEN.read_text(encoding="utf-8").splitlines():
        m = re.search(r"# ── Step (\w+):", line)
        if m:
            step = m.group(1)
            continue
        m = re.search(r'print\(f?"\[(\d[A-Z])\]', line)
        if m and step and "DRY RUN" not in line:   # --dry-run lists every step it skips
            owner.setdefault(m.group(1), set()).add(step)
    shared = {label: steps for label, steps in owner.items() if len(steps) > 1}
    assert not shared, shared
    assert owner["4C"] == {"4C"} and owner["4F"] == {"4F"}
