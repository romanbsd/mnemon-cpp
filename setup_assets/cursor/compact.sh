#!/bin/bash
# mnemon Cursor preCompact hook.
# Cursor shows user_message to the user; it does not inject model context.

cat >/dev/null || true
python3 - <<'PY'
import json

print(json.dumps({
    "user_message": (
        "[mnemon] Context compaction is starting. Preserve only critical, durable "
        "memory before compaction if this session introduced anything important."
    )
}))
PY
