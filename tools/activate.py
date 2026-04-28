#!/usr/bin/env python3
"""
=============================================================================
Xaloqi EDS
FILE: tools/activate.py

PURPOSE: License key activation for Xaloqi EDS.

         Validates a license key JWT, confirms it is a genuine Xaloqi EDS
         key, and writes it to ~/.xaloqi/license.key for offline use by
         codegen.py.

         This file is PUBLIC — it is included in the public repository.
         It can validate and install keys but cannot generate them
         (the Ed25519 private key is never distributed).

USAGE:
    python3 tools/activate.py --key <YOUR_LICENSE_KEY>

    # Or via environment variable (CI, Docker):
    export XALOQI_LICENSE_KEY=<YOUR_LICENSE_KEY>
    python3 tools/activate.py --check

EXIT CODES:
    0  Activation successful (or --check with a valid key).
    1  Activation failed — invalid key, wrong product, expired key.
    2  Argument error.
=============================================================================
"""

from __future__ import annotations

import argparse
import os
import sys
from datetime import datetime, timezone
from pathlib import Path

# ---------------------------------------------------------------------------
# Locate _license.py (same directory as this script).
# activate.py is public; _license.py is private (delivered with license ZIP).
# ---------------------------------------------------------------------------

_TOOLS_DIR = Path(__file__).parent
sys.path.insert(0, str(_TOOLS_DIR))

try:
    import _license
    _HAS_LICENSE_MODULE = True
except ImportError:
    _HAS_LICENSE_MODULE = False


def _require_license_module() -> None:
    if not _HAS_LICENSE_MODULE:
        print(
            "ERROR: tools/_license.py not found.\n"
            "  _license.py is included in the Developer and Professional license ZIPs.\n"
            "  Place it in the tools/ directory alongside activate.py, then retry.\n"
            f"  Purchase a license at {_license.PURCHASE_URL if _HAS_LICENSE_MODULE else 'https://xaloqi.com'}",
            file=sys.stderr,
        )
        sys.exit(1)


def cmd_activate(key: str) -> None:
    """Validate and install a license key."""
    _require_license_module()

    key = key.strip()
    if not key:
        print("ERROR: License key cannot be empty.", file=sys.stderr)
        sys.exit(1)

    print("Validating license key...")

    result = _license.check.__wrapped__(key) if hasattr(_license.check, "__wrapped__") else None

    # Use the internal _verify_jwt directly for activation — we need to
    # validate the key before writing it, including expired keys (they
    # may be in grace period; we still write them so codegen can show
    # the grace warning).
    try:
        claims = _license._verify_jwt(key)
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)

    tier       = claims.get("tier", "unknown")
    email      = claims.get("sub", "unknown")
    exp_ts     = int(claims.get("exp", 0))
    exp_dt     = datetime.fromtimestamp(exp_ts, tz=timezone.utc)
    exp_str    = exp_dt.strftime("%Y-%m-%d")

    import time
    now        = int(time.time())
    days_left  = (exp_ts - now) // 86400

    # Write to ~/.xaloqi/license.key
    license_dir = _license.LICENSE_FILE_PATH.parent
    license_dir.mkdir(parents=True, exist_ok=True)

    _license.LICENSE_FILE_PATH.write_text(key + "\n", encoding="utf-8")

    # Set restrictive permissions on Unix (not critical but good practice).
    try:
        _license.LICENSE_FILE_PATH.chmod(0o600)
    except OSError:
        pass  # Windows — ignore

    print()
    print("=" * 60)
    print("  Xaloqi EDS — License Activated")
    print("=" * 60)
    print(f"  Email    : {email}")
    print(f"  Tier     : {tier.capitalize()}")
    print(f"  Expires  : {exp_str}", end="")

    if days_left < 0:
        days_expired = abs(days_left)
        grace_left   = _license.GRACE_PERIOD_DAYS - days_expired
        if grace_left > 0:
            print(f"  ⚠  EXPIRED {days_expired}d ago — {grace_left}d grace period remains")
        else:
            print(f"  ✗  EXPIRED — grace period over. Renew at {_license.PURCHASE_URL}")
    elif days_left <= 30:
        print(f"  ({days_left} days remaining — renewal recommended)")
    else:
        print(f"  ({days_left} days remaining)")

    print(f"  Saved to : {_license.LICENSE_FILE_PATH}")
    print("=" * 60)
    print()
    print("  Run codegen to confirm activation:")
    print("    python3 tools/codegen.py --config diagnostics_config.yaml --out generated/")
    print()


def cmd_check() -> None:
    """Check the currently installed license key."""
    _require_license_module()

    # Check env var first.
    env_key = os.environ.get(_license.LICENSE_ENV_VAR, "").strip()
    if env_key:
        print(f"License source: environment variable {_license.LICENSE_ENV_VAR}")
    elif _license.LICENSE_FILE_PATH.exists():
        print(f"License source: {_license.LICENSE_FILE_PATH}")
    else:
        print(
            "No license key found.\n"
            f"  File path : {_license.LICENSE_FILE_PATH}\n"
            f"  Env var   : {_license.LICENSE_ENV_VAR}\n"
            f"\n"
            f"  Purchase a license at {_license.PURCHASE_URL}\n"
            f"  Then activate: python3 tools/activate.py --key <YOUR_KEY>"
        )
        sys.exit(1)

    result = _license.check()

    import time
    exp_dt  = datetime.fromtimestamp(result.expires_at, tz=timezone.utc)
    exp_str = exp_dt.strftime("%Y-%m-%d") if result.expires_at else "N/A"

    print()
    print("=" * 60)
    print("  Xaloqi EDS — License Status")
    print("=" * 60)
    print(f"  Status   : {result.status.value.upper()}")
    print(f"  Email    : {result.email or 'N/A'}")
    print(f"  Tier     : {result.tier.capitalize() or 'N/A'}")
    print(f"  Expires  : {exp_str}")
    print(f"  Days left: {result.days_left}")
    print("=" * 60)

    if result.message:
        print()
        print(result.message)

    sys.exit(0 if result.is_usable else 1)


def cmd_deactivate() -> None:
    """Remove the locally stored license key."""
    _require_license_module()

    if _license.LICENSE_FILE_PATH.exists():
        _license.LICENSE_FILE_PATH.unlink()
        print(f"License key removed from {_license.LICENSE_FILE_PATH}")
    else:
        print(f"No license key file found at {_license.LICENSE_FILE_PATH}")

    env_key = os.environ.get(_license.LICENSE_ENV_VAR, "").strip()
    if env_key:
        print(
            f"Note: {_license.LICENSE_ENV_VAR} is still set in your environment.\n"
            f"      Unset it to fully deactivate: unset {_license.LICENSE_ENV_VAR}"
        )


def main() -> None:
    parser = argparse.ArgumentParser(
        prog="activate.py",
        description="Xaloqi EDS license key activation tool.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            "  # Activate a new license key:\n"
            "  python3 tools/activate.py --key eyJhbGciOiJFZERTQSJ9...\n"
            "\n"
            "  # Check the currently installed key:\n"
            "  python3 tools/activate.py --check\n"
            "\n"
            "  # Remove the locally stored key:\n"
            "  python3 tools/activate.py --deactivate\n"
            "\n"
            "  # Activate from environment variable (CI/Docker):\n"
            "  export XALOQI_LICENSE_KEY=eyJhbGciOiJFZERTQSJ9...\n"
            "  python3 tools/activate.py --check\n"
        ),
    )

    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument(
        "--key", "-k",
        metavar="LICENSE_KEY",
        help="License key JWT string to activate.",
    )
    group.add_argument(
        "--check", "-c",
        action="store_true",
        help="Check the currently installed license key and exit.",
    )
    group.add_argument(
        "--deactivate",
        action="store_true",
        help="Remove the locally stored license key file.",
    )

    args = parser.parse_args()

    if args.key:
        cmd_activate(args.key)
    elif args.check:
        cmd_check()
    elif args.deactivate:
        cmd_deactivate()


if __name__ == "__main__":
    main()
