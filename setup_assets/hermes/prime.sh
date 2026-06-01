#!/bin/bash
# mnemon Hermes on_session_start hook.
# Hermes shell hooks return JSON. Session hooks are observational in Hermes, so
# this records lightweight state for the next pre_llm_call injection.

STATE_DIR="${HERMES_HOME:-$HOME/.hermes}/mnemon"
mkdir -p "$STATE_DIR" 2>/dev/null || true

PROMPT_DIR="${MNEMON_DATA_DIR:-$HOME/.mnemon}/prompt"
if [ ! -f "${PROMPT_DIR}/guide.md" ] && [ -f "${HOME}/.mnemon/prompt/guide.md" ]; then
  PROMPT_DIR="${HOME}/.mnemon/prompt"
fi

{
  echo "[mnemon] Memory lifecycle is active for Hermes."
  if command -v mnemon >/dev/null 2>&1; then
    STATS=$(mnemon status 2>/dev/null || true)
    if [ -n "$STATS" ]; then
      INSIGHTS=$(echo "$STATS" | sed -n 's/.*"total_insights": *\([0-9]*\).*/\1/p' | head -1)
      EDGES=$(echo "$STATS" | sed -n 's/.*"edge_count": *\([0-9]*\).*/\1/p' | head -1)
      echo "[mnemon] Memory active (${INSIGHTS:-0} insights, ${EDGES:-0} edges)."
    else
      echo "[mnemon] Memory active."
    fi
  else
    echo "[mnemon] Warning: mnemon not found in PATH."
  fi
  [ -f "${PROMPT_DIR}/guide.md" ] && cat "${PROMPT_DIR}/guide.md"
} > "${STATE_DIR}/prime-context.txt" 2>/dev/null || true

printf '{}\n'
