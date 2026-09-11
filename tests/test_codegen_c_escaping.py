"""
tests/test_codegen_c_escaping.py — regression tests for the 2026-09-11
validation campaign finding: codegen embedded config-supplied names into
generated C without escaping, so hostile-but-valid input produced source
that does not compile while codegen exited 0 reporting "Generation complete."

Two independent breakages, both reachable from a valid AUTOSAR ARXML import
(`arxml_parser.py` escapes correctly for YAML — EDS-toolchain#34 — so the
text round-trips cleanly into codegen and breaks at the C layer instead):

    /* Stub backing store for DID 0x3005 — End*/Comment/*Break. */
                                               ^ comment ends here
        error: unknown type name 'Comment'

    .description = "Say "Hello""
                        ^ literal ends here
        error: expected '}' before 'Hello'

Run:  XALOQI_LICENSE_SKIP=1 pytest tests/test_codegen_c_escaping.py -v
"""

from __future__ import annotations

import importlib.util
import os
import shutil
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parent.parent
_CODEGEN = _REPO_ROOT / "tools" / "codegen.py"

# tools/templates/ is a commercial, gitignored deliverable (#67): present in a
# licensed install, absent in a plain public clone. Same skip convention as
# test_robustness_L_codegen_output_fidelity.py (O-63).
_TEMPLATE_DIR = Path(
    os.environ.get("EDS_TEMPLATE_DIR", str(_REPO_ROOT / "tools" / "templates"))
)
_TEMPLATES_OK = _TEMPLATE_DIR.is_dir() and any(_TEMPLATE_DIR.glob("*.j2"))


def _load_codegen():
    spec = importlib.util.spec_from_file_location("_codegen_under_test", _CODEGEN)
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(mod)
    return mod


codegen = _load_codegen()


# ---------------------------------------------------------------------------
# Unit: the sanitizer itself
# ---------------------------------------------------------------------------

@pytest.mark.parametrize(
    "raw, must_not_contain",
    [
        ("End*/Comment/*Break", "*/"),   # would terminate a block comment
        ('Say "Hello"', '"'),            # would terminate a string literal
    ],
)
def test_sanitiser_removes_the_dangerous_sequence(raw: str, must_not_contain: str) -> None:
    out = codegen._c_safe_text(raw)
    if must_not_contain == '"':
        # Quotes must survive only in escaped form.
        assert '\\"' in out
        assert not [i for i, c in enumerate(out) if c == '"' and (i == 0 or out[i - 1] != "\\")]
    else:
        assert must_not_contain not in out


def test_sanitiser_escapes_backslashes_before_quotes() -> None:
    """Order matters: escaping quotes first would double-escape the backslash."""
    assert codegen._c_safe_text(r"a\b") == r"a\\b"
    assert codegen._c_safe_text('a\\"b') == r'a\\\"b'


def test_sanitiser_strips_control_characters() -> None:
    """A newline breaks a // comment and a string literal alike."""
    out = codegen._c_safe_text("line1\nline2\tend")
    assert "\n" not in out and "\t" not in out


@pytest.mark.parametrize(
    "benign",
    [
        "BMS_PackCurrent_100mA",
        "Kühlmittel-Temperatur",      # umlaut + hyphen
        "2ndAxleTemp",                # leading digit
        "Hash#Comment",               # YAML comment char
        "Vehicle Identification Number (VIN)",
    ],
)
def test_sanitiser_is_identity_for_real_names(benign: str) -> None:
    """Must not perturb any name a real config would carry.

    This is what keeps generated output byte-identical for the 12 shipped
    examples — verified across 8 of them (293 files, 0 substantive diffs)
    when the fix landed.
    """
    assert codegen._c_safe_text(benign) == benign


def test_sanitiser_passes_through_non_strings() -> None:
    assert codegen._c_safe_text(None) is None  # type: ignore[arg-type]


# ---------------------------------------------------------------------------
# End-to-end: hostile config -> generated C -> compiler
# ---------------------------------------------------------------------------

_HOSTILE_CONFIG = textwrap.dedent(
    """\
    schema_version: 1
    metadata:
      ecu_name: "HostileECU"
      version: "1.0.0"
      description: "escaping regression fixture"
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
    dids:
      - id: "0x4001"
        name: "End*/Comment/*Break"
        data_length: 1
        access: ["read"]
        min_session: default
        read_security_level: 0
        write_security_level: 0
        description: "C comment terminator */ in a name"
      - id: "0x4002"
        name: "Say \\"Hello\\""
        data_length: 2
        access: ["read"]
        min_session: default
        read_security_level: 0
        write_security_level: 0
        description: "embedded \\"double quotes\\""
    """
)


@pytest.mark.skipif(not _TEMPLATES_OK, reason=f"templates not found at {_TEMPLATE_DIR}")
@pytest.mark.skipif(shutil.which("gcc") is None, reason="gcc not available")
def test_hostile_names_produce_compilable_c(tmp_path: Path) -> None:
    cfg = tmp_path / "diagnostics_config.yaml"
    cfg.write_text(_HOSTILE_CONFIG, encoding="utf-8")
    out = tmp_path / "generated"

    env = dict(os.environ, XALOQI_LICENSE_SKIP="1")
    proc = subprocess.run(
        [sys.executable, str(_CODEGEN), "--config", str(cfg), "--out", str(out),
         "--template-dir", str(_TEMPLATE_DIR), "--no-manifest"],
        capture_output=True, text=True, timeout=120, env=env,
    )
    assert proc.returncode == 0, f"codegen failed:\n{proc.stdout}\n{proc.stderr}"

    target = out / "did_handlers.c"
    assert target.is_file(), "did_handlers.c was not generated"

    cc = subprocess.run(
        ["gcc", "-fsyntax-only", "-std=c11",
         "-DEDS_MSG_BUF_MAX_STACK_BYTES=8192",
         f"-I{out}",
         f"-I{_REPO_ROOT / 'core'}", f"-I{_REPO_ROOT / 'transport'}",
         f"-I{_REPO_ROOT / 'platform'}", f"-I{_REPO_ROOT / 'config'}",
         str(target)],
        capture_output=True, text=True, timeout=120,
    )
    errors = [ln for ln in cc.stderr.splitlines() if ": error:" in ln]
    assert not errors, (
        "generated C does not compile with hostile DID names — the escaping "
        "regressed:\n" + "\n".join(errors[:10])
    )
