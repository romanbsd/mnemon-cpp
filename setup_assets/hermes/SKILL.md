---
name: mnemon
description: Persistent memory CLI for Hermes Agent. Store facts, recall past knowledge, link related memories, manage lifecycle.
---

# mnemon

Use `mnemon` when durable memory can materially improve continuity across
Hermes sessions. Hooks may inject recalled context before an LLM call, but the
agent decides what is worth storing.

## Workflow

1. Recall when prior decisions, preferences, or facts may affect the current task:
   `mnemon recall "<query>" --limit 10`
2. Remember only stable, reusable knowledge:
   `mnemon remember "<fact>" --cat <cat> --imp <1-5> --entities "e1,e2" --source agent`
3. Link related memories after reviewing candidates from `remember`:
   `mnemon link <id> <candidate> --type <causal|semantic> --weight <0-1>`

## Commands

```bash
mnemon remember "<fact>" --cat <cat> --imp <1-5> --entities "e1,e2" --source agent
mnemon link <id1> <id2> --type <type> --weight <0-1> [--meta '<json>']
mnemon recall "<query>" --limit 10
mnemon search "<query>" --limit 10
mnemon import --dry-run <file>
mnemon import <file>
mnemon forget <id>
mnemon related <id> --edge causal
mnemon gc --threshold 0.4
mnemon gc --keep <id>
mnemon status
mnemon log
mnemon store list
mnemon store create <name>
mnemon store set <name>
mnemon store remove <name>
```

## Guardrails

- Do not store secrets, passwords, tokens, private keys, or short-lived operational noise.
- Prefer concise insights over transcript dumps.
- Categories: `preference` · `decision` · `insight` · `fact` · `context`
- Edge types: `temporal` · `semantic` · `causal` · `entity`
- Max 8,000 chars per insight.
