#!/bin/bash
# mnemon Hermes on_session_finalize hook.
# Queue compaction guidance for the next pre_llm_call. This preserves Hermes'
# non-blocking lifecycle while keeping memory decisions LLM-supervised.

STATE_DIR="${HERMES_HOME:-$HOME/.hermes}/mnemon"
mkdir -p "$STATE_DIR" 2>/dev/null || true

cat > "${STATE_DIR}/pending-nudge.txt" <<'EOF' 2>/dev/null || true
[mnemon] Session finalization occurred. Before relying on compressed or resumed context, preserve only critical continuity with mnemon remember when justified. Do not store transcript dumps.
EOF

printf '{}\n'
