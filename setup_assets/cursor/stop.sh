#!/bin/bash
# mnemon Cursor stop hook.
# Cursor expects JSON on stdout; followup_message is submitted as a follow-up user message.

INPUT=$(cat)
python3 - "$INPUT" <<'PY'
import json
import sys

try:
    payload = json.loads(sys.argv[1])
except Exception:
    payload = {}

last_message = (payload.get("last_assistant_message") or "").lower()
if "mnemon" in last_message or "durable memory" in last_message:
    sys.exit(0)

print(json.dumps({
    "followup_message": (
        "[mnemon] Briefly evaluate whether this exchange warrants durable memory. "
        "If yes, use the mnemon skill/CLI to remember only durable, non-secret facts; "
        "otherwise say no durable memory is needed."
    )
}))
PY
