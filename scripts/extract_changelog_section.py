#!/usr/bin/env python3
"""Extract one version's section from a Keep-a-Changelog CHANGELOG.md,
for use as GitHub Release notes.

Used by .github/workflows/release.yml so a Release's notes are drawn
mechanically from the tagged CHANGELOG section rather than depending on
someone hand-drafting them — that dependency is exactly how a tag has
shipped with no public Release before (O-10, xaloqi-knowledge
reviews/FINDINGS.md): a manual step with no hard trigger is a manual step
that gets skipped.

Finds the line matching `## [<version>]` (anything may follow on that
line, e.g. " — 2026-08-27" or " *(tag updated ...)*"), and returns
everything up to (not including) the next `## [` heading, with leading/
trailing blank lines and a lone trailing `---` separator stripped.

Usage:
    python3 scripts/extract_changelog_section.py CHANGELOG.md 1.10.1

Exit 1 with a message on stderr if the version heading isn't found —
loud failure, not an empty/silent release body.
"""
from __future__ import annotations

import re
import sys


def extract(changelog_text: str, version: str) -> str:
    lines = changelog_text.splitlines()
    heading_re = re.compile(r"^## \[")
    target_re = re.compile(r"^## \[" + re.escape(version) + r"\]")

    start = None
    for i, line in enumerate(lines):
        if target_re.match(line):
            start = i + 1
            break
    if start is None:
        raise SystemExit(
            f"ERROR: no '## [{version}]' heading found in changelog. "
            "Was the CHANGELOG roll (runbook step 1) actually done before tagging?"
        )

    end = len(lines)
    for j in range(start, len(lines)):
        if heading_re.match(lines[j]):
            end = j
            break

    section = lines[start:end]

    # Trim leading/trailing blank lines, and a lone trailing '---' separator
    # (the format between sections is: content, blank, '---', blank, next heading).
    while section and not section[0].strip():
        section.pop(0)
    while section and not section[-1].strip():
        section.pop()
    if section and section[-1].strip() == "---":
        section.pop()
        while section and not section[-1].strip():
            section.pop()

    return "\n".join(section)


def main() -> int:
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} CHANGELOG.md VERSION", file=sys.stderr)
        return 1
    path, version = sys.argv[1], sys.argv[2]
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    print(extract(text, version))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
