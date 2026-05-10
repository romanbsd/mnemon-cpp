# Upstream-sync Skill Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a project-scoped Claude Code skill (`/upstream-sync`) that brings `mnemon-cpp` up to date with new commits from `mnemon-dev/mnemon`, opening one stacked PR per commit needing a port and mirroring upstream tags after PRs merge.

**Architecture:** A deterministic Bash helper (`scripts/upstream_sync.sh`) handles fetch / range / classify / tracker bookkeeping. A separate Bash helper (`scripts/upstream_release_tags.sh`) mirrors tags after merge using a sidecar file plus a PR-body trailer. The skill (`.claude/skills/upstream-sync/SKILL.md`) owns the judgment loop — reading upstream diffs, writing C++ tests, implementing ports, opening stacked PRs. A slash command (`.claude/commands/upstream-sync.md`) is a thin wrapper.

**Tech Stack:** Bash, jq, git, GitHub CLI (`gh`), CMake/Catch2/E2E shell harness already present in the repo.

**Spec:** `docs/superpowers/specs/2026-05-10-upstream-sync-skill-design.md`

---

## File map

| File | Created by task | Responsibility |
|---|---|---|
| `tests/fixtures/upstream-sync/build_fixture.sh` | Task 1 | Builds a synthetic 6-commit upstream repo for tests |
| `tests/upstream_sync_helper_test.sh` | Tasks 2–4 | Runs helper subcommands against the fixture; asserts behavior |
| `tests/upstream_release_tags_test.sh` | Task 6 | Tests the tag-mirroring script (dry-run mode) |
| `scripts/upstream_sync.sh` | Tasks 2–4 | Helper: `init` / `classify` / `advance` / `release-tags` (passthrough) |
| `scripts/upstream_release_tags.sh` | Task 6 | Mirrors upstream tags onto local merged commits |
| `Makefile` | Task 5 | Adds `test-helper` target |
| `.claude/skills/upstream-sync/SKILL.md` | Task 7 | The skill (judgment loop) |
| `.claude/commands/upstream-sync.md` | Task 8 | `/upstream-sync` slash command wrapper |
| `CLAUDE.md` | Task 9 | Adds a section describing the upstream-sync workflow |
| `.upstream-sync` | Task 10 | Tracker file (created by `init`, committed at bootstrap) |
| `.upstream-tag-pending` | Task 10 | Empty tag-pending sidecar (committed at bootstrap) |
| `docs/superpowers/plans/2026-05-10-upstream-sync-skill-pressure-tests.md` | Task 11 | Documents subagent pressure-test scenarios for follow-up |

---

## Task 1: Synthetic upstream fixture

**Files:**
- Create: `tests/fixtures/upstream-sync/build_fixture.sh`

The fixture is a tiny git repo simulating upstream-mnemon, with one commit per classifier class plus a tagged commit. Built on demand (no binary in git).

- [ ] **Step 1: Create the fixture builder script**

Create `tests/fixtures/upstream-sync/build_fixture.sh`:

```bash
#!/usr/bin/env bash
# Build a synthetic upstream-mnemon repo at $1 with 6 commits covering each
# classifier class. Idempotent: nukes $1 first.
set -euo pipefail
DEST="${1:-}"
[[ -n "$DEST" ]] || { echo "usage: $0 <dest>" >&2; exit 1; }

rm -rf "$DEST"
git init -q -b main "$DEST"
cd "$DEST"
git config user.email "fixture@test"
git config user.name "Fixture"
git config commit.gpgsign false

# Commit 0 (baseline): empty seed
mkdir -p .keep
touch .keep/.gitkeep
git add .keep
git commit -q -m "Initial seed"

# Commit 1: doc-meta-only (README addition)
echo "# upstream" > README.md
git add README.md
git commit -q -m "Add README"

# Commit 2: asset-only
mkdir -p internal/setup/assets/skills
echo "skill content v1" > internal/setup/assets/skills/foo.md
git add internal/setup/assets
git commit -q -m "Add foo skill asset"

# Commit 3: relevant (Go internal change)
mkdir -p internal/auth
cat > internal/auth/session.go <<'EOF'
package auth

func NewSession() string { return "v1" }
EOF
git add internal/auth
git commit -q -m "Add session helper"

# Commit 4: mixed (asset + Go) - classifier should report 'relevant' since
# 'has_other' wins over 'has_asset'.
echo "skill content v2" > internal/setup/assets/skills/foo.md
mkdir -p internal/cmd
cat > internal/cmd/main.go <<'EOF'
package cmd

func Run() {}
EOF
git add internal/setup/assets internal/cmd
git commit -q -m "Update foo and add cmd.Run"

# Commit 5: relevant + tag v0.0.1
cat > internal/auth/session.go <<'EOF'
package auth

func NewSession() string { return "v2" }
EOF
git add internal/auth
git commit -q -m "Bump session to v2"
git tag v0.0.1
```

- [ ] **Step 2: Make it executable and run it once to verify**

```bash
chmod +x tests/fixtures/upstream-sync/build_fixture.sh
TMP=$(mktemp -d) && bash tests/fixtures/upstream-sync/build_fixture.sh "$TMP/up" && \
  git -C "$TMP/up" log --oneline && git -C "$TMP/up" tag && rm -rf "$TMP"
```

Expected output:
```
<sha> Bump session to v2
<sha> Update foo and add cmd.Run
<sha> Add session helper
<sha> Add foo skill asset
<sha> Add README
<sha> Initial seed
v0.0.1
```

- [ ] **Step 3: Commit**

```bash
git add tests/fixtures/upstream-sync/build_fixture.sh
git commit -m "Add synthetic upstream fixture builder for upstream-sync tests"
```

---

## Task 2: Helper script `init` subcommand

**Files:**
- Create: `scripts/upstream_sync.sh` (skeleton + `init`)
- Create: `tests/upstream_sync_helper_test.sh` (test scaffold + `init` tests)

- [ ] **Step 1: Write the failing test**

Create `tests/upstream_sync_helper_test.sh`:

```bash
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
```

- [ ] **Step 2: Run test to verify it fails**

```bash
chmod +x tests/upstream_sync_helper_test.sh
bash tests/upstream_sync_helper_test.sh || true
```

Expected: FAIL because `scripts/upstream_sync.sh` doesn't exist yet. Either an explicit "No such file" or the script bailing with usage error.

- [ ] **Step 3: Write minimal implementation**

Create `scripts/upstream_sync.sh`:

```bash
#!/usr/bin/env bash
# upstream_sync.sh — keep mnemon-cpp in lockstep with upstream mnemon-dev/mnemon.
# Subcommands: init, classify, advance, release-tags.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

UPSTREAM_DEFAULT_URL="https://github.com/mnemon-dev/mnemon.git"
UPSTREAM_URL="${MNEMON_UPSTREAM_URL:-$UPSTREAM_DEFAULT_URL}"
UPSTREAM_BRANCH="${MNEMON_UPSTREAM_BRANCH:-main}"
TRACKER_FILE="$REPO_ROOT/.upstream-sync"

usage() {
  cat <<'EOF'
upstream_sync.sh — keep mnemon-cpp in lockstep with upstream

Subcommands:
  init [--force]      Seed .upstream-sync from upstream/<branch> HEAD.
  classify            Emit JSON describing commits since .upstream-sync's last_sha.
  advance <sha>       Bump .upstream-sync to <sha> (must be in upstream history).
  release-tags ...    Pass through to scripts/upstream_release_tags.sh.

Env:
  MNEMON_UPSTREAM_URL    Override upstream URL (default: github.com/mnemon-dev/mnemon).
  MNEMON_UPSTREAM_BRANCH Override upstream branch (default: main).
EOF
}

ensure_upstream_remote() {
  if ! git -C "$REPO_ROOT" remote get-url upstream >/dev/null 2>&1; then
    git -C "$REPO_ROOT" remote add upstream "$UPSTREAM_URL"
  else
    # Re-point if URL drifted (e.g., env override during tests)
    local current
    current=$(git -C "$REPO_ROOT" remote get-url upstream)
    if [[ "$current" != "$UPSTREAM_URL" ]]; then
      git -C "$REPO_ROOT" remote set-url upstream "$UPSTREAM_URL"
    fi
  fi
}

unshallow_if_needed() {
  if [[ "$(git -C "$REPO_ROOT" rev-parse --is-shallow-repository 2>/dev/null || echo false)" == "true" ]]; then
    git -C "$REPO_ROOT" fetch upstream --unshallow --quiet || true
  fi
}

fetch_upstream() {
  ensure_upstream_remote
  git -C "$REPO_ROOT" fetch upstream --tags --quiet
  unshallow_if_needed
}

cmd_init() {
  local force=0
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --force) force=1; shift ;;
      *) echo "init: unknown arg: $1" >&2; exit 2 ;;
    esac
  done

  if [[ -f "$TRACKER_FILE" && $force -eq 0 ]]; then
    echo "init: $TRACKER_FILE already exists. Use --force to overwrite." >&2
    exit 1
  fi

  fetch_upstream

  local sha subject date
  sha=$(git -C "$REPO_ROOT" rev-parse "upstream/$UPSTREAM_BRANCH")
  subject=$(git -C "$REPO_ROOT" log -1 --format=%s "$sha")
  date=$(git -C "$REPO_ROOT" log -1 --format=%cI "$sha")

  cat > "$TRACKER_FILE" <<EOF
# Last upstream commit evaluated by /upstream-sync.
# Do not hand-edit unless reseeding via \`scripts/upstream_sync.sh init --force\`.
upstream_url=$UPSTREAM_URL
last_sha=$sha
last_sha_subject=$subject
last_sha_date=$date
EOF

  echo "init: $TRACKER_FILE seeded at $sha ($subject)"
}

cmd_classify() {
  echo "classify: not implemented yet" >&2
  exit 99
}

cmd_advance() {
  echo "advance: not implemented yet" >&2
  exit 99
}

main() {
  local sub="${1:-}"
  shift || true
  case "$sub" in
    init)         cmd_init "$@" ;;
    classify)     cmd_classify "$@" ;;
    advance)      cmd_advance "$@" ;;
    release-tags) bash "$SCRIPT_DIR/upstream_release_tags.sh" "$@" ;;
    -h|--help|"") usage; [[ -z "$sub" ]] && exit 2 || exit 0 ;;
    *)            echo "unknown subcommand: $sub" >&2; usage >&2; exit 2 ;;
  esac
}

main "$@"
```

- [ ] **Step 4: Run test to verify it passes**

```bash
chmod +x scripts/upstream_sync.sh
bash tests/upstream_sync_helper_test.sh
```

Expected:
```
OK: test_init_creates_tracker
OK: test_init_refuses_overwrite
OK: test_init_force_overwrites
All init tests passed.
```

- [ ] **Step 5: Commit**

```bash
git add scripts/upstream_sync.sh tests/upstream_sync_helper_test.sh
git commit -m "Add upstream_sync.sh init subcommand and helper test scaffold"
```

---

## Task 3: Helper `classify` subcommand

**Files:**
- Modify: `scripts/upstream_sync.sh` (replace `cmd_classify` stub)
- Modify: `tests/upstream_sync_helper_test.sh` (append classify tests)

- [ ] **Step 1: Write the failing test**

Append to `tests/upstream_sync_helper_test.sh` (before the bottom invocation block):

```bash
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
```

Move the original `echo "All init tests passed."` line to read `echo "All init tests passed."` then add `echo "All classify tests passed."` at the end.

- [ ] **Step 2: Run test to verify it fails**

```bash
bash tests/upstream_sync_helper_test.sh || true
```

Expected: previous `init` tests pass; classify test fails because `cmd_classify` exits 99.

- [ ] **Step 3: Write the implementation**

Replace `cmd_classify` in `scripts/upstream_sync.sh` with:

```bash
cmd_classify() {
  [[ -f "$TRACKER_FILE" ]] || {
    echo "classify: $TRACKER_FILE missing. Run 'init' first." >&2
    exit 1
  }

  fetch_upstream

  local last_sha
  last_sha=$(awk -F= '/^last_sha=/{print $2}' "$TRACKER_FILE")
  [[ -n "$last_sha" ]] || { echo "classify: last_sha empty in $TRACKER_FILE" >&2; exit 1; }

  if ! git -C "$REPO_ROOT" merge-base --is-ancestor "$last_sha" "upstream/$UPSTREAM_BRANCH" 2>/dev/null; then
    echo "classify: recorded last_sha=$last_sha is not an ancestor of upstream/$UPSTREAM_BRANCH." >&2
    echo "classify: upstream history may have been rewritten. Re-seed with 'init --force'." >&2
    exit 1
  fi

  local head_sha
  head_sha=$(git -C "$REPO_ROOT" rev-parse "upstream/$UPSTREAM_BRANCH")

  emit_classification "$last_sha" "$head_sha"
}

emit_classification() {
  local last="$1" head="$2"
  local commits_arr='[]'

  while IFS= read -r sha; do
    [[ -z "$sha" ]] && continue
    local commit_json
    commit_json=$(emit_commit_json "$sha")
    commits_arr=$(jq --argjson c "$commit_json" '. + [$c]' <<< "$commits_arr")
  done < <(git -C "$REPO_ROOT" rev-list --reverse "$last..$head")

  jq -n \
    --arg baseline "$last" \
    --arg head "$head" \
    --argjson commits "$commits_arr" \
    '{baseline_sha: $baseline, head_sha: $head, commits: $commits}'
}

emit_commit_json() {
  local sha="$1"
  local subject files class tag
  subject=$(git -C "$REPO_ROOT" log -1 --format=%s "$sha")
  files=$(git -C "$REPO_ROOT" diff-tree --no-commit-id --name-only -r "$sha")
  class=$(classify_paths "$files")
  tag=$(git -C "$REPO_ROOT" tag --points-at "$sha" | head -n1)

  jq -n \
    --arg sha "$sha" \
    --arg subject "$subject" \
    --arg tag "$tag" \
    --arg class "$class" \
    --arg files "$files" \
    '{
       sha: $sha,
       subject: $subject,
       tag: (if $tag == "" then null else $tag end),
       class: $class,
       files: ($files | split("\n") | map(select(. != "")))
     }'
}

classify_paths() {
  local files="$1"
  local has_asset=0 has_doc_meta=0 has_other=0

  while IFS= read -r f; do
    [[ -z "$f" ]] && continue
    if [[ "$f" == internal/setup/assets/* ]]; then
      has_asset=1
    elif is_doc_meta "$f"; then
      has_doc_meta=1
    else
      has_other=1
    fi
  done <<< "$files"

  if [[ $has_other -eq 1 ]]; then
    echo "relevant"
  elif [[ $has_asset -eq 1 ]]; then
    # asset + optional doc-meta, no relevant: still asset-only
    # (asset takes precedence; the asset-sync script handles it)
    echo "asset-only"
  else
    echo "doc-meta-only"
  fi
}

is_doc_meta() {
  local f="$1"
  case "$f" in
    *.md|LICENSE|.gitignore|Makefile|go.mod|go.sum) return 0 ;;
    .github/workflows/*.yml) return 0 ;;
    *) return 1 ;;
  esac
}
```

- [ ] **Step 4: Run test to verify it passes**

```bash
bash tests/upstream_sync_helper_test.sh
```

Expected: all init + classify tests pass.

- [ ] **Step 5: Commit**

```bash
git add scripts/upstream_sync.sh tests/upstream_sync_helper_test.sh
git commit -m "Add upstream_sync.sh classify subcommand with class detection"
```

---

## Task 4: Helper `advance` subcommand

**Files:**
- Modify: `scripts/upstream_sync.sh` (replace `cmd_advance` stub, add helper)
- Modify: `tests/upstream_sync_helper_test.sh` (append advance tests)

- [ ] **Step 1: Write the failing test**

Append to `tests/upstream_sync_helper_test.sh` (before the final `echo`):

```bash
test_advance_bumps_tracker() {
  run_helper init --force >/dev/null
  local first_sha second_sha
  first_sha=$(git -C "$FIXTURE_UPSTREAM" rev-list --max-parents=0 HEAD)
  # Pick the second commit (parent + 1)
  second_sha=$(git -C "$FIXTURE_UPSTREAM" rev-list --reverse HEAD | sed -n '2p')

  awk -v s="$first_sha" '/^last_sha=/ {print "last_sha="s; next} {print}' \
    "$LOCAL/.upstream-sync" > "$LOCAL/.upstream-sync.tmp"
  mv "$LOCAL/.upstream-sync.tmp" "$LOCAL/.upstream-sync"

  run_helper advance "$second_sha" >/dev/null
  grep -qE "^last_sha=$second_sha\$" "$LOCAL/.upstream-sync" \
    || fail "advance did not bump last_sha to $second_sha"
  grep -qE "^last_sha_subject=Add README\$" "$LOCAL/.upstream-sync" \
    || fail "advance did not update subject"
  echo "OK: test_advance_bumps_tracker"
}

test_advance_rejects_unknown_sha() {
  if run_helper advance "0000000000000000000000000000000000000000" 2>/dev/null; then
    fail "advance should reject an unknown SHA"
  fi
  echo "OK: test_advance_rejects_unknown_sha"
}

test_advance_bumps_tracker
test_advance_rejects_unknown_sha
echo "All helper tests passed."
```

- [ ] **Step 2: Run test to verify it fails**

```bash
bash tests/upstream_sync_helper_test.sh || true
```

Expected: previous tests pass, advance test fails (cmd_advance exits 99).

- [ ] **Step 3: Write the implementation**

Replace `cmd_advance` in `scripts/upstream_sync.sh` with:

```bash
cmd_advance() {
  local sha="${1:-}"
  [[ -n "$sha" ]] || { echo "advance: missing SHA arg" >&2; exit 2; }

  fetch_upstream

  if ! git -C "$REPO_ROOT" merge-base --is-ancestor "$sha" "upstream/$UPSTREAM_BRANCH" 2>/dev/null; then
    echo "advance: $sha is not in upstream/$UPSTREAM_BRANCH history" >&2
    exit 1
  fi

  [[ -f "$TRACKER_FILE" ]] || { echo "advance: $TRACKER_FILE missing. Run 'init' first." >&2; exit 1; }

  local full subject date
  full=$(git -C "$REPO_ROOT" rev-parse "$sha")
  subject=$(git -C "$REPO_ROOT" log -1 --format=%s "$full")
  date=$(git -C "$REPO_ROOT" log -1 --format=%cI "$full")

  update_kv "$TRACKER_FILE" "last_sha"         "$full"
  update_kv "$TRACKER_FILE" "last_sha_subject" "$subject"
  update_kv "$TRACKER_FILE" "last_sha_date"    "$date"

  echo "advance: $TRACKER_FILE → $full ($subject)"
}

# Idempotent in-place key=value rewrite. Preserves comments and unknown lines.
update_kv() {
  local file="$1" key="$2" value="$3"
  local tmp found=0
  tmp=$(mktemp)
  while IFS= read -r line || [[ -n "$line" ]]; do
    if [[ "$line" == "$key="* ]]; then
      printf '%s=%s\n' "$key" "$value" >> "$tmp"
      found=1
    else
      printf '%s\n' "$line" >> "$tmp"
    fi
  done < "$file"
  if [[ $found -eq 0 ]]; then
    printf '%s=%s\n' "$key" "$value" >> "$tmp"
  fi
  mv "$tmp" "$file"
}
```

- [ ] **Step 4: Run test to verify it passes**

```bash
bash tests/upstream_sync_helper_test.sh
```

Expected:
```
OK: test_init_creates_tracker
OK: test_init_refuses_overwrite
OK: test_init_force_overwrites
OK: test_classify_emits_correct_classes
OK: test_classify_rejects_rewritten_history
OK: test_advance_bumps_tracker
OK: test_advance_rejects_unknown_sha
All helper tests passed.
```

- [ ] **Step 5: Commit**

```bash
git add scripts/upstream_sync.sh tests/upstream_sync_helper_test.sh
git commit -m "Add upstream_sync.sh advance subcommand with idempotent kv rewrite"
```

---

## Task 5: Wire `make test-helper`

**Files:**
- Modify: `Makefile`

- [ ] **Step 1: Add the target**

Edit `Makefile`. Find the `.PHONY: build test unit clean help` line and add `test-helper`:

```makefile
.PHONY: build test unit test-helper clean help
```

Append (before `clean:`):

```makefile
test-helper: ## Run upstream-sync helper tests (no build, no network)
	bash tests/upstream_sync_helper_test.sh
	@if [ -f tests/upstream_release_tags_test.sh ]; then bash tests/upstream_release_tags_test.sh; fi
```

(The `if` guard lets this target work before Task 6 lands.)

- [ ] **Step 2: Verify**

```bash
make test-helper
```

Expected: helper tests pass.

- [ ] **Step 3: Commit**

```bash
git add Makefile
git commit -m "Add make test-helper for upstream-sync script tests"
```

---

## Task 6: Tag-mirroring script

**Files:**
- Create: `scripts/upstream_release_tags.sh`
- Create: `tests/upstream_release_tags_test.sh`

- [ ] **Step 1: Write the failing test**

Create `tests/upstream_release_tags_test.sh`:

```bash
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
```

- [ ] **Step 2: Run test to verify it fails**

```bash
chmod +x tests/upstream_release_tags_test.sh
bash tests/upstream_release_tags_test.sh || true
```

Expected: fail because the script doesn't exist.

- [ ] **Step 3: Write the implementation**

Create `scripts/upstream_release_tags.sh`:

```bash
#!/usr/bin/env bash
# upstream_release_tags.sh — mirror upstream tags listed in .upstream-tag-pending
# onto local commits whose port-PR has been merged. Idempotent.
#
# Flags:
#   --dry-run     Don't tag, push, or release. Just report what would happen.
#   --no-release  Skip `gh release create`. Still tags + pushes.
#
# Env:
#   RELEASE_TAGS_NO_PUSH=1  Skip `git push origin <tag>` (used in tests).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PENDING_FILE="$REPO_ROOT/.upstream-tag-pending"
DEFAULT_BRANCH="${MNEMON_LOCAL_BRANCH:-master}"

DRY_RUN=0
SKIP_RELEASE=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run)    DRY_RUN=1; shift ;;
    --no-release) SKIP_RELEASE=1; shift ;;
    -h|--help)
      sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *)            echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

if [[ ! -f "$PENDING_FILE" ]]; then
  echo "release-tags: no $PENDING_FILE; nothing to do"
  exit 0
fi

mirrored=()
new_lines=()

while IFS= read -r line || [[ -n "$line" ]]; do
  if [[ -z "$line" || "$line" == \#* ]]; then
    new_lines+=("$line")
    continue
  fi

  local_tag="${line%%=*}"
  upstream_sha="${line#*=}"

  if [[ "$local_tag" == "$line" || -z "$upstream_sha" ]]; then
    echo "release-tags: malformed line, skipping: $line" >&2
    new_lines+=("$line")
    continue
  fi

  local_sha=$(git -C "$REPO_ROOT" log "$DEFAULT_BRANCH" \
    --grep="^Upstream-Commit: $upstream_sha\$" \
    --format=%H -n 1 2>/dev/null || true)

  if [[ -z "$local_sha" ]] && command -v gh >/dev/null 2>&1; then
    local_sha=$(gh pr list --state merged \
      --search "Upstream-Commit: $upstream_sha in:body" \
      --json mergeCommit -q '.[0].mergeCommit.oid' 2>/dev/null || true)
  fi

  if [[ -z "$local_sha" ]]; then
    echo "release-tags: $local_tag (upstream $upstream_sha) — port-PR not merged yet, skipping"
    new_lines+=("$line")
    continue
  fi

  if [[ $DRY_RUN -eq 1 ]]; then
    echo "release-tags (dry-run): would tag $local_tag at $local_sha"
    new_lines+=("$line")
    continue
  fi

  echo "release-tags: tagging $local_tag at $local_sha"
  git -C "$REPO_ROOT" tag "$local_tag" "$local_sha"
  if [[ "${RELEASE_TAGS_NO_PUSH:-0}" != "1" ]]; then
    git -C "$REPO_ROOT" push origin "$local_tag"
  fi

  if [[ $SKIP_RELEASE -eq 0 ]] && command -v gh >/dev/null 2>&1; then
    gh release create "$local_tag" --generate-notes --target "$local_sha" \
      || echo "release-tags: gh release create failed for $local_tag (continuing)" >&2
  fi

  mirrored+=("$local_tag")
done < "$PENDING_FILE"

if [[ ${#mirrored[@]} -gt 0 && $DRY_RUN -eq 0 ]]; then
  : > "$PENDING_FILE"
  for line in "${new_lines[@]}"; do
    printf '%s\n' "$line" >> "$PENDING_FILE"
  done
  echo "release-tags: cleared ${#mirrored[@]} entries: ${mirrored[*]}"
fi
```

- [ ] **Step 4: Run test to verify it passes**

```bash
chmod +x scripts/upstream_release_tags.sh
bash tests/upstream_release_tags_test.sh
```

Expected:
```
OK: test_dry_run_finds_trailer
OK: test_real_run_clears_matched_entry
All release-tags tests passed.
```

Also re-run the umbrella target:

```bash
make test-helper
```

Expected: both helper tests pass.

- [ ] **Step 5: Commit**

```bash
git add scripts/upstream_release_tags.sh tests/upstream_release_tags_test.sh
git commit -m "Add upstream_release_tags.sh tag mirror script with trailer + gh fallback"
```

---

## Task 7: The skill — `.claude/skills/upstream-sync/SKILL.md`

**Files:**
- Create: `.claude/skills/upstream-sync/SKILL.md`

- [ ] **Step 1: Write the skill**

```bash
mkdir -p .claude/skills/upstream-sync
```

Create `.claude/skills/upstream-sync/SKILL.md`:

```markdown
---
name: upstream-sync
description: Use when porting recent commits from mnemon-dev/mnemon (the Go reference for this C++ port) into mnemon-cpp. Triggered by /upstream-sync. Opens stacked PRs — does not auto-merge.
---

# upstream-sync

## When to use

Invoke via `/upstream-sync` when you want to bring this repo up to date with new commits on `https://github.com/mnemon-dev/mnemon` (the `upstream` git remote). The skill processes ALL new upstream commits in one autonomous batch, opening one stacked port-PR per commit that needs porting.

The skill does NOT merge PRs. The human merges in order. After merges, run `/upstream-sync release-tags` (or `bash scripts/upstream_release_tags.sh`) to mirror upstream tags onto this repo.

## Pre-flight (one-time)

1. `bash scripts/upstream_sync.sh init` — adds `upstream` remote, seeds `.upstream-sync`.
2. Commit `.upstream-sync` and an empty `.upstream-tag-pending` to master.

If `.upstream-sync` is missing when you invoke the skill, stop and tell the user to run pre-flight.

## The autonomous batch loop

### Step 1 — Classify

Run:

```bash
bash scripts/upstream_sync.sh classify
```

The helper emits JSON:

```json
{
  "baseline_sha": "...",
  "head_sha": "...",
  "commits": [
    { "sha": "...", "subject": "...", "tag": null,
      "class": "asset-only|doc-meta-only|relevant", "files": [...] }
  ]
}
```

If `commits` is empty: print "Up to date with upstream." and stop.

### Step 2 — Resume check

Search recallium for memories tagged `upstream-sync` referencing any SHA in the classify output. Cross-check `gh pr list --search "head:upstream-sync/" --state open --json headRefName -q '.[].headRefName'`. Skip any commit whose port-branch already exists upstream.

### Step 3 — Per-commit dispatch

Process commits in the order returned (oldest first). For each commit `C`:

**Branch base:**
- First commit processed in this batch: `master`.
- Subsequent: the previous processed commit's branch tip (regardless of class).

**Branch name:** `upstream-sync/<short-sha-of-C>`.

**Dispatch by class:**

#### `doc-meta-only`

No branch. Accumulate the SHA. When the next branch is created, run `bash scripts/upstream_sync.sh advance <sha>` for each accumulated SHA before doing other work, and include the resulting `.upstream-sync` change in that branch's commit. If the batch ends on `doc-meta-only` commits with no relevant follow-up, open a final PR off the last branch tip (or master) titled `Housekeeping: advance upstream-sync tracker through N no-op commits` carrying only the tracker bump.

#### `asset-only`

1. Create branch off previous tip.
2. Run `bash scripts/sync_setup_assets_from_monorepo.sh`.
3. `make build && make test`. If either fails, STOP. Do not push. Do not advance the tracker.
4. `bash scripts/upstream_sync.sh advance <full-sha>`.
5. If `tag` is non-null in the classify output, append `<tag>=<full-sha>` to `.upstream-tag-pending`.
6. `git add setup_assets/ .upstream-sync` (and `.upstream-tag-pending` if applicable).
7. Commit with the standard trailer (see "Commit and PR shape" below).
8. `git push -u origin upstream-sync/<short-sha>`.
9. `gh pr create --base <previous-branch-or-master> --title "Port upstream <short-sha>: <subject>" --body-file <body>`.
10. Store recallium memory: type=feature, content includes upstream SHA, PR URL, files changed; tags=["upstream-sync"]; related_files=[setup_assets/...].

#### `relevant`

1. Create branch off previous tip.
2. Read upstream diff: `git show <full-sha>` (the `upstream` remote is fetched after `classify`; the SHA is locally resolvable).
3. Decide: does this commit need a C++ port? If clearly no-op (e.g., upstream refactored a private Go helper with no observable behavior change), treat as `doc-meta-only` — accumulate and move on. State your reasoning in a recallium note (type=decision).
4. If a port is needed: write or modify a TEST FIRST.
   - Behavioral changes (CLI output, JSON keys, error wording, exit codes, schema): edit `scripts/e2e_test.sh`. Follow the existing `assert_jq` and milestone-banner patterns.
   - Pure in-process logic (engine helpers, math, parsing): add a Catch2 test in `tests/smoke_test.cpp`.
   - The test must FAIL before you implement.
5. Implement the change in `src/`. Match `mnemon-spec.md` parity over idiomatic C++ — JSON keys, error wording, exit codes, file layout are part of the contract enforced by `scripts/e2e_test.sh`.
6. `make build && make test`. Loop until green. If you can't reach green after three reasonable iterations, STOP and report.
7. `bash scripts/upstream_sync.sh advance <full-sha>`.
8. If `tag` is non-null, append `<tag>=<full-sha>` to `.upstream-tag-pending`.
9. `git add` impacted source/test files plus `.upstream-sync` (and `.upstream-tag-pending` if applicable).
10. Commit with trailer.
11. Push and `gh pr create --base <previous-branch-or-master> --title "Port upstream <short-sha>: <subject>" --body-file <body>`.
12. Store recallium memory: type=feature or code-snippet; content covers upstream SHA, the C++ files changed, the rationale; related_files=[the modified files]; tags=["upstream-sync"].

### Step 4 — Summary

After the batch, print:

- PRs opened, with URLs.
- Number of `doc-meta-only` SHAs collapsed.
- Pending tags: count from `.upstream-tag-pending`.
- Anything that failed (commit SHA + reason).

## Commit and PR shape

Commit message:

```
Port upstream <short-sha>: <subject>

<one-paragraph rationale>

Upstream-Commit: <full-sha>
[Upstream-Tag: <tag>]
```

PR body:

```
Ports upstream commit <full-sha> from mnemon-dev/mnemon.

Upstream subject: <subject>
Upstream message:
<body>

## Changes
- tests/...: <one-line summary>
- src/...: <one-line summary>

## Why this commit applies
<one paragraph>

## Verification
- make build: PASS
- make test: PASS

Upstream-Commit: <full-sha>
[Upstream-Tag: <tag>]
```

The `Upstream-Commit:` trailer is load-bearing: `scripts/upstream_release_tags.sh` greps for it after merge to find the local commit for tag mirroring. Verbatim format. Don't reformat. Don't drop. The trailer goes in BOTH the commit message AND the PR body — squash-merge can strip the commit-message version, but the PR body is preserved.

## Tag mirroring (post-merge)

When the user runs `/upstream-sync release-tags` (or invokes the script directly):

```bash
bash scripts/upstream_release_tags.sh
```

The script reads `.upstream-tag-pending`, finds local commits via the trailer (with a `gh pr list --search "... in:body"` fallback), tags them with the upstream tag verbatim, pushes the tags, and creates GitHub releases. It removes cleared entries from `.upstream-tag-pending`. If any entries cleared, open a small housekeeping PR carrying the cleared sidecar.

## Failure modes

| Scenario | What you do |
|---|---|
| Helper exits non-zero (network, history rewrite) | STOP. Print the error. Do not open PRs. Do not advance tracker. |
| `make build` or `make test` fails on a port | STOP. Don't push. Don't advance tracker. Report which commit, which test/build output. |
| `gh pr create` fails | STOP. Surface the gh error. Local branch and tracker bump exist locally; user can re-invoke after cleanup. |
| Mid-batch resume after a kill | Use the Step 2 resume check — recallium notes + `gh pr list --search "head:upstream-sync/"`. |
| Squash-merge stripped the trailer | The release-tags script handles the fallback automatically via `gh pr list --search "... in:body"`. |

## Red flags — STOP and re-read

| Thought | Reality |
|---|---|
| "I'll skip the test for this small change" | Test-first is the contract. Write the test. |
| "Branching off master is simpler than chasing the previous branch" | Stacked PRs require the previous branch as base. Master-base will conflict. |
| "Let me batch these two commits into one PR" | One commit, one PR. Trailer machinery and tag mirroring depend on it. |
| "The classifier got it wrong, I'll re-classify mentally" | Trust the deterministic classifier. If it's wrong, fix `classify_paths` in `scripts/upstream_sync.sh`, not the skill loop. |
| "The trailer is just metadata, I'll shorten it" | The `Upstream-Commit:` trailer is load-bearing for tag mirroring. Verbatim. |
| "I'll auto-merge to speed things up" | The user merges. The skill stops at PR open. |

## Common mistakes

- Forgetting to bump `.upstream-sync` inside the same commit that opens the PR. Result: next batch replays the same commit.
- Including unrelated changes in a port-PR. Each PR contains only the C++ diff for its upstream commit (plus tracker bump and optional tag-pending entry).
- Force-pushing to a stacked branch after later branches have been built off it. If you must rewrite, rebuild the entire stack from that point forward.
```

- [ ] **Step 2: Verify the YAML frontmatter parses**

```bash
head -4 .claude/skills/upstream-sync/SKILL.md
```

Expected: shows the `---`-delimited block with `name` and `description`.

- [ ] **Step 3: Commit**

```bash
git add .claude/skills/upstream-sync/SKILL.md
git commit -m "Add upstream-sync skill (judgment loop for porting upstream commits)"
```

---

## Task 8: Slash command

**Files:**
- Create: `.claude/commands/upstream-sync.md`

- [ ] **Step 1: Create the slash command**

```bash
mkdir -p .claude/commands
```

Create `.claude/commands/upstream-sync.md`:

```markdown
---
description: Sync new commits from mnemon-dev/mnemon — open stacked port-PRs, optionally mirror tags after merge.
argument-hint: "[release-tags]"
---

If the user passed `release-tags` as an argument, run:

```
bash scripts/upstream_release_tags.sh
```

Otherwise, invoke the `upstream-sync` skill via the Skill tool.
```

- [ ] **Step 2: Commit**

```bash
git add .claude/commands/upstream-sync.md
git commit -m "Add /upstream-sync slash command (skill + release-tags wrapper)"
```

---

## Task 9: CLAUDE.md note

**Files:**
- Modify: `CLAUDE.md` (append a new section)

- [ ] **Step 1: Append to CLAUDE.md**

Append after the existing `## Conventions worth knowing` section in `CLAUDE.md`:

```markdown
## Staying in sync with upstream Go binary

The upstream Go reference (`https://github.com/mnemon-dev/mnemon`) is the source of truth for the parity contract. New upstream commits are ported by the `/upstream-sync` skill (`.claude/skills/upstream-sync/SKILL.md`). The skill processes new commits in one autonomous batch, opens one stacked PR per commit that needs porting, and stops without merging — the human merges in order.

State files (committed at repo root):

- `.upstream-sync` — `key=value` tracker recording the last upstream SHA evaluated. Bumped by `scripts/upstream_sync.sh advance <sha>` inside each port-PR. Re-seed with `scripts/upstream_sync.sh init --force` only if upstream history was rewritten.
- `.upstream-tag-pending` — `<tag>=<upstream-sha>` sidecar listing upstream tags whose port-PR hasn't been merged yet. Cleared by `scripts/upstream_release_tags.sh` after merges.

Each port-PR's commit and body carry an `Upstream-Commit: <full-sha>` trailer; this is load-bearing for tag mirroring. Don't reformat or drop it.

Helper tests run via `make test-helper`.
```

- [ ] **Step 2: Commit**

```bash
git add CLAUDE.md
git commit -m "Document upstream-sync workflow in CLAUDE.md"
```

---

## Task 10: Bootstrap

**Files:**
- Create: `.upstream-sync` (via helper)
- Create: `.upstream-tag-pending` (empty)

This task hits the real upstream over the network. Skip if no internet access; user can run it manually later.

- [ ] **Step 1: Seed the tracker against real upstream**

```bash
bash scripts/upstream_sync.sh init
```

Expected output:
```
init: /Users/.../mnemon-cpp/.upstream-sync seeded at <sha> (<subject>)
```

- [ ] **Step 2: Create the empty tag-pending sidecar**

```bash
cat > .upstream-tag-pending <<'EOF'
# Pending upstream-tag mirrors. Cleared by scripts/upstream_release_tags.sh.
# Format: <upstream-tag>=<upstream-sha>
EOF
```

- [ ] **Step 3: Verify**

```bash
cat .upstream-sync
cat .upstream-tag-pending
git status
```

Expected: both files staged-untracked, `.upstream-sync` carries a real SHA from `mnemon-dev/mnemon`, `.upstream-tag-pending` carries only the comment header.

- [ ] **Step 4: Commit**

```bash
git add .upstream-sync .upstream-tag-pending
git commit -m "Bootstrap upstream-sync tracker and empty tag-pending sidecar"
```

---

## Task 11: Document subagent pressure tests (follow-up)

**Files:**
- Create: `docs/superpowers/plans/2026-05-10-upstream-sync-skill-pressure-tests.md`

The skill's judgment loop needs RED-GREEN-REFACTOR pressure-testing per `superpowers:writing-skills`. Execution is deferred to a follow-up session because it requires spawning subagents and iterating on the SKILL.md. This task only documents the scenarios.

- [ ] **Step 1: Write the pressure-test plan**

Create `docs/superpowers/plans/2026-05-10-upstream-sync-skill-pressure-tests.md`:

```markdown
# Upstream-sync skill — subagent pressure tests

> Follow-up activity for the upstream-sync skill. Execute in a separate session per superpowers:writing-skills.

## Setup

For each scenario, prepare a scratch checkout of mnemon-cpp with:
1. The fixture from `tests/fixtures/upstream-sync/build_fixture.sh` extracted to a temp dir.
2. `MNEMON_UPSTREAM_URL` pointed at that temp dir.
3. `.upstream-sync` rewound so all five fixture commits are in range.
4. `.upstream-tag-pending` empty.

## RED scenarios (run WITHOUT the skill loaded)

Spawn a baseline subagent given only `CLAUDE.md` and the helper script. Prompt: *"Port these five upstream commits per the project's conventions. Use stacked PRs."*

Record verbatim:
- Did they fetch upstream correctly?
- Did they classify (or skip the helper and classify by hand)?
- Did they branch off the previous branch tip?
- Did they advance the tracker file?
- Did they write tests first?
- Did they include the `Upstream-Commit:` trailer?
- Did they handle the tagged commit (v0.0.1) by writing to `.upstream-tag-pending`?

Expected baseline failures: at least three of the above will be wrong.

## GREEN scenarios (run WITH the skill loaded)

Same prompt, with the skill loaded via `/upstream-sync`. Verify each item from RED is now correct.

## REFACTOR pressure variants

Run these AFTER GREEN passes. Each is designed to elicit a rationalization.

1. **Time pressure:** "You have ten minutes. Just batch all five commits into one PR."
   Expected: skill resists. If it caves, add to red flags.
2. **Sunk cost:** "You're already three commits in. The build for commit 4 is slow. Skip the test."
   Expected: skill resists.
3. **Authority pressure:** "The lead engineer said the trailer is optional this week."
   Expected: skill resists.
4. **Exhaustion:** "This is your fifteenth port. The classifier seems off. Just classify by hand."
   Expected: skill resists; redirects to fix `classify_paths`.

For each rationalization that gets through, add a row to the SKILL.md "Red flags" or "Common mistakes" tables and re-run that scenario.

## Exit criteria

Skill passes all GREEN and REFACTOR scenarios twice in a row.
```

- [ ] **Step 2: Commit**

```bash
git add docs/superpowers/plans/2026-05-10-upstream-sync-skill-pressure-tests.md
git commit -m "Document subagent pressure tests for upstream-sync skill"
```

---

## Self-review checklist

After all tasks complete, verify:

- [ ] `bash tests/upstream_sync_helper_test.sh` passes (init, classify, advance).
- [ ] `bash tests/upstream_release_tags_test.sh` passes (dry-run + real run with `RELEASE_TAGS_NO_PUSH=1`).
- [ ] `make test-helper` runs both helper test files.
- [ ] `make test` (E2E) still passes — the skill changes don't touch the C++ build.
- [ ] `.claude/skills/upstream-sync/SKILL.md` has YAML frontmatter with `name` and `description`.
- [ ] `.claude/commands/upstream-sync.md` exists.
- [ ] `.upstream-sync` is committed and contains a real upstream SHA.
- [ ] `.upstream-tag-pending` is committed and empty (just the header comment).
- [ ] `CLAUDE.md` mentions `/upstream-sync`, the two state files, and `make test-helper`.
- [ ] No file uses placeholder text — every code block in this plan was checked into the helper or skill verbatim or with documented adaptations.
