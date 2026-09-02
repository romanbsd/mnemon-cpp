#!/bin/bash
# mnemon ZCode Stop hook. Continue at most once so the agent can evaluate writeback.

python3 -c '
import json
import sys

try:
    payload = json.load(sys.stdin)
except Exception:
    payload = {}

if payload.get("stop_hook_active"):
    sys.exit(0)

last_message = (payload.get("last_assistant_message") or "").lower()
if "mnemon" in last_message or "durable memory" in last_message:
    sys.exit(0)

print(json.dumps({
    "decision": "block",
    "reason": (
        "[mnemon] Briefly evaluate whether this exchange warrants durable memory. "
        "If yes, use the mnemon skill/CLI to remember only durable, non-secret facts; "
        "otherwise say no durable memory is needed."
    ),
}))
'
