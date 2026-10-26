#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Tracking issue for a failing weekly run against Zephyr main: ONE, stable title.
#   tracking-issue.sh <pass|fail> <run-url> <dir with sha.txt, sdk.txt, first-failure.txt, tail.txt>
# fail: open it, or comment on the open one. pass: comment with the SHA and close it; with nothing
# open, do nothing. DRY_RUN=1: look the issue up (read-only) and print what would be done.
# TITLE_PREFIX is for tests only.
set -euo pipefail
RESULT=$1; URL=$2; DIR=$3
TITLE="${TITLE_PREFIX:-}EDS breaks against Zephyr main"
DRY=${DRY_RUN:-0}
SHA=$(cat "$DIR/sha.txt" 2>/dev/null || echo unknown)
SDK=$(cat "$DIR/sdk.txt" 2>/dev/null || echo unknown)
NUM=$(gh issue list --state open --limit 200 --json number,title \
      --jq "[.[] | select(.title == \"$TITLE\")] | sort_by(.number) | .[0].number // empty")
act() { if [ "$DRY" = 1 ]; then echo "DRY RUN would run: gh $*"; else gh "$@"; fi; }

if [ "$RESULT" = fail ]; then
  BODY=$(printf 'The weekly build against Zephyr `main` (warnings as errors) failed.\n\nZephyr: `%s`, SDK `%s`\nRun: %s\n\nFirst failure:\n```\n%s\n```\n\n<details><summary>Last lines of that log</summary>\n\n```\n%s\n```\n</details>\n\nReport-only: `ci.yml` (Zephyr 3.7) is the gate and is not affected. Build logs and each build'"'"'s .config are attached to the run for 14 days.' \
        "$SHA" "$SDK" "$URL" "$(cat "$DIR/first-failure.txt" 2>/dev/null || echo '(none captured)')" "$(cat "$DIR/tail.txt" 2>/dev/null || echo '(no log captured)')")
  if [ -n "$NUM" ]; then echo "open tracking issue #$NUM exists: commenting"; act issue comment "$NUM" --body "$BODY"
  else echo "no open tracking issue titled '$TITLE': opening one"; act issue create --title "$TITLE" --body "$BODY"; fi
else
  if [ -n "$NUM" ]; then
    echo "run passes and issue #$NUM is open: commenting and closing"
    act issue comment "$NUM" --body "Builds again against Zephyr \`main\` at \`$SHA\` (SDK \`$SDK\`): $URL"
    act issue close "$NUM"
  else echo "run passes, no open tracking issue: nothing to do"; fi
fi
