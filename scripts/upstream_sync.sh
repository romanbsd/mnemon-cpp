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
  unshallow_if_needed
  git -C "$REPO_ROOT" fetch upstream --tags --quiet
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
