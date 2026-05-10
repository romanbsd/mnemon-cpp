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
