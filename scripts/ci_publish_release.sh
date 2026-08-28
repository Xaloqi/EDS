#!/usr/bin/env bash
# scripts/ci_publish_release.sh — create the public GitHub Release for a
# tag, idempotently, with notes drawn from CHANGELOG.md. Called by
# .github/workflows/release.yml on every v* tag push. See that workflow's
# header comment for why this exists (O-10, xaloqi-knowledge
# reviews/FINDINGS.md) and what it deliberately does NOT automate.
#
# Usage: ci_publish_release.sh vX.Y.Z
# Requires GH_TOKEN in the environment (contents: write on this repo).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TAG="${1:?Usage: ci_publish_release.sh vX.Y.Z}"
VERSION="${TAG#v}"
GH_REPO="Xaloqi/EDS"

if gh release view "$TAG" --repo "$GH_REPO" >/dev/null 2>&1; then
  echo "Release $TAG already exists — skipping creation."
else
  notes_file="$(mktemp)"
  python3 "$REPO_ROOT/scripts/extract_changelog_section.py" \
    "$REPO_ROOT/CHANGELOG.md" "$VERSION" > "$notes_file"
  {
    echo
    echo "---"
    echo
    echo "**Full changelog:** [CHANGELOG.md](https://github.com/${GH_REPO}/blob/main/CHANGELOG.md)"
  } >> "$notes_file"

  gh release create "$TAG" \
    --repo "$GH_REPO" \
    --title "EDS $TAG" \
    --notes-file "$notes_file" \
    --latest

  echo "Created Release $TAG."
fi

# Idempotent safety net: a Release can exist without being Latest (e.g.
# --latest was omitted on manual creation once, or an older tag got
# re-marked latest by mistake) — this is exactly the O-10 failure shape,
# just checked here instead of by a separate script run after the fact.
latest="$(gh api "repos/${GH_REPO}/releases/latest" --jq .tag_name)"
if [ "$latest" != "$TAG" ]; then
  echo "Release $TAG exists but repo Latest is $latest — fixing."
  gh release edit "$TAG" --repo "$GH_REPO" --latest
else
  echo "OK: $TAG is the repo's Latest release."
fi
