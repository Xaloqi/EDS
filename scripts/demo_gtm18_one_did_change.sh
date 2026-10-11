#!/usr/bin/env bash
# =============================================================================
# Xaloqi EDS
# scripts/demo_gtm18_one_did_change.sh
#
# PURPOSE: "Change one line, watch every artifact move" (GTM-18). Changes ONE
#          field in examples/ardep_ecu/diagnostics_config.yaml -- never the
#          committed file, always a scratch copy -- and regenerates every
#          artifact EDS derives from it, so you can see the propagation
#          yourself instead of taking our word for it.
#
#          Public tier (always runs): generated C sources/headers and the
#          SOVD CDA descriptor, via the public tools/codegen.py.
#
#          Commercial tier (runs only if tools/testgen.py resolves to a real
#          file -- Developer/Professional, see INSTALL.md): the generated
#          pytest suite and the CANoe CAPL suite, via tools/testgen.py.
#          Without it, this script says so and skips that half rather than
#          failing or silently claiming less than it did.
#
#          The one field: DID 0xF187 (VehicleManufacturerSparePartNumber,
#          ISO 14229-1 Annex F) -- data_length 11 -> 14. Chosen because a
#          length change touches buffer sizes, length checks, PDU-length
#          arithmetic and NRC boundary tests everywhere it appears, which is
#          exactly what makes the diff worth looking at.
#
#          Full writeup, including why "generate TO these formats FROM a
#          definition we own" is not the same claim that burned the
#          opensovd-core team, and why sovd_cda.json is EDS's own CDA
#          descriptor rather than an ASAM SOVD or ISO 17978 artifact:
#          https://xaloqi.com/blog/change-one-line-four-artifacts
#
# USAGE:
#   bash scripts/demo_gtm18_one_did_change.sh
#
# EXIT CODES:
#   0  Demo ran to completion (commercial-tier half included or skipped, both OK).
#   1  codegen.py failed, or (when present) testgen.py failed.
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CODEGEN="$REPO_ROOT/tools/codegen.py"
TESTGEN="$REPO_ROOT/tools/testgen.py"
CONFIG="$REPO_ROOT/examples/ardep_ecu/diagnostics_config.yaml"

for f in "$CODEGEN" "$CONFIG"; do
  if [ ! -r "$f" ]; then
    echo "ERROR: required file not found or not readable: $f" >&2
    exit 1
  fi
done

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

echo "============================================================"
echo "  GTM-18 demo: change one DID, watch every artifact move"
echo "============================================================"
echo
echo "Field: examples/ardep_ecu/diagnostics_config.yaml, DID 0xF187"
echo "(VehicleManufacturerSparePartNumber, ISO 14229-1 Annex F)"
echo "data_length: 11 -> 14"
echo

cp "$CONFIG" "$WORKDIR/config_before.yaml"
# Scoped to the 0xF187 block specifically, so a stray "data_length: 11"
# elsewhere in the file is never touched by accident.
python3 - "$WORKDIR/config_before.yaml" "$WORKDIR/config_after.yaml" <<'PYEOF'
import re, sys
before_path, after_path = sys.argv[1], sys.argv[2]
text = open(before_path, encoding="utf-8").read()
pattern = re.compile(
    r'(- id:\s*"0xF187".*?data_length:\s*)11(\s*# Supplier part number)',
    re.DOTALL,
)
new_text, n = pattern.subn(r"\g<1>14\g<2>", text)
if n != 1:
    sys.exit(f"ERROR: expected exactly 1 match for the 0xF187 data_length field, found {n}")
open(after_path, "w", encoding="utf-8").write(new_text)
PYEOF

export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1700000000}"

BEFORE_OUT="$WORKDIR/before"
AFTER_OUT="$WORKDIR/after"

echo "--- Regenerating C sources + SOVD CDA (public tools/codegen.py) ---"
for variant in before after; do
  out_dir="$WORKDIR/$variant"
  cfg="$WORKDIR/config_$variant.yaml"
  python3 "$CODEGEN" \
    --config "$cfg" \
    --out "$out_dir" \
    --safety-wrappers --asil-level B --sovd --no-manifest \
    > "$WORKDIR/codegen_$variant.log" 2>&1 \
    || { echo "ERROR: codegen.py failed for $variant -- see $WORKDIR/codegen_$variant.log" >&2; cat "$WORKDIR/codegen_$variant.log" >&2; exit 1; }
done
echo "  done."
echo

if [ -r "$TESTGEN" ]; then
  echo "--- tools/testgen.py found: regenerating pytest + CAPL too (commercial tier) ---"
  for variant in before after; do
    out_dir="$WORKDIR/$variant"
    cfg="$WORKDIR/config_$variant.yaml"
    python3 "$CODEGEN" \
      --config "$cfg" \
      --out "$out_dir" \
      --safety-wrappers --asil-level B --test-gen --no-manifest \
      >> "$WORKDIR/codegen_$variant.log" 2>&1 \
      || { echo "ERROR: codegen.py --test-gen failed for $variant" >&2; exit 1; }
    python3 "$TESTGEN" \
      --config "$cfg" \
      --out "$out_dir" \
      --capl --capl-only \
      > "$WORKDIR/testgen_$variant.log" 2>&1 \
      || { echo "ERROR: testgen.py --capl failed for $variant -- see $WORKDIR/testgen_$variant.log" >&2; cat "$WORKDIR/testgen_$variant.log" >&2; exit 1; }
  done
  echo "  done."
else
  echo "--- tools/testgen.py not found: skipping pytest + CAPL regen ---"
  echo "    That half is Developer/Professional tier (see INSTALL.md). The"
  echo "    already-generated reference output for this example is committed"
  echo "    at examples/ardep_ecu/generated/tests/ if you want to see what it"
  echo "    looks like without the toolchain."
fi
echo

echo "============================================================"
echo "  The diff"
echo "============================================================"
diff -rq "$BEFORE_OUT" "$AFTER_OUT" || true
echo
echo "Every line above that differs traces back to one changed field."
echo "No other file moved."
echo
echo "To see what actually changed in any one of them, e.g.:"
echo "  diff -u $BEFORE_OUT/sovd_cda.json $AFTER_OUT/sovd_cda.json"
echo "(this workdir is deleted when the script exits -- copy out what you want first)"
