"""
tests/test_license_expiry.py — License expiry simulation.

Patches time.time() to fake the current date, then calls _license.check()
against the real installed key. No system clock change required.

Three scenarios:
  1. Valid key   — 30 days before expiry
  2. Grace       — 10 days after expiry (within 14-day grace window)
  3. Expired     — 20 days after expiry (grace period over)

Run:
    cd /home/raul/xaloqi/EDS
    python3 tests/test_license_expiry.py

Exit 0 if all three scenarios produce the expected LicenseStatus.
"""
import sys
import time
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import _license


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


def _run_scenario(label: str, fake_now: int) -> _license.LicenseResult:
    with patch("_license.time") as mock_time:
        mock_time.time.return_value = fake_now
        result = _license.check()
    return result


def main() -> int:
    print("=" * 60)
    print("  Xaloqi EDS — License Expiry Simulation")
    print("=" * 60)

    try:
        exp = _real_expires_at()
    except RuntimeError as e:
        print(f"\nERROR: {e}")
        return 1

    from datetime import datetime, timezone
    exp_dt = datetime.fromtimestamp(exp, tz=timezone.utc).strftime("%Y-%m-%d")
    print(f"\n  Real expiry date : {exp_dt}")
    print(f"  Grace period     : {_license.GRACE_PERIOD_DAYS} days")
    print()

    DAY = 86400
    scenarios = [
        ("SCENARIO 1 — Valid (30 days before expiry)",
         exp - 30 * DAY,
         _license.LicenseStatus.OK),
        ("SCENARIO 2 — Grace period (10 days after expiry)",
         exp + 10 * DAY,
         _license.LicenseStatus.GRACE),
        ("SCENARIO 3 — Expired (20 days after expiry, grace over)",
         exp + 20 * DAY,
         _license.LicenseStatus.EXPIRED),
    ]

    all_pass = True
    for label, fake_now, expected_status in scenarios:
        fake_date = datetime.fromtimestamp(fake_now, tz=timezone.utc).strftime("%Y-%m-%d")
        result = _run_scenario(label, fake_now)

        ok = result.status == expected_status
        icon = "PASS" if ok else "FAIL"
        print(f"[{icon}] {label}")
        print(f"       Simulated date : {fake_date}")
        print(f"       Expected status: {expected_status.value}")
        print(f"       Actual status  : {result.status.value}")
        print(f"       days_left      : {result.days_left}")
        if result.message:
            for line in result.message.strip().splitlines():
                print(f"       msg: {line}")
        print()

        if not ok:
            all_pass = False

    print("=" * 60)
    print(f"  Result: {'ALL PASS' if all_pass else 'FAILURES DETECTED'}")
    print("=" * 60)
    return 0 if all_pass else 1


if __name__ == "__main__":
    sys.exit(main())
