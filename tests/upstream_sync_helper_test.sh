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
