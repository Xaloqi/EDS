#!/usr/bin/env bash
# =============================================================================
# Xaloqi EDS
# scripts/verify_closing_keyword_syntax.sh
#
# PURPOSE: Catch invalid/unreliable GitHub issue-closing references BEFORE
#          merge, mechanically -- not by an agent or human remembering to
#          check. Checks both commit messages AND the PR body/description
#          (GitHub parses closing keywords in both places independently --
#          the second recurrence of this bug class happened purely in a PR
#          body with clean commit messages, see below).
#
#          Two independent, unrelated bad patterns, both already bitten us
#          for real:
#
#          1. REPO#N instead of #N or owner/repo#N (xaloqi-knowledge
#             lessons/run-030). GitHub's auto-close parser only recognizes
#             bare `#123` (same-repo) or `owner/repo#123` (cross-repo).
#             `EDS#123` (repo name, no owner, directly before `#`) matches
#             neither -- not malformed enough to error or warn, it just
#             silently isn't recognized as an issue reference at all.
#
#          2. "Fixes #1, #2, #3" -- a comma-separated list of issue
#             references after a SINGLE closing keyword (xaloqi-knowledge
#             lessons/run-024). GitHub only reliably auto-closes the
#             first-listed issue in a comma-list; the rest silently stay
#             open. This bit us in a PR *body* (not a commit message) the
#             same day the repo-prefix check above shipped -- proof this
#             needed to cover PR bodies too, not just commits.
#
#          Fix for #2: repeat the keyword per issue instead of listing
#          them together, e.g. "Fixes #1. Fixes #2. Fixes #3." rather than
#          "Fixes #1, #2, #3."
#
# USAGE:
#   bash scripts/verify_closing_keyword_syntax.sh [base-ref]
#
#   base-ref defaults to origin/main. Checks every commit message in
#   base-ref..HEAD, plus $PR_BODY (if set -- CI passes
#   github.event.pull_request.body here; nothing to check locally outside
#   a PR context, that's fine). In CI, the checkout must have enough
#   history to reach base-ref (fetch-depth: 0, or at least deep enough to
#   cover the PR).
#
# EXIT CODES:
#   0  No bad closing-keyword references found.
#   1  At least one bad reference found (either pattern).
# =============================================================================

set -euo pipefail

BASE="${1:-origin/main}"
bad_found=0

# --- Pattern 1: repo-prefixed reference (run-030) --------------------------
check_repo_prefix() {
    local where="$1" text="$2"
    local matches
    matches="$(grep -oP '(?i)\b(close[sd]?|fix(es|ed)?|resolve[sd]?)\s+\K[A-Za-z][A-Za-z0-9._/-]*#[0-9]+' <<< "${text}" || true)"
    [ -z "${matches}" ] && return 0

    while IFS= read -r m; do
        [ -z "${m}" ] && continue
        local prefix="${m%%#*}"
        # Bare '#N' never matches the pattern above (it requires a leading
        # letter before '#'), so prefix is never empty here. 'owner/repo#N'
        # contains a '/' -- valid, skip. Anything else (a bare repo name
        # directly before '#') is the bug.
        if [[ "${prefix}" != */* ]]; then
            echo "::error::${where}: '${m}' is not valid GitHub closing-keyword syntax -- '${prefix}#N' (repo name, no owner) is never recognized as an issue reference. Use bare '#N' for a same-repo issue, or 'owner/${prefix}#N' for cross-repo. See xaloqi-knowledge lessons/run-030."
            bad_found=1
        fi
    done <<< "${matches}"
}

# --- Pattern 2: comma-list after one keyword (run-024) ----------------------
check_comma_list() {
    local where="$1" text="$2"
    local matches
    matches="$(grep -ozP '(?i)\b(close[sd]?|fix(es|ed)?|resolve[sd]?)\s+([A-Za-z][A-Za-z0-9._/-]*)?#[0-9]+(\s*,\s*([A-Za-z][A-Za-z0-9._/-]*)?#[0-9]+)+' <<< "${text}" | tr '\0' '\n' || true)"
    [ -z "${matches}" ] && return 0

    while IFS= read -r m; do
        [ -z "${m}" ] && continue
        echo "::error::${where}: '${m}' lists multiple issues after one closing keyword -- GitHub only reliably auto-closes the FIRST one listed, the rest silently stay open. Repeat the keyword instead, e.g. 'Fixes #1. Fixes #2.' not 'Fixes #1, #2.'. See xaloqi-knowledge lessons/run-024."
        bad_found=1
    done <<< "${matches}"
}

check_all() {
    local where="$1" text="$2"
    check_repo_prefix "${where}" "${text}"
    check_comma_list "${where}" "${text}"
}

# --- Commit messages ---------------------------------------------------------
if git rev-parse --verify "${BASE}" >/dev/null 2>&1; then
    for commit in $(git rev-list "${BASE}..HEAD"); do
        msg="$(git log -1 --format=%B "${commit}")"
        check_all "Commit ${commit:0:9}" "${msg}"
    done
else
    echo "SKIP (commits): base ref '${BASE}' not reachable (shallow clone or first commit on this branch) -- nothing to compare against."
fi

# --- PR body (CI passes this via $PR_BODY; no-op locally) -------------------
if [ -n "${PR_BODY:-}" ]; then
    check_all "PR description" "${PR_BODY}"
fi

if [ "${bad_found}" -ne 0 ]; then
    exit 1
fi

echo "OK: no bad closing-keyword references found."
