"""
tests/test_rtm_claims_not_restated.py — the RTM's counts live in the RTM.

EDS-Safety's EDS_Requirements_Traceability_Matrix.csv is the single source
of truth for how many requirements there are and how each is covered. This
repo's docs restated it — "30 requirements (16 ASIL-B, 14 QM), all COVERED" —
and kept saying so after the RTM grew to 36 rows with REQ-TIMING-001 marked
PARTIAL (O-161, 2026-10-15). INSTALL.md ships in both commercial ZIPs, so an
assessor read "all COVERED" beside a matrix that says otherwise (2026-10-16
validation campaign, F5). Second time: O-46 corrected the same count on
2026-08-31, with nothing to stop it drifting again.

This repo cannot read the CSV (it ships from EDS-Safety), so the guard is the
one-fact-one-home rule: shipped docs describe the RTM, never restate its
counts or claim blanket coverage.

Run:  pytest tests/test_rtm_claims_not_restated.py -v
"""

from __future__ import annotations

import re
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parent.parent

# A count of requirements/rows next to the RTM, or a blanket coverage claim.
_CLAIM = re.compile(
    r"(\b\d+\s+(requirements|rows)\b|\ball\s+COVERED\b|\d+\s+ASIL-B,\s*\d+\s+QM)",
    re.IGNORECASE,
)
_RTM = re.compile(r"Traceability|\bRTM\b")


def _docs() -> list[Path]:
    return [_REPO_ROOT / "INSTALL.md", _REPO_ROOT / "README.md",
            *sorted((_REPO_ROOT / "docs").glob("*.md"))]


def test_no_doc_restates_rtm_counts_or_coverage() -> None:
    hits = []
    for doc in _docs():
        if not doc.is_file():
            continue
        for n, line in enumerate(doc.read_text(encoding="utf-8").splitlines(), 1):
            if _RTM.search(line) and _CLAIM.search(line):
                hits.append(f"{doc.relative_to(_REPO_ROOT)}:{n}: {line.strip()[:120]}")
    assert not hits, (
        "Docs restate the RTM's counts or claim blanket coverage; the CSV in "
        "EDS-Safety is the only home for those facts:\n  " + "\n  ".join(hits)
    )
