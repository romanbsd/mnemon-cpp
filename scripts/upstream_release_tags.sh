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
_git_root="$(git rev-parse --show-toplevel 2>/dev/null || true)"
REPO_ROOT="${_git_root:-$(cd "$SCRIPT_DIR/.." && pwd)}"
unset _git_root
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

  # Primary: find a port commit on master with an exact Upstream-Commit trailer match.
  port_sha=$(git -C "$REPO_ROOT" log "$DEFAULT_BRANCH" \
    --grep="^Upstream-Commit: $upstream_sha\$" \
    --format=%H -n 1 2>/dev/null || true)

  # Ancestor fallback: upstream version tags often point to release-notes commits
  # that are never ported. Find the most recent master commit whose Upstream-Commit
  # trailer refers to a commit that is an ancestor of (or equal to) the upstream
  # release commit — i.e., the last port commit included in that version.
  if [[ -z "$port_sha" ]] && \
     git -C "$REPO_ROOT" cat-file -e "${upstream_sha}^{commit}" 2>/dev/null; then
    while IFS= read -r candidate; do
      us=$(git -C "$REPO_ROOT" log -1 --format="%B" "$candidate" \
        | sed -n 's/^Upstream-Commit: //p' | head -1)
      if [[ -n "$us" ]] && \
         git -C "$REPO_ROOT" merge-base --is-ancestor "$us" "$upstream_sha" 2>/dev/null; then
        port_sha="$candidate"
        break
      fi
    done < <(git -C "$REPO_ROOT" log "$DEFAULT_BRANCH" \
      --grep="Upstream-Commit:" --format="%H" 2>/dev/null)
  fi

  # Prefer the merge commit on the default branch that contains the port commit,
  # so the tag lands on a commit that is a direct parent in master's first-parent chain.
  local_sha=""
  if [[ -n "$port_sha" ]]; then
    merge_sha=$(git -C "$REPO_ROOT" log "$DEFAULT_BRANCH" --ancestry-path \
      --merges --format="%H" "${port_sha}..${DEFAULT_BRANCH}" 2>/dev/null \
      | tail -1 || true)
    local_sha="${merge_sha:-$port_sha}"
  fi

  if [[ -z "$local_sha" ]] && command -v gh >/dev/null 2>&1; then
    local_sha=$(gh pr list --state merged \
      --search "Upstream-Commit: $upstream_sha in:body" \
      --json mergeCommit -q '.[0].mergeCommit.oid // empty' 2>/dev/null || true)
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

  existing_sha=""
  if git -C "$REPO_ROOT" rev-parse "refs/tags/$local_tag" >/dev/null 2>&1; then
    existing_sha=$(git -C "$REPO_ROOT" rev-parse "refs/tags/$local_tag")
  fi

  if [[ -n "$existing_sha" && "$existing_sha" == "$local_sha" ]]; then
    echo "release-tags: $local_tag already correctly tagged at $local_sha"
  elif [[ -n "$existing_sha" ]]; then
    echo "release-tags: $local_tag exists at wrong commit ($existing_sha), moving to $local_sha"
    git -C "$REPO_ROOT" tag -f "$local_tag" "$local_sha"
  else
    echo "release-tags: tagging $local_tag at $local_sha"
    git -C "$REPO_ROOT" tag "$local_tag" "$local_sha"
  fi
  if [[ "${RELEASE_TAGS_NO_PUSH:-0}" != "1" ]]; then
    push_flags=""
    [[ -n "$existing_sha" && "$existing_sha" != "$local_sha" ]] && push_flags="--force"
    if ! git -C "$REPO_ROOT" push $push_flags origin "$local_tag"; then
      echo "release-tags: push failed for $local_tag (will retry on next run)" >&2
    fi
  fi

  if [[ $SKIP_RELEASE -eq 0 ]] && command -v gh >/dev/null 2>&1; then
    # Delete any existing release that points to the wrong commit before re-creating.
    if [[ -n "$existing_sha" && "$existing_sha" != "$local_sha" ]]; then
      gh release delete "$local_tag" --yes 2>/dev/null || true
    fi
    gh release create "$local_tag" --generate-notes --target "$local_sha" \
      || echo "release-tags: gh release create failed for $local_tag (continuing)" >&2
  fi

  mirrored+=("$local_tag")
done < "$PENDING_FILE"

if [[ ${#mirrored[@]} -gt 0 && $DRY_RUN -eq 0 ]]; then
  : > "$PENDING_FILE"
  for line in ${new_lines[@]+"${new_lines[@]}"}; do
    printf '%s\n' "$line" >> "$PENDING_FILE"
  done
  echo "release-tags: cleared ${#mirrored[@]} entries: ${mirrored[*]}"
fi
