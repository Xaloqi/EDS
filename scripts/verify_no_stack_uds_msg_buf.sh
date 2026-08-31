#!/usr/bin/env bash
# =============================================================================
# Xaloqi EDS
# scripts/verify_no_stack_uds_msg_buf.sh
#
# PURPOSE: CI gate for issue #152, part 2.
#
#          core/uds_types.h's EDS_MSG_BUF_MAX_STACK_BYTES _Static_assert
#          documents its own limitation: it only checks sizeof(uds_msg_buf_t)
#          against a configured threshold, which is necessary but NOT
#          sufficient to catch a stack-allocated uds_msg_buf_t (4097 bytes by
#          default) — an integrator who raises the threshold (e.g. to 8192 for
#          a host build) gets zero protection from that check alone. This
#          script is the "real check" the comment says is otherwise "a
#          code-review... concern": a grep-based scan that flags any
#          function-scope (stack) declaration of uds_msg_buf_t.
#
# SCOPE:   *.c files under core/ transport/ platform/ config/ (the runtime
#          stack — see CONTRIBUTING.md's dual-license table). Header files are
#          excluded deliberately: in this codebase uds_msg_buf_t only ever
#          appears in headers as a struct member, a function parameter/return
#          type, or a documentation-comment example — never as an executable
#          declaration — so scoping to *.c is what "exclude header
#          declarations" (issue #152) means in practice here.
#
# WHAT IS FLAGGED: a bare "uds_msg_buf_t <name>;" declaration — the shape a
#          stack (automatic-storage) local variable takes. This excludes,
#          by construction of the pattern:
#            - pointers/parameters ("uds_msg_buf_t *req", "const uds_msg_buf_t *req")
#            - function prototypes (they don't end the identifier in ';' alone
#              on style used across this codebase, and are excluded by the
#              'static' function-local exclusion below where relevant)
#          and, by explicit filtering:
#            - comment lines (documentation examples, e.g. uds_periodic.h)
#            - anything declared 'static' — a static local has STATIC storage
#              duration (it lives in .bss/.data, not on the stack) whether it
#              is declared at file scope or inside a function body, so it is
#              exactly the safe pattern this codebase uses everywhere
#              (see core/uds_safety.c, platform/freertos/freertos_platform_api.c).
#
# LIMITATION: this is intentionally grep-based, not a real parser (per issue
#          #152's own guidance — "keep it simple"). It cannot see across
#          preprocessor conditionals or macro-generated declarations. It is a
#          necessary-but-not-sufficient gate, same as the _Static_assert it
#          complements — not a replacement for code review.
#
# USAGE:
#   bash scripts/verify_no_stack_uds_msg_buf.sh
#
# EXIT CODES:
#   0  No stack-scoped uds_msg_buf_t declarations found.
#   1  One or more were found (or an unrecognised non-static, non-comment
#      match was found that isn't in the KNOWN_SAFE allowlist below and
#      needs a human to classify it).
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${ROOT}"

# ---------------------------------------------------------------------------
# Known-safe non-static occurrences, as "file:content" pairs — currently
# empty because every non-static match in *.c today is a false positive this
# script already filters (comments/static). If a future legitimate case can't
# be expressed by those filters (e.g. a struct member declared inside a .c
# file), add an exact "path/to/file.c:<trimmed line text>" entry here with a
# comment explaining why it's safe, rather than loosening the pattern.
# ---------------------------------------------------------------------------
KNOWN_SAFE=(
)

SCAN_DIRS=(core transport platform config)
PATTERN='uds_msg_buf_t[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*;'

echo ""
echo "================================================================"
echo "  Xaloqi EDS — uds_msg_buf_t stack-allocation guard (issue #152)"
echo "================================================================"
echo ""
echo "Scanning *.c under: ${SCAN_DIRS[*]}"
echo "Pattern: ${PATTERN}"
echo ""

matches="$(grep -rnE "${PATTERN}" --include='*.c' "${SCAN_DIRS[@]}" 2>/dev/null || true)"

fail=0
checked=0

while IFS= read -r m; do
    [ -z "${m}" ] && continue
    checked=$((checked + 1))

    file="${m%%:*}"
    rest="${m#*:}"
    content="${rest#*:}"

    trimmed="$(printf '%s' "${content}" | sed -e 's/^[[:space:]]*//')"

    # Skip comment lines (doc examples such as core/uds_periodic.h's sample
    # "static uds_msg_buf_t s_periodic_frame;" inside a /** ... */ block).
    case "${trimmed}" in
        '*'*|'//'*|'/*'*)
            continue
            ;;
    esac

    # Skip anything with static storage duration — file-scope or
    # function-local 'static' is not stack allocation.
    case "${trimmed}" in
        static*)
            continue
            ;;
    esac

    # Anything left is a non-static, non-comment "uds_msg_buf_t <name>;" —
    # exactly the shape of a stack (automatic storage) declaration. Allow it
    # only if explicitly allowlisted as a known-safe case.
    allowed=0
    for safe in "${KNOWN_SAFE[@]:-}"; do
        [ -z "${safe}" ] && continue
        if [ "${file}:${trimmed}" = "${safe}" ]; then
            allowed=1
            break
        fi
    done

    if [ "${allowed}" -eq 1 ]; then
        continue
    fi

    echo "FAIL: possible stack-scoped uds_msg_buf_t declaration"
    echo "      ${m}"
    fail=1
done <<< "${matches}"

echo "Checked ${checked} raw match(es)."
echo ""

if [ "${fail}" -ne 0 ]; then
    echo "================================================================"
    echo "  FAIL: stack-allocation guard found a non-static, non-comment"
    echo "  'uds_msg_buf_t <name>;' declaration outside the known-safe set."
    echo ""
    echo "  uds_msg_buf_t is UDS_MAX_PAYLOAD_LEN + 2 bytes (4097 by default)."
    echo "  Allocating it on a task's stack risks silent stack corruption or"
    echo "  an MPU fault. Use a 'static' (file-scope or function-local)"
    echo "  declaration instead — see core/uds_safety.c or"
    echo "  platform/freertos/freertos_platform_api.c for the pattern, and"
    echo "  core/uds_types.h's EDS_MSG_BUF_MAX_STACK_BYTES comment for the"
    echo "  full rationale."
    echo "================================================================"
    exit 1
fi

echo "PASS: no stack-scoped uds_msg_buf_t declarations found."
exit 0
