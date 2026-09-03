"""
tests/test_license_expiry.py — License expiry simulation.

Patches time.time() to fake the current date, then calls _license.check()
against the real installed key. No system clock change required.

Three scenarios:
  1. Valid key   — 30 days before expiry
  2. Grace       — 10 days after expiry (within 14-day grace window)
  3. Expired     — 20 days after expiry (grace period over)

Collected by the canonical `pytest tests/` sweep (issue #227 — this file
used to be a standalone unittest-style script with only an
`if __name__ == "__main__":` runner block, so pytest silently collected
zero tests from it despite the filename matching its default discovery
pattern). Direct invocation still works too:

    cd /path/to/EDS
    python3 tests/test_license_expiry.py
"""
import sys
from datetime import datetime, timezone
from pathlib import Path
from unittest.mock import patch

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

# [ENV] tools/_license.py is a commercial, gitignored deliverable (CLAUDE.md /
# issue #67) — it is never present in a bare public checkout. Without this
# guard, pytest's collection of tests/ (issue #150) hard-errors here with a
# bare ModuleNotFoundError, indistinguishable at a glance from a real test
# failure. The "[ENV] " prefix lets `pytest`/run_python_tests.sh output tell
# "this environment is missing an optional/commercial component" apart from
# an actual regression.
_ENV_SKIP_REASON = (
    "[ENV] tools/_license.py not present (commercial gitignored deliverable, "
    "not part of the public repo checkout)"
)
try:
    import _license
except ImportError:
    if __name__ == "__main__":
        # `python3 tests/test_license_expiry.py` direct invocation.
        print(_ENV_SKIP_REASON)
        sys.exit(0)
    # Collected by pytest — turn the import error into a proper SKIP report
    # instead of a collection error.
    pytest.skip(_ENV_SKIP_REASON, allow_module_level=True)

DAY = 86400


def _real_expires_at() -> int:
    """Extract exp timestamp from the installed key without checking expiry."""
    raw = _license._load_raw_key()
    if raw is None:
        raise RuntimeError(
            "No license key found. Activate one first:\n"
            "  python3 tools/activate.py --key <YOUR_KEY>"
        )
    claims = _license._verify_jwt(raw)
    return int(claims["exp"])


@pytest.fixture(scope="module")
def expires_at() -> int:
    """Real expiry timestamp of the installed key.

    Skips (not fails) if no key is activated yet — same "environment
    setup gap, not a regression" spirit as the _license import guard
    above, since a fresh commercial checkout with _license.py present
    but no key activated is a real, expected local-dev state.
    """
    try:
        return _real_expires_at()
    except RuntimeError as e:
        pytest.skip(f"[ENV] {e}")


def _check_at(fake_now: int) -> "_license.LicenseResult":
    with patch("_license.time") as mock_time:
        mock_time.time.return_value = fake_now
        return _license.check()


def test_valid_license_30_days_before_expiry(expires_at: int) -> None:
    result = _check_at(expires_at - 30 * DAY)
    assert result.status == _license.LicenseStatus.OK, (
        f"expected OK 30 days before expiry, got {result.status.value} "
        f"(days_left={result.days_left}, msg={result.message!r})"
    )


def test_grace_period_10_days_after_expiry(expires_at: int) -> None:
    result = _check_at(expires_at + 10 * DAY)
    assert result.status == _license.LicenseStatus.GRACE, (
        f"expected GRACE 10 days after expiry (within the "
        f"{_license.GRACE_PERIOD_DAYS}-day grace window), got "
        f"{result.status.value} (days_left={result.days_left}, "
        f"msg={result.message!r})"
    )


def test_expired_20_days_after_expiry(expires_at: int) -> None:
    result = _check_at(expires_at + 20 * DAY)
    assert result.status == _license.LicenseStatus.EXPIRED, (
        f"expected EXPIRED 20 days after expiry (past the "
        f"{_license.GRACE_PERIOD_DAYS}-day grace window), got "
        f"{result.status.value} (days_left={result.days_left}, "
        f"msg={result.message!r})"
    )


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
