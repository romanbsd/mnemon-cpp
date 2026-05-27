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

test_idempotent_rerun_with_existing_tag() {
  # Re-run the script after a prior successful run: the tag exists locally,
  # and the matched entry is no longer in pending. The script should be a no-op,
  # not crash on a duplicate-tag attempt.
  local out
  out=$(RELEASE_TAGS_NO_PUSH=1 run_script --no-release 2>&1)
  # No mirrored entries this time (v0.0.1 already gone from pending, v0.0.2 still unmerged)
  echo "$out" | grep -q "v0.0.2.*not merged yet" || fail "expected v0.0.2 still skipped; got: $out"
  echo "$out" | grep -q "tagging v0.0.1" && fail "should NOT re-tag v0.0.1; got: $out"
  echo "OK: test_idempotent_rerun_with_existing_tag"
}

test_push_failure_does_not_abort_sidecar_rewrite() {
  # Re-prime pending with a new entry that has a matching trailer
  local UP_SHA="cafebabe1234567890cafebabe1234567890cafe"
  ( cd "$LOCAL" && \
    echo y > y.txt && git add y.txt && \
    git commit -q -m "Port: another thing

Upstream-Commit: $UP_SHA" )
  cat > "$LOCAL/.upstream-tag-pending" <<EOF
v0.0.3=$UP_SHA
EOF
  # Force `git push` to fail by pointing origin at a bogus URL.
  ( cd "$LOCAL" && git remote add origin "/nonexistent/path-$$.git" 2>/dev/null || git remote set-url origin "/nonexistent/path-$$.git" )
  local out
  out=$( run_script --no-release 2>&1 || true )
  # The tag should still be created locally
  git -C "$LOCAL" rev-parse refs/tags/v0.0.3 >/dev/null || fail "v0.0.3 should be created locally even when push fails"
  # The sidecar should still be rewritten (entry removed)
  if grep -qE "^v0.0.3=" "$LOCAL/.upstream-tag-pending"; then
    fail "v0.0.3 should be removed from pending despite push failure"
  fi
  echo "OK: test_push_failure_does_not_abort_sidecar_rewrite"
}

test_ancestor_fallback_for_unported_release_commit() {
  # Simulates the common case: upstream tag points to a release-notes commit
  # that was never ported. The script must find the most recent master commit
  # whose Upstream-Commit trailer is an ancestor of the release commit.
  local T="$TEST_TMP/ancestor_test"
  mkdir -p "$T"

  local UP="$T/upstream"
  git init -q -b master "$UP"
  git -C "$UP" config user.email "up@test"
  git -C "$UP" config user.name "Up"
  git -C "$UP" config commit.gpgsign false
  git -C "$UP" commit -q --allow-empty -m "feat: the real work"
  local UP_FEAT_SHA; UP_FEAT_SHA=$(git -C "$UP" rev-parse HEAD)
  git -C "$UP" commit -q --allow-empty -m "chore(release): prepare v0.0.9 notes"
  local UP_RELEASE_SHA; UP_RELEASE_SHA=$(git -C "$UP" rev-parse HEAD)

  local LOC="$T/local"
  git init -q -b master "$LOC"
  git -C "$LOC" config user.email "lo@test"
  git -C "$LOC" config user.name "Local"
  git -C "$LOC" config commit.gpgsign false
  git -C "$LOC" commit -q --allow-empty -m "initial"
  git -C "$LOC" remote add upstream "$UP"
  git -C "$LOC" fetch -q upstream

  git -C "$LOC" commit -q --allow-empty -m "Port feat: the real work

Upstream-Commit: $UP_FEAT_SHA"
  local PORT_SHA; PORT_SHA=$(git -C "$LOC" rev-parse HEAD)

  cat > "$LOC/.upstream-tag-pending" <<EOF
v0.0.9=$UP_RELEASE_SHA
EOF

  local out
  out=$(cd "$LOC" && RELEASE_TAGS_NO_PUSH=1 bash "$SCRIPT" --no-release 2>&1)

  git -C "$LOC" rev-parse refs/tags/v0.0.9 >/dev/null \
    || fail "ancestor-fallback: v0.0.9 not tagged; got: $out"
  local tag_sha; tag_sha=$(git -C "$LOC" rev-parse refs/tags/v0.0.9)
  [[ "$tag_sha" == "$PORT_SHA" ]] \
    || fail "ancestor-fallback: tag at $tag_sha, want port commit $PORT_SHA"
  grep -qE "^v0.0.9=" "$LOC/.upstream-tag-pending" \
    && fail "ancestor-fallback: pending entry should be cleared"
  echo "OK: test_ancestor_fallback_for_unported_release_commit"
}

test_dry_run_finds_trailer
test_real_run_clears_matched_entry
test_idempotent_rerun_with_existing_tag
test_push_failure_does_not_abort_sidecar_rewrite
test_ancestor_fallback_for_unported_release_commit
echo "All release-tags tests passed."
