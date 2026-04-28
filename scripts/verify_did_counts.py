#!/usr/bin/env python3
# =============================================================================
# Xaloqi EDS
# FILE: scripts/verify_did_counts.py
#
# PURPOSE: Verify that GEN_SAFETY_DID_COUNT and GEN_DID_COUNT in the
#          generated headers match the actual 'dids:' list length in the
#          corresponding diagnostics_config.yaml.
#
# FIX [CRIT-1]: The naive approach of using
#
#     grep -c '- id:' diagnostics_config.yaml
#
#   returns the wrong number because both 'dids:' entries AND 'routines:'
#   entries use the '- id:' key notation.  For basic_ecu that gives 8
#   instead of 5 (5 DIDs + 3 routines).  The generated macros are correct
#   (codegen.py already uses len(cfg['dids'])), but any CI or tooling that
#   uses grep -c to count DIDs from the YAML will mismatch.
#
#   This script parses the YAML properly with PyYAML and compares the
#   len(cfg['dids']) value against both generated headers.
#
# USAGE:
#   python3 scripts/verify_did_counts.py \
#       --yaml  examples/basic_ecu/diagnostics_config.yaml \
#       --safety-header  generated/safety_config.h \
#       --config-header  generated/generated_config.h
#
#   Exit codes:
#     0  All counts consistent.
#     1  Count mismatch — details printed to stderr.
#     2  Argument error or file not found.
#
# CI INTEGRATION:
#   Add this step after codegen in any job that verifies generated files:
#
#     - name: Verify GEN_SAFETY_DID_COUNT matches YAML dids list
#       run: |
#         python3 scripts/verify_did_counts.py \
#           --yaml         examples/basic_ecu/diagnostics_config.yaml \
#           --safety-header generated/safety_config.h \
#           --config-header generated/generated_config.h
#
# SPDX-License-Identifier: Apache-2.0
# =============================================================================

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


# ---------------------------------------------------------------------------
# YAML parser (subset — only needs to count top-level list items safely)
# ---------------------------------------------------------------------------

def _load_yaml(path: Path) -> dict:
    """Load YAML using PyYAML (required).  Hard-fail if not installed."""
    try:
        import yaml  # type: ignore[import]
    except ImportError:
        print(
            "ERROR: PyYAML is not installed.  Run: pip install pyyaml",
            file=sys.stderr,
        )
        sys.exit(2)

    with path.open("r", encoding="utf-8") as fh:
        data = yaml.safe_load(fh)

    if not isinstance(data, dict):
        print(
            f"ERROR: {path} does not parse to a YAML mapping.",
            file=sys.stderr,
        )
        sys.exit(2)

    return data


# ---------------------------------------------------------------------------
# Header parser
# ---------------------------------------------------------------------------

def _extract_macro_value(header_path: Path, macro_name: str) -> int | None:
    """
    Extract the integer value of a #define macro from a C header.

    Handles both forms:
        #define GEN_SAFETY_DID_COUNT          (5U)
        #define GEN_DID_COUNT             (5U)

    Returns None if the macro is not found.
    """
    pattern = re.compile(
        r"^\s*#\s*define\s+" + re.escape(macro_name) + r"\s+\((\d+)U?\)",
        re.MULTILINE,
    )
    text = header_path.read_text(encoding="utf-8")
    m = pattern.search(text)
    if m is None:
        return None
    return int(m.group(1))


# ---------------------------------------------------------------------------
# Main verification logic
# ---------------------------------------------------------------------------

def verify(
    yaml_path: Path,
    safety_header_path: Path,
    config_header_path: Path,
) -> bool:
    """
    Compare DID counts from three sources.

    Returns True if all counts are consistent, False otherwise.
    Prints a human-readable summary to stdout in all cases.
    """
    ok = True

    # ── 1. Count from YAML (the ground truth) ──────────────────────────────
    cfg = _load_yaml(yaml_path)

    yaml_dids      = cfg.get("dids",     [])
    yaml_routines  = cfg.get("routines", [])
    yaml_dtcs      = cfg.get("dtcs",     [])

    yaml_did_count = len(yaml_dids)

    # Demonstrate why grep -c '- id:' is wrong — print both numbers so the
    # difference is visible in CI logs.
    grep_would_count = sum(
        1
        for section in (yaml_dids, yaml_routines)
        for entry in section
        if "id" in entry
    )

    print(f"YAML file           : {yaml_path}")
    print(f"  dids[]  length    : {yaml_did_count}  ← correct DID count")
    print(f"  routines[] length : {len(yaml_routines)}")
    print(f"  dtcs[]  length    : {len(yaml_dtcs)}")
    print(
        f"  grep -c '- id:' would return: {grep_would_count}"
        f"  (dids + routines — WRONG for DID-only checks)"
    )
    print()

    # ── 2. GEN_SAFETY_DID_COUNT from safety_config.h ───────────────────────
    safety_count = _extract_macro_value(safety_header_path, "GEN_SAFETY_DID_COUNT")
    if safety_count is None:
        print(
            f"ERROR: GEN_SAFETY_DID_COUNT not found in {safety_header_path}",
            file=sys.stderr,
        )
        ok = False
    else:
        status = "PASS" if safety_count == yaml_did_count else "FAIL"
        print(f"GEN_SAFETY_DID_COUNT ({safety_header_path.name}): {safety_count}  [{status}]")
        if safety_count != yaml_did_count:
            print(
                f"  MISMATCH: header has {safety_count}, YAML dids[] has {yaml_did_count}.",
                file=sys.stderr,
            )
            print(
                "  Re-run codegen:  python3 tools/codegen.py "
                "--config <yaml> --out <dir> --safety-wrappers --asil-level B",
                file=sys.stderr,
            )
            ok = False

    # ── 3. GEN_DID_COUNT from generated_config.h ───────────────────────────
    config_count = _extract_macro_value(config_header_path, "GEN_DID_COUNT")
    if config_count is None:
        print(
            f"ERROR: GEN_DID_COUNT not found in {config_header_path}",
            file=sys.stderr,
        )
        ok = False
    else:
        status = "PASS" if config_count == yaml_did_count else "FAIL"
        print(f"GEN_DID_COUNT       ({config_header_path.name}): {config_count}  [{status}]")
        if config_count != yaml_did_count:
            print(
                f"  MISMATCH: header has {config_count}, YAML dids[] has {yaml_did_count}.",
                file=sys.stderr,
            )
            ok = False

    # ── 4. Cross-check: safety and config headers must agree ───────────────
    if safety_count is not None and config_count is not None:
        if safety_count != config_count:
            print(
                f"\nERROR: GEN_SAFETY_DID_COUNT ({safety_count}) != "
                f"GEN_DID_COUNT ({config_count}) — headers are inconsistent.",
                file=sys.stderr,
            )
            ok = False

    print()
    if ok:
        print(f"PASS: all DID counts consistent ({yaml_did_count})")
    else:
        print("FAIL: DID count mismatch — see details above", file=sys.stderr)

    return ok


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Verify GEN_SAFETY_DID_COUNT and GEN_DID_COUNT match the YAML dids[] "
            "list length.  Uses proper YAML parsing — never grep."
        ),
    )
    parser.add_argument(
        "--yaml",
        required=True,
        metavar="PATH",
        help="Path to diagnostics_config.yaml",
    )
    parser.add_argument(
        "--safety-header",
        required=True,
        metavar="PATH",
        help="Path to generated/safety_config.h",
    )
    parser.add_argument(
        "--config-header",
        required=True,
        metavar="PATH",
        help="Path to generated/generated_config.h",
    )
    return parser.parse_args()


def main() -> None:
    args = _parse_args()

    yaml_path          = Path(args.yaml)
    safety_header_path = Path(args.safety_header)
    config_header_path = Path(args.config_header)

    for p in (yaml_path, safety_header_path, config_header_path):
        if not p.exists():
            print(f"ERROR: file not found: {p}", file=sys.stderr)
            sys.exit(2)

    ok = verify(yaml_path, safety_header_path, config_header_path)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
