#!/usr/bin/env bash
# Helper-script test runner. Builds a synthetic upstream, runs the helper
# inside a temp local repo, asserts behavior. No network, no GitHub, no LLM.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HELPER="$REPO_ROOT/scripts/upstream_sync.sh"
FIXTURE_BUILDER="$SCRIPT_DIR/fixtures/upstream-sync/build_fixture.sh"

TEST_TMP=$(mktemp -d)
trap "rm -rf $TEST_TMP" EXIT

FIXTURE_UPSTREAM="$TEST_TMP/upstream"
bash "$FIXTURE_BUILDER" "$FIXTURE_UPSTREAM"

LOCAL="$TEST_TMP/local"
git init -q -b master "$LOCAL"
git -C "$LOCAL" config user.email "test@local"
git -C "$LOCAL" config user.name "Local"
git -C "$LOCAL" config commit.gpgsign false
git -C "$LOCAL" commit -q --allow-empty -m "initial"

run_helper() {
  ( cd "$LOCAL" && \
    MNEMON_UPSTREAM_URL="$FIXTURE_UPSTREAM" \
    MNEMON_UPSTREAM_BRANCH=main \
    bash "$HELPER" "$@" )
}

fail() { echo "FAIL: $*" >&2; exit 1; }

test_init_creates_tracker() {
  rm -f "$LOCAL/.upstream-sync"
  run_helper init >/dev/null
  [[ -f "$LOCAL/.upstream-sync" ]] || fail "tracker not created"
  grep -qE "^upstream_url=" "$LOCAL/.upstream-sync" || fail "missing upstream_url"
  grep -qE "^last_sha=[0-9a-f]{40}$" "$LOCAL/.upstream-sync" || fail "bad last_sha"
  grep -qE "^last_sha_subject=" "$LOCAL/.upstream-sync" || fail "missing subject"
  grep -qE "^last_sha_date=" "$LOCAL/.upstream-sync" || fail "missing date"
  echo "OK: test_init_creates_tracker"
}

test_init_refuses_overwrite() {
  if run_helper init 2>/dev/null; then
    fail "init should refuse to overwrite without --force"
  fi
  echo "OK: test_init_refuses_overwrite"
}

test_init_force_overwrites() {
  run_helper init --force >/dev/null
  echo "OK: test_init_force_overwrites"
}

test_init_creates_tracker
test_init_refuses_overwrite
test_init_force_overwrites
echo "All init tests passed."

test_classify_emits_correct_classes() {
  # Re-seed tracker at the FIRST commit so all subsequent commits appear in classify.
  run_helper init --force >/dev/null
  local first_sha
  first_sha=$(git -C "$FIXTURE_UPSTREAM" rev-list --max-parents=0 HEAD)
  local tmp
  tmp=$(mktemp)
  awk -v s="$first_sha" '/^last_sha=/ {print "last_sha="s; next} {print}' \
    "$LOCAL/.upstream-sync" > "$tmp"
  mv "$tmp" "$LOCAL/.upstream-sync"

  local out
  out=$(run_helper classify)

  # Validate JSON
  echo "$out" | jq -e . >/dev/null || fail "classify output not valid JSON"

  local count
  count=$(jq '.commits | length' <<< "$out")
  [[ "$count" == "5" ]] || fail "expected 5 commits in range, got $count"

  local classes
  classes=$(jq -r '.commits[].class' <<< "$out")
  [[ "$(sed -n 1p <<< "$classes")" == "doc-meta-only" ]] || fail "commit 1 class wrong: got $(sed -n 1p <<<"$classes")"
  [[ "$(sed -n 2p <<< "$classes")" == "asset-only" ]]    || fail "commit 2 class wrong: got $(sed -n 2p <<<"$classes")"
  [[ "$(sed -n 3p <<< "$classes")" == "relevant" ]]      || fail "commit 3 class wrong: got $(sed -n 3p <<<"$classes")"
  [[ "$(sed -n 4p <<< "$classes")" == "relevant" ]]      || fail "commit 4 (mixed) class wrong: got $(sed -n 4p <<<"$classes")"
  [[ "$(sed -n 5p <<< "$classes")" == "relevant" ]]      || fail "commit 5 class wrong: got $(sed -n 5p <<<"$classes")"

  local tag5
  tag5=$(jq -r '.commits[4].tag' <<< "$out")
  [[ "$tag5" == "v0.0.1" ]] || fail "expected v0.0.1 tag on commit 5, got $tag5"

  local tag1
  tag1=$(jq -r '.commits[0].tag' <<< "$out")
  [[ "$tag1" == "null" ]] || fail "expected null tag on commit 1, got $tag1"

  local files4
  files4=$(jq -r '.commits[3].files | length' <<< "$out")
  [[ "$files4" == "2" ]] || fail "expected 2 files on commit 4 (mixed), got $files4"

  echo "OK: test_classify_emits_correct_classes"
}

test_classify_rejects_rewritten_history() {
  # Forge a tracker last_sha that is NOT in the fixture's history.
  awk '/^last_sha=/ {print "last_sha=0000000000000000000000000000000000000000"; next} {print}' \
    "$LOCAL/.upstream-sync" > "$LOCAL/.upstream-sync.tmp"
  mv "$LOCAL/.upstream-sync.tmp" "$LOCAL/.upstream-sync"

  if run_helper classify 2>/dev/null; then
    fail "classify should reject a non-ancestor last_sha"
  fi
  echo "OK: test_classify_rejects_rewritten_history"
}

test_classify_emits_correct_classes
test_classify_rejects_rewritten_history
echo "All classify tests passed."
