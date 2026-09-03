#!/usr/bin/env bash
# =============================================================================
# Xaloqi EDS
# scripts/verify_closing_keyword_syntax.sh
#
# PURPOSE: Catch invalid GitHub issue-closing references BEFORE merge,
#          mechanically -- not by an agent or human remembering to check.
#
#          GitHub's auto-close keyword parser (close(s|d)/fix(es|ed)/
#          resolve(s|d)) only recognizes two reference forms: bare `#123`
#          (same-repo) or `owner/repo#123` (cross-repo). `EDS#123` (repo
#          name, no owner, directly before `#`) matches neither -- it is
#          not malformed enough to error or warn, it just silently isn't
#          recognized as an issue reference at all. Four issues went
#          unclosed this way in one PR with zero signal anything was
#          wrong (xaloqi-knowledge lessons/run-030); the documented
#          mitigation ("remember the correct syntax") demonstrably failed
#          to prevent an immediate recurrence in the very next PR
#          (lessons/run-031's sibling note). This script is the actual
#          fix: a mechanical, unconditional gate, not a reminder.
#
# USAGE:
#   bash scripts/verify_closing_keyword_syntax.sh [base-ref]
#
#   base-ref defaults to origin/main. Checks every commit in
#   base-ref..HEAD. In CI, the checkout must have enough history to reach
#   base-ref (fetch-depth: 0, or at least deep enough to cover the PR).
#
# EXIT CODES:
#   0  No repo-prefixed closing-keyword references found.
#   1  At least one commit uses the invalid REPO#N form.
# =============================================================================

set -euo pipefail

BASE="${1:-origin/main}"

if ! git rev-parse --verify "${BASE}" >/dev/null 2>&1; then
    echo "SKIP: base ref '${BASE}' not reachable (shallow clone or first commit on this branch) -- nothing to compare against."
    exit 0
fi

RANGE="${BASE}..HEAD"
bad_found=0

# %H then a NUL-safe body would be nicer, but commit messages here are
# plain text without embedded NULs in practice; %B per-commit via a
# second git log call keeps this simple and readable.
for commit in $(git rev-list "${RANGE}"); do
    msg="$(git log -1 --format=%B "${commit}")"

    # Capture whatever comes between a closing keyword and '#N'.
    matches="$(grep -oP '(?i)\b(close[sd]?|fix(es|ed)?|resolve[sd]?)\s+\K[A-Za-z][A-Za-z0-9._/-]*#[0-9]+' <<< "${msg}" || true)"
    [ -z "${matches}" ] && continue

    while IFS= read -r m; do
        [ -z "${m}" ] && continue
        prefix="${m%%#*}"
        # Bare '#N' never matches the pattern above (it requires a
        # leading letter before '#'), so prefix is never empty here.
        # 'owner/repo#N' contains a '/' -- valid, skip. Anything else
        # (a bare repo name directly before '#') is the bug.
        if [[ "${prefix}" != */* ]]; then
            short="${commit:0:9}"
            echo "::error::Commit ${short}: '${m}' is not valid GitHub closing-keyword syntax -- '${prefix}#N' (repo name, no owner) is never recognized as an issue reference. Use bare '#N' for a same-repo issue, or 'owner/${prefix}#N' for cross-repo. See xaloqi-knowledge lessons/run-030."
            bad_found=1
        fi
    done <<< "${matches}"
done

if [ "${bad_found}" -ne 0 ]; then
    exit 1
fi

echo "OK: no repo-prefixed closing-keyword references found in ${RANGE}."
