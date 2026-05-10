#!/bin/bash
# mnemon PreCompact hook — prompt LLM to save key insights before context compaction.
# Fires before context window compression (manual /compact or auto).

echo "[mnemon] Context compaction starting. Review this session and remember the most valuable insights (up to 5) before context is compressed. Delegate to Task sub-agents now."
