#!/usr/bin/env bash
# Tests scripts/upstream_release_tags.sh in dry-run mode (no network, no gh).
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SCRIPT="$REPO_ROOT/scripts/upstream_release_tags.sh"

TEST_TMP=$(mktemp -d)
trap "rm -rf $TEST_TMP" EXIT

LOCAL="$TEST_TMP/local"
git init -q -b master "$LOCAL"
git -C "$LOCAL" config user.email "test@local"
git -C "$LOCAL" config user.name "Local"
git -C "$LOCAL" config commit.gpgsign false
git -C "$LOCAL" commit -q --allow-empty -m "initial"

fail() { echo "FAIL: $*" >&2; exit 1; }

UP_SHA_FOUND="abc123def4567890abc123def4567890abcdef00"
UP_SHA_MISSING="0000000000000000000000000000000000000001"

# Add a port commit carrying the trailer
( cd "$LOCAL" && \
  echo x > x.txt && git add x.txt && \
  git commit -q -m "Port: do thing

Upstream-Commit: $UP_SHA_FOUND" )

# Pending entries: one matchable, one not
cat > "$LOCAL/.upstream-tag-pending" <<EOF
# header comment
v0.0.1=$UP_SHA_FOUND
v0.0.2=$UP_SHA_MISSING
EOF

run_script() {
  ( cd "$LOCAL" && bash "$SCRIPT" "$@" )
}

test_dry_run_finds_trailer() {
  local out
  out=$(run_script --dry-run --no-release 2>&1)
  echo "$out" | grep -q "would tag v0.0.1" || fail "expected dry-run to report v0.0.1; got: $out"
  echo "$out" | grep -q "v0.0.2.*not merged yet" || fail "expected v0.0.2 to be skipped; got: $out"
  # Pending file unchanged in dry-run
  grep -qE "^v0.0.1=" "$LOCAL/.upstream-tag-pending" || fail "dry-run should not mutate pending file"
  echo "OK: test_dry_run_finds_trailer"
}

test_real_run_clears_matched_entry() {
  # Use --no-release and stub `git push` by setting GIT_PUSH_DRY_RUN=1 in script.
  # The script must consult RELEASE_TAGS_NO_PUSH env var to skip the push.
  local out
  out=$(RELEASE_TAGS_NO_PUSH=1 run_script --no-release 2>&1)
  echo "$out" | grep -q "tagging v0.0.1" || fail "expected tagging line; got: $out"
  # Locally the tag should exist
  git -C "$LOCAL" rev-parse refs/tags/v0.0.1 >/dev/null || fail "v0.0.1 tag not created locally"
  # Pending file should no longer carry v0.0.1
  if grep -qE "^v0.0.1=" "$LOCAL/.upstream-tag-pending"; then
    fail "v0.0.1 should have been removed from pending file"
  fi
  # v0.0.2 should still be there
  grep -qE "^v0.0.2=" "$LOCAL/.upstream-tag-pending" || fail "v0.0.2 should remain pending"
  # Header comment should be preserved
  grep -q "^# header comment" "$LOCAL/.upstream-tag-pending" || fail "comment was stripped"
  echo "OK: test_real_run_clears_matched_entry"
}

test_dry_run_finds_trailer
test_real_run_clears_matched_entry
echo "All release-tags tests passed."
