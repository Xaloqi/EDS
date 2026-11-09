"""
tests/test_no_bare_asil_wording.py — customer-visible text must not present
"ASIL-B" as a property of the tool (wording rule decided 2026-11-09; the
toolchain's tests/test_no_bare_asil_badge.py enforces the same rule on its side).

Three cases for "ASIL-B":
  1. LEVEL NAMES stay: the value of a level the customer selects
     ("2 = ASIL-B", "ASIL Level: B", "#error ... must be 1 for ASIL-B.").
  2. OUTCOMES / PRODUCT DESCRIPTIONS say what was checked, or "ASIL-B-oriented".
  3. RULE TAGS name our rules ("EDS safety rule"), not the standard.
Identifiers (ASIL_B_MAX_DID_DATA_LEN, validate_asil_b, --asil-level B) never change.

Not covered on purpose: the safety documents (docs/Safety_Model.md and the
EDS-Safety package have their own review) and C source comments under core/,
transport/, config/, platform/ (tracked separately).
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SCOPED = (
    "tools/codegen.py",
    "README.md",
    "INSTALL.md",
    "CONTRIBUTING.md",
    "SECURITY.md",
    "docs/*.md",
    "examples/**/*",
)
EXCLUDED = ("docs/Safety_Model.md",)

FORBIDDEN = {
    "rule tag names the standard": r"EDS \[ASIL-B\]",
    "claims compliance/certification/qualification":
        r"ASIL-B(?!-oriented)\W+(?:compli|certif|qualif)|(?:compli\w*|certif\w*|qualif\w*) (?:with|to|for) ASIL-B",
    "claims validation against the level":
        r"(?i)validated against ASIL|ASIL\S* (?:safety )?validation (?:passed|active)",
    "outcome line next to ASIL": r"(?i)validation passed[^\n]*ASIL|ASIL[^\n]*validation passed",
    "bare ASIL-B describing our product or rules":
        r"ASIL-B(?!-oriented)(?! \*?candidate)(?:[ \t]+(?!builds?\b)[A-Za-z0-9*]|:|\*\*|\))",
}
# Case 1 lines that legitimately put a word after the level name. Exact substrings.
ALLOWED_LINES = (
    "ASIL-A/B builds",
    "before claiming certified ASIL-B status",   # INSTALL.md disclaimer: names the claim the customer must not make yet
    "If your ECU targets ISO 26262 ASIL-B",   # the customer's own target
    "Per-DID safe accessor",   # "(ASIL-{{ asil_level }})": the selected level's value
    "ISO 26262 ASIL-D functional safety target",  # an example ECU's own target (bms_ecu)
)


def _scoped_files():
    seen = set()
    for pattern in SCOPED:
        for p in ROOT.glob(pattern):
            rel = p.relative_to(ROOT).as_posix()
            if (p.is_file() and rel not in EXCLUDED and p.suffix not in {".png", ".ico", ".svg", ".bin", ".elf"}
                    and p not in seen):
                seen.add(p)
                yield p


def test_wording_rule_no_forbidden_asil_phrases():
    hits = []
    for path in _scoped_files():
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for n, line in enumerate(text.splitlines(), 1):
            if any(ok in line for ok in ALLOWED_LINES):
                continue
            for why, rx in FORBIDDEN.items():
                if re.search(rx, line):
                    hits.append(f"{path.relative_to(ROOT)}:{n}: {why}: {line.strip()[:110]}")
    assert not hits, "ASIL wording rule violated (see module docstring):\n" + "\n".join(hits)


def test_status_line_names_the_rule_set():
    src = (ROOT / "tools/codegen.py").read_text(encoding="utf-8")
    assert "EDS safety rules: passed (ASIL-{asil_level}-oriented checks)" in src
