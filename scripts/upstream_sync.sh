#!/usr/bin/env bash
# upstream_sync.sh — keep mnemon-cpp in lockstep with upstream mnemon-dev/mnemon.
# Subcommands: init, classify, advance, release-tags.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
_git_root="$(git rev-parse --show-toplevel 2>/dev/null || true)"
REPO_ROOT="${_git_root:-$(cd "$SCRIPT_DIR/.." && pwd)}"
unset _git_root

UPSTREAM_DEFAULT_URL="https://github.com/mnemon-dev/mnemon.git"
UPSTREAM_URL="${MNEMON_UPSTREAM_URL:-$UPSTREAM_DEFAULT_URL}"
UPSTREAM_BRANCH_OVERRIDE="${MNEMON_UPSTREAM_BRANCH:-}"
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
  MNEMON_UPSTREAM_BRANCH Override branch (default: auto-detect from upstream/HEAD).
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

# Upstream SHA→tag map built during fetch_upstream (avoids per-commit ls-remote calls).
_UPSTREAM_TAG_MAP=""

# Resolve the upstream branch to use. Override via MNEMON_UPSTREAM_BRANCH;
# otherwise auto-detect from upstream/HEAD (which `git remote set-head --auto`
# populates after a fetch). Fails clearly if neither is available.
resolve_upstream_branch() {
  if [[ -n "$UPSTREAM_BRANCH_OVERRIDE" ]]; then
    printf '%s\n' "$UPSTREAM_BRANCH_OVERRIDE"
    return
  fi
  git -C "$REPO_ROOT" remote set-head upstream --auto >/dev/null 2>&1 || true
  local head_ref
  head_ref=$(git -C "$REPO_ROOT" symbolic-ref --short refs/remotes/upstream/HEAD 2>/dev/null || true)
  if [[ -z "$head_ref" ]]; then
    echo "upstream-sync: unable to detect upstream's default branch." >&2
    echo "  Set MNEMON_UPSTREAM_BRANCH explicitly (e.g., MNEMON_UPSTREAM_BRANCH=master)." >&2
    exit 1
  fi
  # head_ref looks like "upstream/master" — strip the prefix.
  printf '%s\n' "${head_ref#upstream/}"
}

unshallow_if_needed() {
  if [[ "$(git -C "$REPO_ROOT" rev-parse --is-shallow-repository 2>/dev/null || echo false)" == "true" ]]; then
    git -C "$REPO_ROOT" fetch upstream --unshallow --quiet || true
  fi
}

fetch_upstream() {
  ensure_upstream_remote
  unshallow_if_needed
  git -C "$REPO_ROOT" fetch upstream --quiet
  # Build SHA→tag map without --tags (which would clobber locally-mirrored port tags).
  _UPSTREAM_TAG_MAP=$(git -C "$REPO_ROOT" ls-remote upstream 'refs/tags/*' 2>/dev/null || true)
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

  local upstream_branch
  upstream_branch=$(resolve_upstream_branch)

  local sha subject date
  sha=$(git -C "$REPO_ROOT" rev-parse "upstream/$upstream_branch")
  subject=$(git -C "$REPO_ROOT" log -1 --format=%s "$sha")
  date=$(git -C "$REPO_ROOT" log -1 --format=%cI "$sha")

  {
    printf '# Last upstream commit evaluated by /upstream-sync.\n'
    printf '# Do not hand-edit unless reseeding via `scripts/upstream_sync.sh init --force`.\n'
    printf 'upstream_url=%s\n' "$UPSTREAM_URL"
    printf 'last_sha=%s\n' "$sha"
    printf 'last_sha_subject=%s\n' "$subject"
    printf 'last_sha_date=%s\n' "$date"
  } > "$TRACKER_FILE"

  echo "init: $TRACKER_FILE seeded at $sha ($subject)"
}

cmd_classify() {
  [[ -f "$TRACKER_FILE" ]] || {
    echo "classify: $TRACKER_FILE missing. Run 'init' first." >&2
    exit 1
  }

  fetch_upstream

  local upstream_branch
  upstream_branch=$(resolve_upstream_branch)

  local last_sha
  last_sha=$(awk -F= '/^last_sha=/{print $2}' "$TRACKER_FILE")
  [[ -n "$last_sha" ]] || { echo "classify: last_sha empty in $TRACKER_FILE" >&2; exit 1; }

  if ! git -C "$REPO_ROOT" merge-base --is-ancestor "$last_sha" "upstream/$upstream_branch" 2>/dev/null; then
    echo "classify: recorded last_sha=$last_sha is not an ancestor of upstream/$upstream_branch." >&2
    echo "classify: upstream history may have been rewritten. Re-seed with 'init --force'." >&2
    exit 1
  fi

  local head_sha
  head_sha=$(git -C "$REPO_ROOT" rev-parse "upstream/$upstream_branch")

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
  tag=$(printf '%s' "$_UPSTREAM_TAG_MAP" | awk -v s="$sha" 'BEGIN{FS="\t"} $1==s{sub("refs/tags/","", $2); sub(/\^\{\}$/,"", $2); if($2!="") {print $2; exit}}')

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
  elif [[ $has_doc_meta -eq 1 ]]; then
    echo "doc-meta-only"
  else
    # Empty diff (merge commit without -m, or empty commit).
    # Force the skill to examine and decide rather than silently skipping.
    echo "relevant"
  fi
}

is_doc_meta() {
  local f="$1"
  case "$f" in
    *.md|LICENSE|.gitignore|Makefile|go.mod|go.sum) return 0 ;;
    .github/workflows/*.yml) return 0 ;;
    # Harness runtime files and docs (not part of the binary contract)
    harness/*|docs/*) return 0 ;;
    # Goreleaser, changelogs, gitignore additions (no binary impact)
    .goreleaser.yml|CHANGELOG.md|*.goreleaser.*) return 0 ;;
    # Codex eval and validation scripts (not part of mnemon binary)
    scripts/codex_app_server_eval.py|scripts/validate_harness*.sh) return 0 ;;
    *) return 1 ;;
  esac
}

cmd_advance() {
  local sha="${1:-}"
  [[ -n "$sha" ]] || { echo "advance: missing SHA arg" >&2; exit 2; }

  fetch_upstream

  local upstream_branch
  upstream_branch=$(resolve_upstream_branch)

  if ! git -C "$REPO_ROOT" merge-base --is-ancestor "$sha" "upstream/$upstream_branch" 2>/dev/null; then
    echo "advance: $sha is not in upstream/$upstream_branch history" >&2
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
