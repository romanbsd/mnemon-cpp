#!/bin/bash
# mnemon Cursor sessionStart hook.
# Cursor expects JSON on stdout; additional_context is added to initial context.

PROMPT_DIR="${MNEMON_DATA_DIR:-$HOME/.mnemon}/prompt"
if [ ! -f "${PROMPT_DIR}/guide.md" ] && [ -f "${HOME}/.mnemon/prompt/guide.md" ]; then
  PROMPT_DIR="${HOME}/.mnemon/prompt"
fi

resolve_mnemon() {
  if [ -n "${MNEMON_BIN:-}" ] && [ -x "${MNEMON_BIN}" ]; then
    printf "%s" "${MNEMON_BIN}"
    return 0
  fi
  if command -v mnemon >/dev/null 2>&1; then
    command -v mnemon
    return 0
  fi
  if [ -x "${HOME}/go/bin/mnemon" ]; then
    printf "%s" "${HOME}/go/bin/mnemon"
    return 0
  fi
  return 1
}

{
  if MNEMON="$(resolve_mnemon)"; then
    STATS="$("${MNEMON}" status 2>/dev/null || true)"
    if [ -n "${STATS}" ]; then
      INSIGHTS=$(echo "${STATS}" | sed -n 's/.*"total_insights": *\([0-9]*\).*/\1/p' | head -1)
      EDGES=$(echo "${STATS}" | sed -n 's/.*"edge_count": *\([0-9]*\).*/\1/p' | head -1)
      echo "[mnemon] Memory active (${INSIGHTS:-0} insights, ${EDGES:-0} edges)."
    else
      echo "[mnemon] Memory active."
    fi
  else
    echo "[mnemon] Warning: mnemon not found in PATH. Set MNEMON_BIN or add mnemon to PATH."
  fi

  if [ -f "${PROMPT_DIR}/guide.md" ]; then
    cat "${PROMPT_DIR}/guide.md"
  fi
} | python3 -c 'import json, sys; text = sys.stdin.read().strip(); print(json.dumps({"additional_context": text} if text else {}))'
