#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Guard against stale unit-test counts and dead script paths in prose docs.

Filed as #119: the `Verify test count` CI step keeps `.github/workflows/ci.yml`
honest about how many unit-test modules exist, but nothing guarded the same
number where it appears in prose — docs drifted to 42 and 35 against an actual
43, and pointed at a `scripts/build_tests.sh` that has never existed (the
script lives at the repo root).

Widened by #170: both checks were blind to ASCII tree diagrams, which is
exactly where a path/count claim is most likely to be copied and left to rot.
`docs/ARCHITECTURE.md` had `└── scripts/` on one line and `build_tests.sh` as
its own leaf line below — the same dead path as `scripts/build_tests.sh`, but
never on one line, so the substring check never fired. It also had
`Canonical Unity unit tests (36 modules)`, a phrasing the original
COUNT_PATTERNS list didn't cover.

This script is the prose-side counterpart of that CI step:

  1. No tracked doc may reference `scripts/build_tests.sh` /
     `scripts/build_harness.sh` — inline, or split across a tree diagram's
     `scripts/` node and a `build_tests.sh`/`build_harness.sh` leaf line.
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
    # Tree-diagram annotation style, e.g. "unit tests (36 modules)" (#170).
    r"unit\s+tests?\s*\((\d+)\s+modules?\)",
)

# A tree-diagram leaf naming one of these under a `scripts/` node is the same
# dead path as DEAD_PATHS below, just split across two lines (#170). Matched
# as a bare leaf (optional tree-drawing prefix, nothing else on the line)
# so real prose sentences that merely mention the filename aren't flagged.
TREE_LEAF_RE = re.compile(r"^[\s│├└─]*(build_tests\.sh|build_harness\.sh)\b")
TREE_SCRIPTS_NODE_RE = re.compile(r"^[\s│├└─]*scripts/\s*$")


def real_module_count() -> int:
    n = len(list((ROOT / "tests" / "unit_runnable").glob("test_*.c")))
    if n == 0:
        sys.exit("FAIL: found no tests/unit_runnable/test_*.c — wrong repo root?")
    return n


def docs():
    for p in sorted(ROOT.rglob("*.md")):
        rel = p.relative_to(ROOT)
        if (
            rel.name in EXEMPT
            or ".git" in rel.parts
            or "node_modules" in rel.parts
            or ".claude" in rel.parts  # gitignored local agent-worktree scratch space
        ):
            continue
        yield rel, p.read_text(encoding="utf-8")


def main() -> int:
    expected = real_module_count()
    problems = []

    for rel, text in docs():
        in_fence = False
        saw_scripts_node_at = None  # lineno of the most recent `scripts/` tree node, or None
        for lineno, line in enumerate(text.splitlines(), 1):
            if line.strip().startswith("```"):
                in_fence = not in_fence
                if not in_fence:
                    saw_scripts_node_at = None
                continue

            for dead in DEAD_PATHS:
                if dead in line:
                    problems.append(
                        f"{rel}:{lineno}: references `{dead}`, which does not exist "
                        f"— the script lives at the repo root"
                    )

            if in_fence:
                if TREE_SCRIPTS_NODE_RE.match(line):
                    saw_scripts_node_at = lineno
                elif saw_scripts_node_at is not None:
                    m = TREE_LEAF_RE.match(line)
                    if m:
                        problems.append(
                            f"{rel}:{lineno}: tree diagram shows `{m.group(1)}` "
                            f"under the `scripts/` node opened at line "
                            f"{saw_scripts_node_at}, which does not exist "
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
