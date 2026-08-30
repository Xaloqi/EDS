#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Guard against stale unit-test counts and dead script paths in prose docs.

Filed as #119: the `Verify test count` CI step keeps `.github/workflows/ci.yml`
honest about how many unit-test modules exist, but nothing guarded the same
number where it appears in prose — docs drifted to 42 and 35 against an actual
43, and pointed at a `scripts/build_tests.sh` that has never existed (the
script lives at the repo root).

This script is the prose-side counterpart of that CI step:

  1. No tracked doc may reference `scripts/build_tests.sh` /
     `scripts/build_harness.sh`.
  2. Any unit-test module count stated in prose must equal the real count,
     derived from `tests/unit_runnable/test_*.c`.

CHANGELOG.md is exempt: its entries are a historical record of what was true
at the time and must not be rewritten.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# CHANGELOG.md records what was true at the time; PHASE1_SECURITY_CHANGES.md
# declares itself a historical record in its own header. Neither must be
# rewritten to match today's count.
EXEMPT = {"CHANGELOG.md", "PHASE1_SECURITY_CHANGES.md"}

DEAD_PATHS = ("scripts/build_tests.sh", "scripts/build_harness.sh")

# Each pattern must capture the stated count in group 1.
COUNT_PATTERNS = (
    r"(\d+)\s+Unity\s+(?:C\s+)?(?:unit\s+)?test\s+modules?",
    r"(\d+)\s+Unity\s+modules?",
    r"(\d+)\s+unit\s+test\s+modules?",
    r"(\d+)\s+unit\s+tests?\b",
    r"Coverage\s+—\s+(\d+)\s+unit",
    r"unit\s+tests?\s+must\s+pass\s+\(currently\s+(\d+)\)",
    r"unit\s+test\s+to\s+fail\s+\(currently\s+(\d+)\s",
)


def real_module_count() -> int:
    n = len(list((ROOT / "tests" / "unit_runnable").glob("test_*.c")))
    if n == 0:
        sys.exit("FAIL: found no tests/unit_runnable/test_*.c — wrong repo root?")
    return n


def docs():
    for p in sorted(ROOT.rglob("*.md")):
        rel = p.relative_to(ROOT)
        if rel.name in EXEMPT or ".git" in rel.parts or "node_modules" in rel.parts:
            continue
        yield rel, p.read_text(encoding="utf-8")


def main() -> int:
    expected = real_module_count()
    problems = []

    for rel, text in docs():
        for lineno, line in enumerate(text.splitlines(), 1):
            for dead in DEAD_PATHS:
                if dead in line:
                    problems.append(
                        f"{rel}:{lineno}: references `{dead}`, which does not exist "
                        f"— the script lives at the repo root"
                    )
            # Several patterns can match the same phrase; report each
            # (file, line, stated count) once, with the longest match as
            # the most descriptive quote.
            hits = {}
            for pat in COUNT_PATTERNS:
                for m in re.finditer(pat, line, re.IGNORECASE):
                    stated = int(m.group(1))
                    if stated != expected:
                        quote = m.group(0).strip()
                        if len(quote) > len(hits.get(stated, "")):
                            hits[stated] = quote
            for stated, quote in sorted(hits.items()):
                problems.append(
                    f"{rel}:{lineno}: states {stated} unit test modules, "
                    f"actual is {expected} — {quote!r}"
                )

    if problems:
        print("FAIL: stale references in prose docs (#119 guard)\n")
        for p in problems:
            print(f"  {p}")
        print(
            f"\n{len(problems)} problem(s). Update the docs, or widen "
            f"scripts/verify_doc_counts.py if a match is a false positive."
        )
        return 1

    print(f"PASS: prose docs agree with the real unit-test count ({expected}) "
          f"and reference no dead script paths.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
