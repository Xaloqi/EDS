"""EDS#612: a doc must not pair an NRC number with a name that is not its own.

README.md described the DID validation chain with "NRC 0x6F
conditionsNotCorrect". 0x6F is not an ISO 14229-1 NRC and
conditionsNotCorrect is 0x22. This test reads every "0xNN <camelCaseName>"
pair in the public docs and checks it against the uds_nrc_t enum in
core/uds_types.h, which is what the stack sends.
"""
import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
DOCS = [ROOT / "README.md", ROOT / "INSTALL.md", *sorted((ROOT / "docs").glob("*.md"))]

ENUM = re.compile(r"\bUDS_NRC_([A-Z0-9_]+)\s*=\s*0x([0-9A-Fa-f]{2})U?")
# "0x31 requestOutOfRange", "`0x31` | requestOutOfRange", "NRC 0x33 (securityAccessDenied)"
PAIR = re.compile(r"0x([0-9A-Fa-f]{2})`?\s*[|(:,—-]*\s*\(?([a-z]+(?:[A-Z][a-z]+){1,7})\b")


def _norm(name: str) -> str:
    return name.replace("_", "").lower()


def _enum() -> dict[str, int]:
    text = (ROOT / "core" / "uds_types.h").read_text(encoding="utf-8")
    table = {_norm(n): int(v, 16) for n, v in ENUM.findall(text)}
    assert len(table) > 20, "uds_nrc_t enum not found in core/uds_types.h"
    return table


def _pairs():
    for doc in DOCS:
        for lineno, line in enumerate(doc.read_text(encoding="utf-8").splitlines(), 1):
            for num, name in PAIR.findall(line):
                yield doc.relative_to(ROOT), lineno, int(num, 16), name


def test_pattern_sees_the_original_defect():
    assert PAIR.findall("Step 4  Access type permitted?   → NRC 0x6F conditionsNotCorrect") == [
        ("6F", "conditionsNotCorrect")]


def test_doc_nrc_names_match_the_enum():
    enum = _enum()
    wrong = [f"{doc}:{lineno}: 0x{num:02X} {name} (the stack's {name} is 0x{enum[_norm(name)]:02X})"
             for doc, lineno, num, name in _pairs()
             if _norm(name) in enum and enum[_norm(name)] != num]
    assert not wrong, "NRC number/name pairs that disagree with core/uds_types.h:\n" + "\n".join(wrong)


def test_the_check_reads_real_pairs():
    assert sum(1 for _ in _pairs()) >= 10, "the pattern matched almost nothing; it no longer guards the docs"
