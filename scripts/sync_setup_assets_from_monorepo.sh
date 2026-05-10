#!/usr/bin/env bash
# Refresh setup_assets/ from github.com/mnemon-dev/mnemon (internal/setup/assets).
#
# Default: shallow clone https://github.com/mnemon-dev/mnemon.git into
#   .cache/mnemon-upstream-assets (parent .cache/ is gitignored), then rsync.
# Override: MNEMON_UPSTREAM_ASSETS=/path/to/internal/setup/assets (skip clone).
# Override: MNEMON_UPSTREAM_CLONE= directory for the shallow clone cache.
# Override: MNEMON_REPO_URL= clone URL.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MNEMON_REPO_URL="${MNEMON_REPO_URL:-https://github.com/mnemon-dev/mnemon.git}"
ASSETS_REL="internal/setup/assets"

if [[ -n "${MNEMON_UPSTREAM_ASSETS:-}" ]]; then
  UPSTREAM="$MNEMON_UPSTREAM_ASSETS"
else
  CLONE_ROOT="${MNEMON_UPSTREAM_CLONE:-$REPO_ROOT/.cache/mnemon-upstream-assets}"
  mkdir -p "$(dirname "$CLONE_ROOT")"
  if [[ ! -d "$CLONE_ROOT/.git" ]]; then
    rm -rf "$CLONE_ROOT"
    git clone --depth 1 "$MNEMON_REPO_URL" "$CLONE_ROOT"
  else
    git -C "$CLONE_ROOT" fetch --depth 1 origin
    head_branch=$(git -C "$CLONE_ROOT" symbolic-ref -q refs/remotes/origin/HEAD | sed 's|^refs/remotes/origin/||')
    if [[ -z "$head_branch" ]]; then
      head_branch=main
    fi
    git -C "$CLONE_ROOT" reset --hard "origin/${head_branch}"
  fi
  UPSTREAM="$CLONE_ROOT/$ASSETS_REL"
fi

if [[ ! -d "$UPSTREAM" ]]; then
  echo "sync_setup_assets: upstream not found at $UPSTREAM" >&2
  echo "  Set MNEMON_UPSTREAM_ASSETS or fix clone (MNEMON_REPO_URL / MNEMON_UPSTREAM_CLONE)." >&2
  exit 1
fi
UPSTREAM="$(cd "$UPSTREAM" && pwd)"
rsync -a --delete "$UPSTREAM/" "$REPO_ROOT/setup_assets/"
echo "synced $UPSTREAM/ → $REPO_ROOT/setup_assets/"
