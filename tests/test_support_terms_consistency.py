"""
tests/test_support_terms_consistency.py — one-fact-one-home guard for the
customer-facing support terms.

2026-10-03 validation campaign: the Professional support SLA was stated four
different ways across artifacts a customer actually receives —

    INSTALL.md (both ZIPs)        "2 business days (Developer) /
                                   5 business days SLA (Professional)"
    LICENSE_COMMERCIAL.txt        "within 5 business days"   <- the contract
    docs/COMMERCIAL_ONBOARDING.md "within 1 business day"
    EDS-Safety CUSTOMER_NOTICE.md "5 business day SLA"

INSTALL.md made the EUR 1,990 tier look *slower* than the cheaper one and
invented a Developer-tier guarantee the contract explicitly denies, while the
onboarding doc promised 1 business day against a contract that says 5 — and
routed customers to support@xaloqi.com, an address that appears nowhere else
in the product or on the website.

`LICENSE_COMMERCIAL.txt` is the binding document, so it is the home of this
fact. Everything else must agree with it or not state a number at all.
"""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parent.parent
_CONTRACT = _REPO_ROOT / "LICENSE_COMMERCIAL.txt"

#: Docs that may legitimately quote a response time. Any other occurrence is
#: fine too — the assertion is about the *number*, not about who may mention it.
_CUSTOMER_DOCS = [
    "INSTALL.md",
    "LICENSE_COMMERCIAL.txt",
    "README.md",
    "COMMERCIAL_NOTICE.md",
    "CONTRIBUTING.md",
    "docs/COMMERCIAL_ONBOARDING.md",
]

#: SECURITY.md is deliberately absent from the list above: it describes the
#: vulnerability-disclosure timetable (5 / 15 business days), which is a
#: different commitment from the commercial support SLA.

_BUSINESS_DAYS = re.compile(r"(\d+)\s+business\s+days?", re.IGNORECASE)


def _contract_response_days() -> str:
    """The single authoritative number, read from the contract itself."""
    text = _CONTRACT.read_text(encoding="utf-8")
    match = re.search(
        r"Professional tier:.*?within\s+(\d+)\s+business\s+days",
        text,
        re.IGNORECASE | re.DOTALL,
    )
    assert match is not None, (
        "LICENSE_COMMERCIAL.txt no longer states a Professional response time "
        "in the expected form — update this guard deliberately, not by deleting it."
    )
    return match.group(1)


def test_contract_states_a_response_time() -> None:
    assert _contract_response_days().isdigit()


@pytest.mark.parametrize("rel", _CUSTOMER_DOCS)
def test_no_doc_contradicts_the_contract_sla(rel: str) -> None:
    """Every business-day figure in a customer doc must match the contract."""
    path = _REPO_ROOT / rel
    if not path.is_file():
        pytest.skip(f"{rel} not present in this checkout")

    expected = _contract_response_days()
    found = set(_BUSINESS_DAYS.findall(path.read_text(encoding="utf-8")))
    bad = found - {expected}
    assert not bad, (
        f"{rel} states a response time of {sorted(bad)} business day(s) while "
        f"LICENSE_COMMERCIAL.txt commits to {expected}. The contract is the "
        "home of this fact — change it there, or stop stating a number here."
    )


def test_support_mailbox_is_consistent() -> None:
    """support@xaloqi.com is not a mailbox we publish anywhere else."""
    # Tracked files only. Scanning the working tree picks up untracked local
    # artifacts (stale agent worktrees under .claude/, build output) that are
    # not part of the product and would make this guard fail for the wrong
    # reason on a developer machine.
    listing = subprocess.run(
        ["git", "ls-files", "-z", "*.md", "*.txt", "*.html"],
        cwd=_REPO_ROOT, capture_output=True, text=True, timeout=60,
    )
    assert listing.returncode == 0, f"git ls-files failed: {listing.stderr}"

    # CHANGELOG.md records what the docs *used* to say, so it legitimately
    # names the retired address. The guard is about documents that route a
    # customer somewhere, not about the historical record.
    historical = {"CHANGELOG.md"}

    offenders = []
    for rel in filter(None, listing.stdout.split("\0")):
        if rel in historical:
            continue
        path = _REPO_ROOT / rel
        if not path.is_file():
            continue
        if "support@xaloqi.com" in path.read_text(encoding="utf-8", errors="replace"):
            offenders.append(rel)
    assert not offenders, (
        "these files route customers to support@xaloqi.com; every other "
        f"document and the website use contact@xaloqi.com: {offenders}"
    )
