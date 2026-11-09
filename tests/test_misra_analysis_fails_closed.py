"""EDS#572: the MISRA gate must FAIL whenever cppcheck did not really analyse.

Each case puts a fake `cppcheck` first on PATH and runs the real script. None
needs cppcheck installed, so they run in every environment.
"""
import os
import stat
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "misra_analysis.py"


def _run(tmp_path: Path, fake_body, extra=()):
    env = dict(os.environ)
    bindir = tmp_path / "bin"
    bindir.mkdir()
    if fake_body is not None:
        fake = bindir / "cppcheck"
        fake.write_text("#!/bin/sh\n" + fake_body)
        fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
    # Keep python3 and gcc reachable, but drop any real cppcheck.
    keep = tmp_path / "tools"
    keep.mkdir()
    for tool in ("python3", "gcc", "sh"):
        real = subprocess.run(["which", tool], capture_output=True, text=True).stdout.strip()
        if real:
            (keep / tool).symlink_to(real)
    env["PATH"] = f"{bindir}:{keep}"
    return subprocess.run(
        [sys.executable, str(SCRIPT), "--out", str(tmp_path / "r.json"), *extra],
        capture_output=True, text=True, env=env, cwd=ROOT, timeout=600,
    )


def _assert_fail(r):
    assert r.returncode != 0, r.stdout
    assert "RESULT: FAIL" in r.stdout
    assert "RESULT: PASS" not in r.stdout


def test_missing_cppcheck_fails(tmp_path):
    _assert_fail(_run(tmp_path, None))


def test_nonzero_exit_fails(tmp_path):
    body = '[ "$1" = --version ] && { echo Cppcheck 9; exit 0; }\n' \
           'echo "Failed to load library configuration file std.cfg" >&2\nexit 1\n'
    _assert_fail(_run(tmp_path, body))


def test_exit_zero_but_nothing_checked_fails(tmp_path):
    # No findings and exit 0 is exactly what a broken install can produce:
    # only the canary (a known violation) tells it apart from a clean tree.
    body = '[ "$1" = --version ] && { echo Cppcheck 9; exit 0; }\nexit 0\n'
    _assert_fail(_run(tmp_path, body))


def test_gcc_only_is_still_explicit_opt_in(tmp_path):
    r = _run(tmp_path, None, extra=("--gcc-only",))
    assert "GCC-only subset" in r.stdout
