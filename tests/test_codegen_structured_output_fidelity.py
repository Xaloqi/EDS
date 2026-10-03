"""
tests/test_codegen_structured_output_fidelity.py — regression tests for the
2026-10-03 validation campaign finding: codegen applied ``_c_safe_text()`` —
a *C source* escaper — to values it then emitted into **JSON** and
**TypeScript**, where ``json.dumps`` escapes them a second time.

The result is silent data corruption in two machine-readable interop
artifacts, with codegen exiting 0:

    diagnostics_config.yaml   name: 9lives"quote
    sovd_cda.json             "name": "9lives\\\\\\"quote"   -> 9lives\\"quote
    catalog.ts                 name: "9lives\\\\\\"quote"    -> 9lives\\"quote

``ecuIdentification`` in the same SOVD document was already correct (raw value
+ ``json.dumps``), which is the in-repo precedent these tests lock in: a value
bound for JSON/TS must reach ``json.dumps`` **unescaped**. ``_c_safe_text()``
remains correct — and still required — for C, CAPL and Python output.

Also covered here: ``_c_identifier()`` collisions. Two DIDs whose names differ
only by separator (``Foo-Bar`` / ``Foo_Bar`` / ``Foo Bar``) normalise to the
same C identifier, and codegen used to exit 0 while emitting C with duplicate
definitions:

    error: redefinition of 's_mock_foo_bar'
    error: redefinition of 'did_read_foo_bar'

Recorded as a known deferred item by the 2026-08-31 campaign; fixed with the
escaping work because both are codegen name-handling defects.

Run:  pytest tests/test_codegen_structured_output_fidelity.py -v
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

_needs_templates = pytest.mark.skipif(
    not _TEMPLATES_OK,
    reason=f"commercial templates not present at {_TEMPLATE_DIR}",
)

# The exact characters that `_c_safe_text()` rewrites: a double quote, a
# backslash and a C comment terminator. All three are legal in a JSON string
# and in an AUTOSAR LONG-NAME, which is where real configs get them from.
_HOSTILE_DID_NAME = '9lives"quote'
_HOSTILE_DTC_DESC = 'Sensor "A" circuit high — see OEM spec \\ section 4'
_HOSTILE_ROUTINE_NAME = '7Routine"Grün\\x'

_FIDELITY_CONFIG = textwrap.dedent(
    f"""\
    schema_version: 1
    metadata:
      ecu_name: "FidelityECU"
      version: "1.0.0"
      description: "structured-output fidelity fixture"
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
      - id: "0xF190"
        name: {json.dumps(_HOSTILE_DID_NAME)}
        data_length: 4
        access: ["read"]
    dtcs:
      - code: "0x900100"
        description: {json.dumps(_HOSTILE_DTC_DESC)}
        severity: check_at_next_halt
    routines:
      - id: "0xBB00"
        name: {json.dumps(_HOSTILE_ROUTINE_NAME)}
        min_session: extended
        security_level: 0
        support: ["start"]
    """
)

# Two DIDs and two routines whose names collapse to one C identifier each.
_COLLIDING_CONFIG = textwrap.dedent(
    """\
    schema_version: 1
    metadata:
      ecu_name: "CollideECU"
      version: "1.0.0"
      description: "identifier collision fixture"
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
      - id: "0xF190"
        name: "Foo-Bar"
        data_length: 4
        access: ["read"]
      - id: "0xF191"
        name: "Foo_Bar"
        data_length: 4
        access: ["read"]
      - id: "0xF192"
        name: "Foo Bar"
        data_length: 4
        access: ["read"]
    """
)


def _run_codegen(tmp_path: Path, config_text: str, *extra: str):
    cfg = tmp_path / "diagnostics_config.yaml"
    cfg.write_text(config_text, encoding="utf-8")
    out = tmp_path / "generated"
    proc = subprocess.run(
        [sys.executable, str(_CODEGEN),
         "--config", str(cfg), "--out", str(out),
         "--template-dir", str(_TEMPLATE_DIR), "--no-manifest", *extra],
        capture_output=True, text=True, timeout=120,
    )
    return proc, out


# ---------------------------------------------------------------------------
# sovd_cda.json — the OpenSOVD 1.0 interop contract
# ---------------------------------------------------------------------------

@_needs_templates
def test_sovd_cda_names_round_trip_exactly(tmp_path: Path) -> None:
    """Every SOVD name/description must decode back to the config value.

    json.dumps already escapes for JSON. Pre-escaping with _c_safe_text()
    doubles every backslash and quote, so an OEM SOVD client reads a name
    that does not match the ECU's own configuration.
    """
    proc, out = _run_codegen(tmp_path, _FIDELITY_CONFIG, "--sovd")
    assert proc.returncode == 0, f"codegen failed:\n{proc.stdout}\n{proc.stderr}"

    cda = json.loads((out / "sovd_cda.json").read_text(encoding="utf-8"))

    assert cda["dataIdentifiers"][0]["name"] == _HOSTILE_DID_NAME
    assert cda["dtcs"][0]["description"] == _HOSTILE_DTC_DESC
    assert cda["routines"][0]["name"] == _HOSTILE_ROUTINE_NAME
    # Already correct before the fix — locked in so it cannot regress the
    # other way (i.e. someone "consistently" adding _c_safe_text here too).
    assert cda["ecuIdentification"]["name"] == "FidelityECU"


@_needs_templates
def test_sovd_cda_contains_no_double_escaping(tmp_path: Path) -> None:
    """A literal backslash-backslash-quote is the signature of the defect."""
    proc, out = _run_codegen(tmp_path, _FIDELITY_CONFIG, "--sovd")
    assert proc.returncode == 0, proc.stderr
    raw = (out / "sovd_cda.json").read_text(encoding="utf-8")
    assert '\\\\"' not in raw, (
        "sovd_cda.json contains a double-escaped quote — a C escaper ran "
        "before json.dumps"
    )


# ---------------------------------------------------------------------------
# catalog.ts — the GUI dashboard catalogue
# ---------------------------------------------------------------------------

@_needs_templates
def test_gui_catalog_names_round_trip_exactly(tmp_path: Path) -> None:
    """generate_gui_types() reuses the C template context builders, so it
    inherited the same pre-escaping even though it emits TypeScript."""
    gui_out = tmp_path / "gui_generated"
    proc, _ = _run_codegen(
        tmp_path, _FIDELITY_CONFIG, "--gui-types", "--gui-out", str(gui_out)
    )
    assert proc.returncode == 0, f"codegen failed:\n{proc.stdout}\n{proc.stderr}"

    text = (gui_out / "catalog.ts").read_text(encoding="utf-8")

    for anchor, expected in (
        ("0xF190", _HOSTILE_DID_NAME),
        ("0xBB00", _HOSTILE_ROUTINE_NAME),
    ):
        line = next(
            (ln for ln in text.splitlines() if anchor in ln and "name:" in ln),
            None,
        )
        assert line is not None, f"no catalogue entry for {anchor}"
        match = re.search(r'name:\s*("(?:\\.|[^"\\])*")', line)
        assert match is not None, f"could not parse name from: {line}"
        # A TS double-quoted string literal is JSON-decodable.
        assert json.loads(match.group(1)) == expected, (
            f"{anchor}: catalog.ts name does not round-trip"
        )


# ---------------------------------------------------------------------------
# _c_identifier() collisions
# ---------------------------------------------------------------------------

@pytest.mark.parametrize(
    "a, b",
    [
        ("Foo-Bar", "Foo_Bar"),
        ("Foo-Bar", "Foo Bar"),
        ("Engine-Speed", "Engine Speed"),
    ],
)
def test_distinct_names_that_collide_are_detected(a: str, b: str) -> None:
    """Names differing only by separator normalise to one C identifier."""
    import importlib.util

    spec = importlib.util.spec_from_file_location("_codegen_collide", _CODEGEN)
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(mod)
    assert mod._c_identifier(a) == mod._c_identifier(b), "fixture is not colliding"


@_needs_templates
def test_colliding_did_names_fail_validation(tmp_path: Path) -> None:
    """Codegen must refuse, not emit C with duplicate definitions.

    Before the fix codegen exited 0 and gcc reported
    'redefinition of did_read_foo_bar'. A validation error (exit 1) naming
    both DIDs is the documented contract for this class (see codegen.py's
    --help: 'Validation error (duplicate DID, invalid ID range, ...)').
    """
    proc, _ = _run_codegen(tmp_path, _COLLIDING_CONFIG)
    assert proc.returncode != 0, (
        "codegen exited 0 on colliding DID names — it will emit C that does "
        f"not compile:\n{proc.stdout}"
    )
    combined = (proc.stdout + proc.stderr).lower()
    assert "0xf191" in combined or "foo_bar" in combined, (
        "collision error does not identify the offending DID:\n"
        f"{proc.stdout}\n{proc.stderr}"
    )
