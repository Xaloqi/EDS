"""
tests/test_codegen_output_contract.py — regression tests for the 2026-09-19
validation campaign findings on codegen's *output set*.

Two defects, one root cause: codegen decided per-run which files to emit, but
neither the generated ``#include`` directives nor the CMake source lists nor
the output directory itself were reconciled against that decision.

  1. Conditional emission vs. unconditional consumption.
     ``uds_init.c`` includes ``routine_handlers.h`` and ``uds_init.h``
     includes ``safety_config.h`` unconditionally, and CMakeLists.txt adds
     ``routine_handlers.c`` / ``did_safety_wrappers.c`` unconditionally — but
     codegen skipped emitting them for a config with no ``routines:`` block,
     or when ``--safety-wrappers`` was not passed. Result: C that does not
     compile, while codegen exited 0 printing "Generation complete."
     All 12 shipped examples reproduced it; none exercised it, because every
     example declares routines and every documented command passes the flag.

  2. Stale output survived regeneration.
     Because emission was conditional, regenerating into an existing --out
     directory with a config that no longer declares routines left the
     *previous* configuration's ``routine_handlers.c`` in place — still
     registering RoutineControl identifiers the engineer had just removed —
     and CMake still compiled it in. On a BMS those are actuation routines.
     Same mechanism left an unrelated ECU's ASIL-B ``did_safety_wrappers.c``
     and ``safety_config.h`` behind when ``--safety-wrappers`` was dropped,
     so the safety chain in the image no longer matched the DID set it
     claimed to wrap.

The fix makes the output set unconditional for these files (``--safety-wrappers``
now gates ASIL validation *enforcement*, not emission) and reconciles the
output directory against the previous manifest for what stays conditional
(``tests/``, ``sovd_cda.json``, the GUI catalog).

Run:  XALOQI_LICENSE_SKIP=1 pytest tests/test_codegen_output_contract.py -v
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parent.parent
_CODEGEN = _REPO_ROOT / "tools" / "codegen.py"

# tools/templates/ is a commercial, gitignored deliverable (#67): present in a
# licensed install, absent in a plain public clone. Same skip convention as
# tests/test_codegen_c_escaping.py.
_TEMPLATE_DIR = Path(
    os.environ.get("EDS_TEMPLATE_DIR", str(_REPO_ROOT / "tools" / "templates"))
)
_TEMPLATES_OK = _TEMPLATE_DIR.is_dir() and any(_TEMPLATE_DIR.glob("*.j2"))

pytestmark = pytest.mark.skipif(
    not _TEMPLATES_OK,
    reason="commercial tools/templates/ not present in this checkout (#67)",
)

# Every file name codegen itself can write into --out. An #include of one of
# these must resolve inside the output directory; anything else is a core,
# transport or platform header resolved by the build system's include paths
# and is out of scope here.
_CODEGEN_OWNED_HEADERS = frozenset(
    {
        "generated_config.h",
        "did_handlers.h",
        "did_safety_wrappers.h",
        "dtc_config.h",
        "routine_handlers.h",
        "safety_config.h",
        "uds_init.h",
    }
)

_INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)

_BASE_CONFIG = textwrap.dedent(
    """\
    schema_version: 1
    metadata:
      ecu_name: "ContractECU"
      version: "1.0.0"
      description: "output-contract regression fixture"
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
      - id: "0x4001"
        name: "ContractTemp_degC"
        data_length: 2
        access: ["read"]
        min_session: default
        read_security_level: 0
        write_security_level: 0
        description: "a benign DID"
    dtcs:
      - code: "0x900100"
        name: "ContractOverTemp"
        description: "over temperature"
    """
)

# The same config plus a routines: block. Removing it between two runs is the
# customer workflow that finding 2 above is about.
_ROUTINES_BLOCK = textwrap.dedent(
    """\
    routines:
      - id: "0xBB00"
        name: "ContractSelfTest"
        min_session: extended
        security_level: 0
        support: ["start", "results"]
      - id: "0xBB01"
        name: "ContractForceActuate"
        min_session: extended
        security_level: 0
        support: ["start", "results"]
    """
)


def _run_codegen(config: Path, out: Path, *flags: str) -> subprocess.CompletedProcess:
    env = dict(os.environ, XALOQI_LICENSE_SKIP="1")
    return subprocess.run(
        [
            sys.executable,
            str(_CODEGEN),
            "--config", str(config),
            "--out", str(out),
            "--template-dir", str(_TEMPLATE_DIR),
            *flags,
        ],
        capture_output=True,
        text=True,
        env=env,
        cwd=str(_REPO_ROOT),
    )


def _write(path: Path, text: str) -> Path:
    path.write_text(text, encoding="utf-8")
    return path


def _unresolved_includes(out: Path) -> list[str]:
    """Every codegen-owned header an emitted file includes but which is absent."""
    missing = []
    for src in sorted(list(out.glob("*.c")) + list(out.glob("*.h"))):
        for inc in _INCLUDE_RE.findall(src.read_text(encoding="utf-8")):
            name = Path(inc).name
            if name in _CODEGEN_OWNED_HEADERS and not (out / name).is_file():
                missing.append(f"{src.name} -> {inc}")
    return missing


# ---------------------------------------------------------------------------
# Finding 1 — conditional emission vs. unconditional consumption
# ---------------------------------------------------------------------------

def test_routine_handlers_emitted_for_a_config_without_routines(tmp_path: Path) -> None:
    """uds_init.c includes routine_handlers.h unconditionally, so it must exist.

    Before the fix codegen printed "No routines configured — routine_handlers
    skipped", exited 0, and left uds_init.c including a file it had not written.
    """
    cfg = _write(tmp_path / "no_routines.yaml", _BASE_CONFIG)
    out = tmp_path / "gen"
    proc = _run_codegen(cfg, out, "--safety-wrappers", "--asil-level", "B")

    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert (out / "routine_handlers.h").is_file(), "routine_handlers.h not emitted"
    assert (out / "routine_handlers.c").is_file(), "routine_handlers.c not emitted"


def test_safety_files_emitted_without_the_safety_wrappers_flag(tmp_path: Path) -> None:
    """uds_init.h includes safety_config.h unconditionally, so it must exist.

    --safety-wrappers gates ASIL validation enforcement, not emission.
    """
    cfg = _write(tmp_path / "cfg.yaml", _BASE_CONFIG + _ROUTINES_BLOCK)
    out = tmp_path / "gen"
    proc = _run_codegen(cfg, out)  # deliberately no --safety-wrappers

    assert proc.returncode == 0, proc.stdout + proc.stderr
    for name in ("safety_config.h", "did_safety_wrappers.h", "did_safety_wrappers.c"):
        assert (out / name).is_file(), f"{name} not emitted"


@pytest.mark.parametrize(
    "with_routines, flags",
    [
        (True,  ("--safety-wrappers", "--asil-level", "B")),
        (True,  ()),
        (False, ("--safety-wrappers", "--asil-level", "B")),
        (False, ()),
    ],
    ids=["routines+asil", "routines+qm", "noroutines+asil", "noroutines+qm"],
)
def test_every_generated_include_resolves(
    tmp_path: Path, with_routines: bool, flags: tuple[str, ...]
) -> None:
    """The generated tree must be self-consistent for every flag combination.

    This is the invariant the two skip-branches violated: a file codegen wrote
    may not #include a codegen-owned header codegen chose not to write.
    """
    body = _BASE_CONFIG + (_ROUTINES_BLOCK if with_routines else "")
    cfg = _write(tmp_path / "cfg.yaml", body)
    out = tmp_path / "gen"
    proc = _run_codegen(cfg, out, *flags)

    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert _unresolved_includes(out) == []


# ---------------------------------------------------------------------------
# Finding 2 — stale output must not survive regeneration
# ---------------------------------------------------------------------------

def test_routines_removed_from_yaml_do_not_survive_in_the_output(tmp_path: Path) -> None:
    """The customer workflow: delete the routines: block, regenerate, same flags.

    Before the fix the previous run's routine_handlers.c stayed on disk still
    registering 0xBB00/0xBB01 — and CMakeLists.txt compiles that file
    unconditionally, so the removed routines shipped.
    """
    out = tmp_path / "gen"
    with_r = _write(tmp_path / "with.yaml", _BASE_CONFIG + _ROUTINES_BLOCK)
    without_r = _write(tmp_path / "without.yaml", _BASE_CONFIG)

    assert _run_codegen(with_r, out, "--safety-wrappers", "--asil-level", "B").returncode == 0
    first = (out / "routine_handlers.c").read_text(encoding="utf-8")
    assert "0xBB00" in first, "fixture is not exercising the defect"

    proc = _run_codegen(without_r, out, "--safety-wrappers", "--asil-level", "B")
    assert proc.returncode == 0, proc.stdout + proc.stderr

    second = (out / "routine_handlers.c").read_text(encoding="utf-8")
    assert "0xBB00" not in second, "removed routine 0xBB00 survived regeneration"
    assert "0xBB01" not in second, "removed routine 0xBB01 survived regeneration"


def test_safety_wrappers_from_a_previous_run_do_not_survive(tmp_path: Path) -> None:
    """Dropping --safety-wrappers must not leave the previous DID set's wrappers.

    Before the fix did_safety_wrappers.c and safety_config.h kept the earlier
    ECU's identity and wrapped DIDs absent from the new configuration — the
    ASIL-B chain in the image no longer matching the config it came from.
    """
    out = tmp_path / "gen"
    first_cfg = _write(tmp_path / "first.yaml", _BASE_CONFIG + _ROUTINES_BLOCK)
    assert _run_codegen(first_cfg, out, "--safety-wrappers", "--asil-level", "B").returncode == 0

    second_cfg = _write(
        tmp_path / "second.yaml",
        _BASE_CONFIG.replace('ecu_name: "ContractECU"', 'ecu_name: "SecondECU"')
        .replace('id: "0x4001"', 'id: "0x4009"')
        .replace('name: "ContractTemp_degC"', 'name: "SecondTemp_degC"'),
    )
    proc = _run_codegen(second_cfg, out)  # no --safety-wrappers
    assert proc.returncode == 0, proc.stdout + proc.stderr

    wrappers = (out / "did_safety_wrappers.c").read_text(encoding="utf-8")
    assert "0x4001" not in wrappers, "previous config's DID still wrapped"
    assert "ContractECU" not in wrappers, "previous ECU identity survived"


def test_conditional_output_is_pruned_when_the_flag_is_dropped(tmp_path: Path) -> None:
    """sovd_cda.json stays conditional, so it must be reconciled via the manifest.

    --sovd is used rather than --test-gen because SOVD rendering is built into
    codegen.py, while test generation needs the commercial tools/testgen.py
    that a public checkout does not have. The pruning path under test is the
    same one for both.
    """
    out = tmp_path / "gen"
    cfg = _write(tmp_path / "cfg.yaml", _BASE_CONFIG + _ROUTINES_BLOCK)

    assert _run_codegen(cfg, out, "--safety-wrappers", "--asil-level", "B",
                        "--sovd").returncode == 0
    assert (out / "sovd_cda.json").is_file(), "fixture is not exercising the defect"

    proc = _run_codegen(cfg, out, "--safety-wrappers", "--asil-level", "B")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert not (out / "sovd_cda.json").exists(), \
        "stale sovd_cda.json survived a run that no longer produces it"


@pytest.mark.skipif(
    not (_REPO_ROOT / "tools" / "testgen.py").is_file(),
    reason="commercial tools/testgen.py not present in this checkout (#67)",
)
def test_generated_tests_are_pruned_when_test_gen_is_dropped(tmp_path: Path) -> None:
    """Same reconciliation, on the conditional output a customer sees most."""
    out = tmp_path / "gen"
    cfg = _write(tmp_path / "cfg.yaml", _BASE_CONFIG + _ROUTINES_BLOCK)

    assert _run_codegen(cfg, out, "--safety-wrappers", "--asil-level", "B",
                        "--test-gen").returncode == 0
    assert any((out / "tests").glob("test_*.py"))

    proc = _run_codegen(cfg, out, "--safety-wrappers", "--asil-level", "B")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    leftover = sorted(p.name for p in (out / "tests").glob("test_*.py")) \
        if (out / "tests").is_dir() else []
    assert leftover == [], f"stale generated tests survived: {leftover}"


# ---------------------------------------------------------------------------
# The manifest is the reconciliation key, so it has to be trustworthy
# ---------------------------------------------------------------------------

def test_exactly_one_manifest_after_switching_the_safety_flag(tmp_path: Path) -> None:
    """Phase 2A and Phase 3 manifests must never coexist and disagree."""
    out = tmp_path / "gen"
    cfg = _write(tmp_path / "cfg.yaml", _BASE_CONFIG + _ROUTINES_BLOCK)

    assert _run_codegen(cfg, out, "--safety-wrappers", "--asil-level", "B").returncode == 0
    assert _run_codegen(cfg, out).returncode == 0

    manifests = sorted(p.name for p in out.glob("generated_files_*.json"))
    assert len(manifests) == 1, f"expected one manifest, found {manifests}"


def test_manifest_paths_are_relative_and_leak_no_absolute_paths(tmp_path: Path) -> None:
    """_relative() used a hard-coded parent.parent.parent that only made sense
    for examples/*/generated/. Elsewhere it recorded unresolvable fragments, and
    three committed example manifests leaked a developer-machine path."""
    out = tmp_path / "gen"
    cfg = _write(tmp_path / "cfg.yaml", _BASE_CONFIG + _ROUTINES_BLOCK)
    assert _run_codegen(cfg, out, "--safety-wrappers", "--asil-level", "B").returncode == 0

    manifest = json.loads((out / "generated_files_phase3.json").read_text(encoding="utf-8"))

    for entry in manifest["files"]:
        assert not Path(entry).is_absolute(), f"absolute path in manifest: {entry}"
        assert (out / entry).is_file(), f"manifest entry does not resolve: {entry}"

    blob = json.dumps(manifest)
    assert "/home/" not in blob, "manifest leaks a developer-machine path"
