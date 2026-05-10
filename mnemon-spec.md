# Mnemon — Drop-In Replacement Specification

> **Audience.** This document specifies the behavior an implementation must reproduce to be a drop-in replacement for the reference Go binary. It is fully self-contained: an implementer should not need to read the Go source. Language and library choices are deferred to the implementer; this spec pins only the externally observable contract and the algorithms that affect that contract.

---

## 1. Purpose & Drop-In Contract

Mnemon is a single-binary memory daemon for LLM agents. Reference implementation: Go, SQLite (WAL), no CGO, no external service required. This spec covers a re-implementation in any language (Rust and modern C++ are both viable targets).

**Drop-in means: an existing user can replace the upstream binary with the new one and observe no externally-visible difference.** Specifically:

1. **CLI compatibility** — every command, subcommand, flag (long and short), positional argument, exit-code convention, and stdout JSON shape matches §6.
2. **Schema compatibility** — the new binary opens existing `~/.mnemon/data/<store>/mnemon.db` files written by the Go binary without conversion, and writes back rows readable by the Go binary. Schema is pinned in §4.
3. **On-disk layout compatibility** — same path layout under `MNEMON_DATA_DIR` (default `~/.mnemon`), same `active` file format, same `prompt/` shared directory, same legacy migration target (§3).
4. **Hook/setup compatibility** — `mnemon setup` writes byte-identical hook scripts, skill files, plugin manifests, and `settings.json` blocks to the same target paths. `--eject` removes everything `setup` installed.

**Out of scope** (§15):
- New edge types, new categories, schema additions.
- Stable internal API or library bindings — the contract surface is the CLI and the on-disk files.
- Re-deriving the embedded assets (hook scripts, skills, guides). The implementer **copies these bytes verbatim** into `setup_assets/` (same tree as the reference monorepo’s `internal/setup/assets/`). They are a fixed corpus, not specified inline here.

**Conformance test** (§13): `scripts/e2e_test.sh` in this project is the authoritative behavioral test. It must pass against the new binary (use `MNEMON_TEST_BINARY` to point at a prebuilt binary if needed).

---

## 2. System Overview

### 2.1 The LLM-Supervised Pattern

Mnemon does **not** embed an LLM. The host LLM (Claude Code, OpenClaw, etc.) is the supervisor: it decides what to remember, what to link, when to forget. The binary handles deterministic computation: storage, graph indexing, retrieval, lifecycle decay.

This means the binary has **no API keys**, no network calls except an optional HTTP request to a local Ollama instance for embeddings (§8). It is a CLI that emits JSON; nothing else.

### 2.2 Five-Layer Architecture

```
Integration   hooks, skills, guides (assets embedded into the binary, copied to disk by `setup`)
CLI           command parsing, flag validation, JSON output
Engine        recall (intent + RRF + beam search + rerank), edge generators, diff/dedup
Storage       SQLite WAL, transactions, schema migration, oplog
External      Ollama HTTP (optional, localhost only by default)
```

The implementer's module/crate boundaries should respect these layers. Engine code must not reach into CLI parsing; storage code must not reach into engine algorithms; the Ollama client must never hold a SQL transaction (§7.3, §7.6).

### 2.3 Canonical Flows

**`remember <content>`:**

1. Validate content/category/importance/tags/entities (§6.1 limits).
2. Generate UUID v4. Capture `now = UTC RFC3339`.
3. **If embedding enabled** (Ollama probe succeeds within 2 s): embed `<content>` via Ollama HTTP. *This call must not hold a SQL transaction.*
4. **If `--no-diff` not set**: load all active insights, run dedup/diff (§7.3). Result is one of `ADD`, `DUPLICATE`, `CONFLICT`, `UPDATE`.
   - `DUPLICATE` → log `diff-skip`, emit `action: "skipped"` JSON, exit 0 without inserting.
   - `CONFLICT` or `UPDATE` → mark for soft-delete-then-replace.
   - `ADD` → proceed.
5. **Begin transaction.**
   - If replacing: soft-delete the matched insight, log `diff-replace`.
   - Insert the new insight row.
   - If embedding present: write `embedding` blob.
   - Run edge engine (§7.4): create temporal, entity, causal, semantic edges.
   - Update `entities` field with merged extracted entities.
   - Recompute and persist `effective_importance` (§7.7).
   - Run `AutoPrune` (cap = 1000, batch = 10), excluding the just-inserted ID.
   - Log `remember`.
   - **Commit.**
6. **Outside the transaction**, compute `semantic_candidates` and `causal_candidates` for the JSON response (read-only).
7. Emit JSON (§6.1).

**`recall <query>`:**

1. If `--basic`: SQL `LIKE '%<query>%'` filter, ordered by `importance DESC, created_at DESC`. Increment `access_count` for each result. Log `recall:basic`. Emit array.
2. Otherwise: intent-aware recall (§7.6).
   - Resolve intent (override or auto-detect).
   - Try to embed query (Ollama, optional).
   - Extract entities from query (§7.2 patterns + dictionary).
   - Run RRF anchor selection (3 signals: keyword, vector if embeddings, time).
   - Beam-search from each anchor with intent-weighted edge transitions.
   - Multi-factor rerank (keyword, entity, similarity, graph).
   - For `WHY` intent: causal topological sort (Kahn's with score-tiebreak).
   - Hint `"sparse_results"` if results are < `limit/2`.
   - Increment `access_count` for each returned insight. Log `recall`.
   - Emit `RecallResponse` (§6.2).

---

## 3. On-Disk Layout & Migration

### 3.1 Layout

```
$MNEMON_DATA_DIR/                       (default: $HOME/.mnemon, /tmp/.mnemon if HOME unset)
├── active                              plain text: name of the active store + trailing "\n"
├── prompt/                             shared across stores
│   ├── guide.md                        behavioral guide (copied from upstream assets)
│   └── skill.md                        skill definition (copied from upstream assets)
└── data/
    ├── default/                        always present (auto-created on first use)
    │   ├── mnemon.db                   SQLite database
    │   ├── mnemon.db-wal               (WAL mode sidecar, runtime)
    │   └── mnemon.db-shm               (WAL mode sidecar, runtime)
    └── <store-name>/                   additional named stores
        └── mnemon.db
```

### 3.2 Path Resolution

- `MNEMON_DATA_DIR` env var overrides default.
- Default = `$HOME/.mnemon`. If `$HOME` is unset and `os.UserHomeDir()`-equivalent fails, fall back to `/tmp/.mnemon`.
- Store directory = `<base>/data/<store>`.
- Active file path = `<base>/active`.
- Database file path = `<base>/data/<store>/mnemon.db`.

### 3.3 Legacy Migration (always run before opening a writable DB)

If `<base>/mnemon.db` exists *and* `<base>/data/default/mnemon.db` does **not** exist:

1. `mkdir -p <base>/data/default`.
2. Move `<base>/mnemon.db` → `<base>/data/default/mnemon.db`.
3. If `<base>/mnemon.db-wal` exists, move it to `<base>/data/default/mnemon.db-wal`.
4. If `<base>/mnemon.db-shm` exists, move it to `<base>/data/default/mnemon.db-shm`.
5. Print `mnemon: migrated database to <new path>` to stderr.

Idempotent: re-running on a migrated tree is a no-op. Read-only mode (`--readonly`) skips this.

### 3.4 Active File

- File contents: store name followed by a single `\n`. Trim whitespace on read.
- Empty or missing → resolve to `"default"`.
- Written with mode `0644`; parent directory created with `0755` if missing.

### 3.5 Store Name Validation

Regex (anchored): `^[a-zA-Z0-9][a-zA-Z0-9_-]*$`. Rejects names starting with `-` or `_`, names containing path separators, dots, spaces, or other punctuation.

---

## 4. SQLite Schema (authoritative)

### 4.1 Initial Schema

The implementation must execute the following on every `Open` against a writable database. All statements are `IF NOT EXISTS` so this is idempotent.

```sql
CREATE TABLE IF NOT EXISTS insights (
    id          TEXT PRIMARY KEY,
    content     TEXT NOT NULL,
    category    TEXT DEFAULT 'general',
    importance  INTEGER DEFAULT 3,
    tags        TEXT DEFAULT '[]',
    entities    TEXT DEFAULT '[]',
    source      TEXT DEFAULT 'user',
    access_count INTEGER DEFAULT 0,
    created_at  TEXT NOT NULL,
    updated_at  TEXT NOT NULL,
    deleted_at  TEXT
);

CREATE TABLE IF NOT EXISTS edges (
    source_id   TEXT NOT NULL,
    target_id   TEXT NOT NULL,
    edge_type   TEXT NOT NULL CHECK(edge_type IN ('temporal','semantic','causal','entity')),
    weight      REAL DEFAULT 1.0,
    metadata    TEXT DEFAULT '{}',
    created_at  TEXT NOT NULL,
    PRIMARY KEY (source_id, target_id, edge_type),
    FOREIGN KEY (source_id) REFERENCES insights(id) ON DELETE CASCADE,
    FOREIGN KEY (target_id) REFERENCES insights(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_insights_category    ON insights(category);
CREATE INDEX IF NOT EXISTS idx_insights_importance  ON insights(importance);
CREATE INDEX IF NOT EXISTS idx_insights_created     ON insights(created_at);
CREATE INDEX IF NOT EXISTS idx_insights_deleted     ON insights(deleted_at);
CREATE INDEX IF NOT EXISTS idx_insights_source      ON insights(source);
CREATE INDEX IF NOT EXISTS idx_edges_source         ON edges(source_id);
CREATE INDEX IF NOT EXISTS idx_edges_target         ON edges(target_id);
CREATE INDEX IF NOT EXISTS idx_edges_type           ON edges(edge_type);
CREATE INDEX IF NOT EXISTS idx_edges_source_type    ON edges(source_id, edge_type);
CREATE INDEX IF NOT EXISTS idx_edges_target_type    ON edges(target_id, edge_type);

CREATE TABLE IF NOT EXISTS oplog (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    operation   TEXT NOT NULL,
    insight_id  TEXT,
    detail      TEXT DEFAULT '',
    created_at  TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_oplog_created ON oplog(created_at);
```

### 4.2 Additive Migrations (run after the initial schema, in order)

These were added in successive upstream versions. The implementation must apply them on every `Open`. Each `ALTER TABLE ADD COLUMN` may report "duplicate column name"; that error is to be ignored, all other errors propagated.

```sql
ALTER TABLE insights ADD COLUMN last_accessed_at      TEXT;
ALTER TABLE insights ADD COLUMN embedding             BLOB;
ALTER TABLE insights ADD COLUMN effective_importance  REAL DEFAULT 0.5;

CREATE INDEX IF NOT EXISTS idx_insights_effective_imp ON insights(effective_importance);
CREATE INDEX IF NOT EXISTS idx_prune_candidates ON insights(deleted_at, importance, access_count, effective_importance);
```

### 4.3 Narrative-Edges Cleanup

Older databases allowed `edge_type = 'narrative'`. Detect and rebuild:

1. Probe with `INSERT INTO edges VALUES ('__test','__test','narrative',0,'{}',datetime('now'))`. If it errors, the schema is current — skip.
2. If it succeeds, run inside a transaction:
   - `DELETE FROM edges WHERE source_id = '__test'`
   - `DELETE FROM edges WHERE edge_type = 'narrative'`
   - `ALTER TABLE edges RENAME TO edges_old`
   - Re-create `edges` exactly as in §4.1 (with the four-type CHECK).
   - `INSERT INTO edges SELECT * FROM edges_old`
   - `DROP TABLE edges_old`
   - Re-create the three indexes `idx_edges_source`, `idx_edges_target`, `idx_edges_type`.

### 4.4 Narrative-Category Cleanup

Detect with `SELECT COUNT(*) FROM insights WHERE category = 'narrative' AND deleted_at IS NULL`. If non-zero:

```sql
UPDATE insights SET deleted_at = datetime('now')
WHERE category = 'narrative' AND deleted_at IS NULL;
```

(Skip the `UPDATE` when count is zero — avoids unnecessary writes.)

### 4.5 Open Pragmas

**Read-write open:**
```
journal_mode = WAL
foreign_keys = ON
```
Single open connection (the equivalent of `MaxOpenConns=1` in Go). SQLite is a single-writer engine; running multiple writers from one process produces lock contention without parallelism gain.

**Read-only open** (`--readonly`):
```
mode         = ro
journal_mode = OFF
foreign_keys = ON
```
`journal_mode=OFF` is required so that opening a DB on a read-only filesystem mount does not attempt to write WAL/SHM sidecars. Reject the open if the file does not exist (do not auto-create in read-only mode).

---

## 5. Data Model

### 5.1 Insight

| Field | Type | JSON key | Storage |
|---|---|---|---|
| ID | UUID v4 string (lowercase hex with hyphens, RFC 4122 §4.4 random) | `id` | `TEXT PRIMARY KEY` |
| Content | string, ≤ 8000 chars | `content` | `TEXT NOT NULL` |
| Category | enum: `preference \| decision \| fact \| insight \| context \| general` | `category` | `TEXT` |
| Importance | int 1..5 | `importance` | `INTEGER` |
| Tags | array of strings, each ≤ 100 chars, ≤ 20 items | `tags` | `TEXT` (JSON-encoded array) |
| Entities | array of strings, each ≤ 200 chars, ≤ 50 items | `entities` | `TEXT` (JSON-encoded array) |
| Source | string, no enum constraint at storage level (CLI defaults to `user`/`agent`/`external`) | `source` | `TEXT` |
| AccessCount | int, default 0 | `access_count` | `INTEGER` |
| CreatedAt | RFC 3339 UTC | `created_at` | `TEXT` |
| UpdatedAt | RFC 3339 UTC | `updated_at` | `TEXT` |
| DeletedAt | RFC 3339 UTC, nullable | `deleted_at` (omitted if null) | `TEXT NULL` |
| LastAccessedAt | RFC 3339 UTC, nullable; falls back to `CreatedAt` when null | not in standard JSON | `TEXT NULL` |
| Embedding | float64 vector, nullable | not in standard JSON | `BLOB` (see §5.4) |
| EffectiveImportance | float, default 0.5 | not in standard `Insight` JSON; surfaced in `gc` and `remember` | `REAL` |

**`tags` and `entities`** are stored as JSON arrays of strings. On insert, an empty list is stored as the literal `"[]"` (never NULL). On read, a NULL or unparseable value yields an empty list.

**Timestamps** are RFC 3339 with explicit timezone offset, always UTC (e.g. `2026-05-10T12:34:56Z`). The implementation must serialize using the equivalent of Go's `time.RFC3339` format and parse the same.

### 5.2 Categories

```
preference   — user preference
decision     — architectural/technical decision
fact         — objective fact
insight      — reasoning conclusion
context      — project context
general      — fallback
```

Validation: any other value passed to `--cat` produces error `invalid category "<x>"; valid: preference, decision, fact, insight, context, general` and exit code non-zero.

### 5.3 Edge

| Field | Type | JSON key | Storage |
|---|---|---|---|
| SourceID | UUID string | `source_id` | `TEXT` (FK insights.id) |
| TargetID | UUID string | `target_id` | `TEXT` (FK insights.id) |
| EdgeType | enum: `temporal \| semantic \| causal \| entity` | `edge_type` | `TEXT` (CHECK constrained) |
| Weight | float 0..1 | `weight` | `REAL` |
| Metadata | map<string,string> | `metadata` | `TEXT` (JSON-encoded object) |
| CreatedAt | RFC 3339 UTC | `created_at` | `TEXT` |

**Composite PK**: `(source_id, target_id, edge_type)`. `INSERT OR REPLACE` semantics for upsert.

**`metadata`** is a flat string→string map. Common keys: `sub_type` (e.g. `"backbone"`, `"proximity"`, `"causes"`, `"enables"`, `"prevents"`), `direction` (`"precedes"`/`"succeeds"`), `created_by` (`"auto"`, `"claude"`), `cosine`, `overlap`, `entity`, `hours_diff`, `reason`. Empty `metadata` serializes as `"{}"`.

### 5.4 Embedding Blob Format

```
Encoding: little-endian sequence of IEEE 754 binary64 values
Length:   N * 8 bytes for an N-dimensional vector
NULL:     no embedding present
```

Serialize: for each `f64` in the vector, write 8 little-endian bytes. Deserialize: if `len(blob) % 8 != 0` or `len(blob) == 0`, treat as missing (return null). Otherwise slice into `f64`s.

Vector dimensionality is determined by the embedding model (default 768 for `nomic-embed-text`). The schema does not validate dimensions — vectors of mismatched length are simply incompatible for cosine similarity (which returns 0 in that case, §7.3).

### 5.5 UUID Format

Standard RFC 4122 v4 (random), formatted as 36 chars: `xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx` where `y ∈ {8,9,a,b}`, all hex lowercase. Existing databases were written with `github.com/google/uuid` defaults — the implementation must use the same canonical formatting (lowercase, hyphenated, no `urn:uuid:` prefix).

### 5.6 Soft Delete

`deleted_at` is the soft-delete tombstone. Conventions:

- All read paths filter `WHERE deleted_at IS NULL` unless explicitly fetching deleted rows.
- Soft-deleting an insight also `DELETE FROM edges WHERE source_id = ? OR target_id = ?` — soft delete on a node means hard delete on its edges. The intent is that edges to dead nodes are never traversed.
- The FK `ON DELETE CASCADE` on `edges` is a defense-in-depth: it triggers only when an `insights` row is hard-deleted (which the application code never does).

---

## 6. CLI Surface

### 6.1 Global Flags

Available on every command:

| Flag | Default | Behavior |
|---|---|---|
| `--data-dir <path>` | `$MNEMON_DATA_DIR` or `$HOME/.mnemon` | base data directory |
| `--store <name>` | (auto, see §9) | named memory store (overrides env and `active` file) |
| `--readonly` | `false` | open DB read-only with `journal_mode=OFF` (§4.5) |
| `--version` / `-v` | | print version and exit 0 |
| `--help` / `-h` | | print command help and exit 0 |

**Help text** is generated from the command tree; minor wording differences from upstream are acceptable so long as flag names, defaults, and value formats match.

**Exit codes**: `0` on success; non-zero on any error (Go's `os.Exit(1)`-on-error pattern). The exact non-zero value is not contractually pinned — only "0 vs non-zero" matters for `e2e_test.sh`.

**Error output**: writes `<error-message>\n` to stderr and exits non-zero. Output to stdout on the success path is the only thing the test harness inspects with `jq`.

### 6.2 Commands

#### `mnemon remember <content...>`

Stores a new insight. `content` is the concatenation of all positional args separated by single spaces.

| Flag | Default | Validation |
|---|---|---|
| `--cat` | `general` | one of the six categories (§5.2) |
| `--imp` | `3` | integer 1..5 |
| `--tags` | `""` | comma-separated; each tag ≤ 100 chars; ≤ 20 tags |
| `--entities` | `""` | comma-separated; each ≤ 200 chars; ≤ 50 entries |
| `--source` | `user` | free text, conventionally `user`/`agent`/`external` |
| `--no-diff` | `false` | skip diff/dedup (§7.3); always inserts as `ADD` |

**Length error**: content > 8000 chars → `content too long (<n> chars, max 8000); consider chunking into multiple remember calls`. Exit non-zero.

**JSON output, `action=skipped` (DUPLICATE):**

```json
{
  "id": "<new-uuid-that-was-not-inserted>",
  "content": "<input content>",
  "action": "skipped",
  "diff_suggestion": "DUPLICATE",
  "replaced_id": "<existing-id>"
}
```

**JSON output, `action=added` or `action=updated`:**

```json
{
  "id": "<uuid>",
  "content": "<content>",
  "category": "<category>",
  "importance": <int>,
  "tags": ["..."],
  "entities": ["..."],
  "action": "added" | "updated",
  "diff_suggestion": "ADD" | "CONFLICT" | "UPDATE",
  "created_at": "<RFC3339>",
  "edges_created": {
    "temporal": <int>,
    "entity":   <int>,
    "causal":   <int>,
    "semantic": <int>
  },
  "semantic_candidates": [
    { "id": "...", "content": "...", "category": "...",
      "similarity": 0.0, "auto_linked": false }
  ],
  "causal_candidates": [
    { "id": "...", "content": "...", "category": "...",
      "hop": 1, "via_edge": "entity",
      "causal_signal": "because", "suggested_sub_type": "causes" }
  ],
  "embedded": <bool>,
  "effective_importance": <float>,
  "auto_pruned": <int>,
  "replaced_id": "<id>"            // only when action=updated
}
```

JSON serializer must indent with two spaces and emit a trailing newline (matches `json.NewEncoder(os.Stdout).SetIndent("", "  ").Encode(...)`). All other commands use the same encoder.

`semantic_candidates` and `causal_candidates` are always present as arrays (never null); `[]` when empty.

#### `mnemon recall <query...>`

| Flag | Default | Notes |
|---|---|---|
| `--cat` | `""` | filter by category (only used in `--basic` mode) |
| `--source` | `""` | filter by source (only used in `--basic` mode) |
| `--limit` | `10` | max results |
| `--basic` | `false` | use SQL LIKE matching instead of intent-aware recall |
| `--intent` | `""` | override intent: `WHY`, `WHEN`, `ENTITY`, `GENERAL` |
| `--smart` | (hidden, deprecated, no-op) | accepted for backward compat; not advertised |

**Basic mode** (`--basic`) output is a JSON array of `Insight` objects, ordered `importance DESC, created_at DESC`, limited to `--limit`. SQL: `WHERE deleted_at IS NULL AND content LIKE '%<query>%' [AND category = ?] [AND source = ?]`.

**Smart mode** (default) output (`RecallResponse`):

```json
{
  "results": [
    {
      "insight": { /* full Insight object */ },
      "score":   <float>,
      "intent":  "WHY" | "WHEN" | "ENTITY" | "GENERAL",
      "via":    "keyword" | "vector" | "time" | "hybrid" | "<edge-type>",
      "signals": {
        "keyword":    <float>,
        "entity":     <float>,
        "similarity": <float>,
        "graph":      <float>
      }
    }
  ],
  "meta": {
    "intent":        "WHY" | "WHEN" | "ENTITY" | "GENERAL",
    "intent_source": "auto" | "override",
    "anchor_count":  <int>,
    "traversed":     <int>,
    "hint":          "sparse_results"   // omitted when empty
  }
}
```

Both modes increment `access_count` (and `last_accessed_at`) for each returned insight.

#### `mnemon search <query...>`

Token-scored keyword search (no graph traversal).

| Flag | Default |
|---|---|
| `--limit` | `10` |

Output: JSON array of `{id, content, category, importance, tags, score}`. Score = `|intersection(query_tokens, content_tokens)| / |query_tokens|` (§7.1). Sort by score desc; tie-break by importance desc.

#### `mnemon link <source_id> <target_id>`

| Flag | Default | Validation |
|---|---|---|
| `--type` | `semantic` | one of `temporal`, `semantic`, `causal`, `entity` |
| `--weight` | `0.5` | float 0..1 |
| `--meta` | `""` | optional JSON object string; merged into edge metadata |

Validates both IDs reference active insights. Always creates a **bidirectional** pair: `(source→target)` and `(target→source)`, both with the given type, weight, metadata (forced `created_by: "claude"`).

Output:
```json
{
  "status":    "linked",
  "source_id": "...",
  "target_id": "...",
  "edge_type": "...",
  "weight":    0.5,
  "metadata":  { "created_by": "claude", ... }
}
```

#### `mnemon related <id>`

| Flag | Default | Validation |
|---|---|---|
| `--edge` | `""` | filter by edge type (empty = all) |
| `--depth` | `2` | max BFS depth |

BFS from `<id>` over active nodes only, optionally filtered by edge type (§7.4 BFS rules). Output: JSON array of `{id, content, category, importance, depth, via_edge_type}`.

#### `mnemon forget <id>`

Soft-delete the insight (sets `deleted_at`, removes its edges). Output:
```json
{ "id": "...", "status": "deleted", "message": "Insight soft-deleted successfully" }
```
Error if ID missing or already deleted.

#### `mnemon gc`

| Flag | Default |
|---|---|
| `--threshold` | `0.5` |
| `--limit` | `20` |
| `--keep <id>` | `""` |

**Keep mode** (when `--keep` is set): boost the insight's `access_count` by 3, refresh `last_accessed_at` and `updated_at`, recompute `effective_importance`. Log `gc_keep`. Output:
```json
{
  "status": "retained",
  "id": "<id>",
  "content": "<content>",
  "new_access": <int>,
  "effective_importance": <float>,
  "immune": <bool>
}
```

**Suggest mode** (default): compute fresh `effective_importance` for all active insights (and persist them in a single transaction; failures are warnings, not fatal). Return non-immune insights with `EI < threshold`, sorted by EI ascending, capped to `--limit`. Output:
```json
{
  "total_insights":   <int>,
  "threshold":        <float>,
  "candidates_found": <int>,
  "candidates": [
    {
      "insight":              { /* Insight */ },
      "effective_importance": <float>,
      "days_since_access":    <float>,
      "edge_count":           <int>,
      "immune":               false
    }
  ],
  "max_insights": 1000,
  "actions": {
    "purge": "mnemon forget <id>",
    "keep":  "mnemon gc --keep <id>"
  }
}
```

#### `mnemon embed [<id>]`

| Flag | Default |
|---|---|
| `--all` | `false` |
| `--status` | `false` |

**`--status` mode** (no DB writes):
```json
{
  "total_insights":  <int>,
  "embedded":        <int>,
  "coverage":        "<int>%",          // "0%" when total=0
  "ollama_available": <bool>,
  "model":           "nomic-embed-text"
}
```
Coverage is formatted as `"<n>%"` where `n = round(embedded / max(total,1) * 100)`.

**Single-insight mode** (positional `<id>`): embed the insight's content via Ollama, store the blob. Errors if Ollama unavailable. Output:
```json
{ "status":"embedded", "id":"<id>", "dimension": <int>, "model":"<name>" }
```

**`--all` (backfill)**: iterate active insights with NULL embedding, embed each, store. Continue past per-row errors. Output:
```json
{ "status":"backfill_complete", "succeeded": <int>, "failed": <int>, "model":"<name>" }
```
If no rows missing embeddings:
```json
{ "status":"complete", "message":"all insights already have embeddings" }
```

If neither `--all`, `--status`, nor `<id>` given: error `specify --all to backfill, --status to check coverage, or provide an insight ID`.

#### `mnemon viz`

| Flag | Default |
|---|---|
| `--format` | `dot` (one of `dot`, `html`) |
| `-o`, `--output` | `-` (stdout) |

Output rules in §11.

#### `mnemon status`

```json
{
  "total_insights":   <int>,
  "deleted_insights": <int>,
  "by_category":      { "<cat>": <int>, ... },
  "edge_count":       <int>,
  "top_entities":     [ { "entity":"<name>", "count": <int> }, ... ],   // top 20 by distinct insights
  "oplog_count":      <int>,
  "db_path":          "<absolute path>",
  "db_size_bytes":    <int>
}
```

`top_entities` is queried via SQLite's `json_each` over `insights.entities`:
```sql
SELECT je.value, COUNT(DISTINCT i.id) AS cnt
FROM insights i, json_each(i.entities) je
WHERE i.deleted_at IS NULL
GROUP BY je.value ORDER BY cnt DESC LIMIT 20;
```
Implementations without bundled `json_each` must replicate this aggregate (e.g. by parsing entities in application code) — output must be identical.

#### `mnemon log`

| Flag | Default |
|---|---|
| `--limit` | `20` |

**Tabular output** to stdout (not JSON). Use a tab-separated layout that prints aligned columns when piped through column-stretching tabwriter logic equivalent to Go's `text/tabwriter` with min-width 0, tabwidth 0, padding 2, padchar `' '`:

```
TIME                  OP        INSIGHT   DETAIL
----                  --        -------   ------
2026-05-10T12:34:56Z  remember  abcd1234  Chose Qdrant over Milvus...
```

`INSIGHT` truncated to first 8 chars. `DETAIL` truncated to 60 chars; if longer, suffix `...` (so visible width is 60: `[..57 chars]...`).

Empty oplog: print exactly `No operations recorded yet.\n`.

#### `mnemon store list`

Print one store name per line, each prefixed by `* ` (active) or `  ` (inactive). Empty:

```
  (no stores yet — run 'mnemon store create <name>' or any command to create default)
```

#### `mnemon store create <name>`

Validate name (§3.5). Error if exists. Create directory, open + initialize DB, close. Print `Created store "<name>"`.

#### `mnemon store set <name>`

Error if store does not exist (`'mnemon store create <name>' first`). Write `<name>\n` to `<base>/active`. Print `Active store set to "<name>"`.

#### `mnemon store remove <name>`

Error if store does not exist. Error if `<name>` resolves as the active store (`cannot remove the active store "<name>" (switch first with 'mnemon store set <other>')`). Remove the directory recursively. Print `Removed store "<name>"`.

#### `mnemon setup`

| Flag | Default |
|---|---|
| `--target` | `""` (auto-detect) — values: `claude-code`, `openclaw` |
| `--global` | `false` — install to `~/.claude` / `~/.openclaw` instead of project-local |
| `--eject` | `false` — uninstall mode |
| `--yes` | `false` — non-interactive: accept defaults |

Behavior covered in §12.

---

## 7. Algorithms

### 7.1 Tokenization

Used by keyword search, dedup similarity, causal-edge overlap.

```
Input:  free-text string
Output: set of distinct lowercase tokens
```

1. Lowercase the input.
2. Walk the string rune-by-rune.
3. **CJK runes** (Unicode `Han` script — code points in CJK Unified Ideographs and related blocks):
   - Buffer them. When the run ends (or string ends), emit *character bigrams* (consecutive 2-rune slices).
   - If the run is exactly 1 rune long, emit that single rune as a token.
4. **Letters and digits** (Unicode letter/digit categories): accumulate into the current word.
5. **Anything else** (whitespace, punctuation, symbols): terminates the current word. Emit it if non-empty and not in the stopword list (§7.1.1).
6. Deduplicate by inserting into a set.

Token weights: every emitted token contributes 1.0 (set semantics — no count). Tokens that are present in the stopword list are dropped *before* deduplication (so an all-stopword query produces an empty set).

#### 7.1.1 Stopword List (verbatim)

```
a an the is are was were be been being have has had do does did
will would could should may might shall can to of in for on with at by
from as into about that this it its or and but if not no so up out
than then too very just also more some any all each
i me my we you your he she they them his her our their
what which who how when where
```

These are the literal Go map keys; preserve them exactly.

### 7.2 Entity Extraction

Used to populate `insight.entities` (merged with anything the LLM passed via `--entities`).

#### 7.2.1 Regex Patterns (run in order, all in case-sensitive mode unless noted)

```
1. CamelCase                \b([A-Z][a-z]+(?:[A-Z][a-z]+)+)\b
2. ALLCAPS acronym 2..6     \b([A-Z]{2,6})\b
3. File path                (?:^|[\s"'(])([.\w/-]+\.\w{1,10})(?:[\s"'),.]|$)
4. URL                      https?://[^\s"'<>)]+
5. @mention                 @([a-zA-Z_]\w+)
6. CJK book/quote marks     [《「]([^》」]+)[》」]
```

For each match, take the **last** capture group (or the whole match for the URL pattern, which has no group).

#### 7.2.2 Acronym Stopwords

After regex extraction, drop any candidate that matches a member of the acronym stopword set (case-sensitive ALLCAPS):

```
IN ON AT TO BY OR AN IF IS IT OF AS DO NO SO UP WE HE MY BE GO
THE AND FOR ARE BUT NOT YOU ALL CAN HER WAS ONE OUR OUT HAS HAD
HOW MAN NEW NOW OLD SEE WAY MAY SAY SHE TWO USE BOY DID GET HIM
HIS LET PUT TOP TOO ANY
```

#### 7.2.3 Tech Dictionary

After regex extraction, also scan the input by splitting on non-`[a-zA-Z0-9]` runs (preserving original case) and add any word that exact-matches an entry in this dictionary:

```
Languages:    Go Rust Python Java Kotlin Swift Ruby Elixir Zig Lua
              Dart Scala Perl Haskell OCaml Julia Clojure
JS:           JavaScript TypeScript React Vue Angular Svelte Next Nuxt
              Node Deno Bun Vite Webpack
DBs:          SQLite PostgreSQL Postgres MySQL Redis MongoDB DynamoDB Cassandra
              Qdrant Milvus Chroma Pinecone Neo4j Weaviate Elasticsearch
Infra:        Docker Kubernetes Terraform Ansible Nginx Caddy Kafka RabbitMQ
              AWS GCP Azure Vercel Netlify Cloudflare Supabase Firebase
AI/ML:        Ollama OpenAI Claude Anthropic PyTorch TensorFlow LangChain
              LlamaIndex FAISS Hugging
Tools:        Git GitHub GitLab Cobra FastAPI Flask Django Rails Spring
              Express Gin Echo Fiber Pytest Jest Vitest
Protocols:    gRPC GraphQL WebSocket OAuth JWT YAML TOML Protobuf
Mnemon:       MAGMA MCP RLM
```

Match is case-sensitive and exact-token (no substring). Implementations should hold this as a hash set.

#### 7.2.4 Output Order

`extracted = (regex matches in pattern order, then dictionary matches in dictionary-iteration order)`, deduplicated preserving first occurrence. When merged with LLM-provided entities (passed via `--entities`), provided entities come first (also deduped).

### 7.3 Diff / Dedup (§5 of `remember`)

Inputs: new content string, all active insights, optional new embedding vector, optional pre-loaded existing embeddings.

**Step 1** — Keyword candidates: run §7.1 keyword search across all active insights, return top 5 (parameter `Limit`, default 5).

**Step 2** — Score each candidate:
- `tokenSim` = symmetric token overlap = `max(|A∩B|/|A|, |A∩B|/|B|)` over tokenized content (§7.1, this metric is `ContentSimilarity` not the asymmetric `KeywordSearch` score).
- `cosineSim` = cosine similarity between new and existing embedding if both present, else 0.
- **Combined**: `similarity = tokenSim`; if `cosineSim ≥ 0.7` and `cosineSim > tokenSim`, use `cosineSim` instead.

**Step 3** — Cosine fan-out: if a query embedding exists, also evaluate cosine against every existing embedded insight not yet in the candidate set; keep those with `cosineSim ≥ 0.7`; sort desc; take top `Limit`. For each, recompute the combined similarity. Drop entries that classify as `ADD`.

**Step 4** — Classify each candidate:

```
similarity < 0.5                                         → ADD
otherwise, if any negation keyword in either text        → CONFLICT
otherwise, similarity > 0.9                              → DUPLICATE
otherwise                                                → UPDATE
```

Negation keyword set:
```
not, no longer, don't, doesn't, never, switched from, instead of, rather than,
replaced, deprecated,
不, 没有, 不再, 放弃, 替换, 取消
```
Substring match in lowercased text.

**Step 5** — Overall suggestion:
- Default: take the suggestion of the strongest (first, highest-similarity) match, or `ADD` if no matches.
- **Override**: if any candidate is `DUPLICATE`, the overall suggestion is `DUPLICATE`. (Stronger signal wins.)

**Output**: `{Suggestion, Matches[]}` — drives the `remember` action mapping:

```
DUPLICATE                  → action="skipped"
CONFLICT or UPDATE         → action="updated"  (and replaced_id = matches[0].id)
ADD or no matches          → action="added"
```

`replaced_id` is the ID of the first (strongest) match.

### 7.4 Edge Engine

Run inside the `remember` transaction, after the new insight row is inserted. Order matters: temporal → entity → causal → semantic.

#### 7.4.1 Temporal

Constants: `windowHours = 24`, `maxProximityEdges = 10`.

1. **Backbone**: query the most recent active insight by `source` excluding the new ID (`SELECT ... ORDER BY created_at DESC, rowid DESC LIMIT 1`). If found:
   - Insert `prev → new` with type `temporal`, weight `1.0`, metadata `{sub_type:"backbone", direction:"precedes"}`.
   - Insert `new → prev` with same type/weight, metadata `{sub_type:"backbone", direction:"succeeds"}`.
2. **Proximity**: query active insights with `source = ?` and `created_at >= now - 24h`, excluding the new ID, ordered by `created_at DESC LIMIT 10`. (Note: upstream uses *all sources* in the proximity window, not source-filtered — see implementation reference: `GetRecentInsightsInWindow` filters only by ID and time window. Match this behavior.)
3. For each proximity neighbor (skipping the backbone target):
   - `hoursDiff = abs(new.CreatedAt - near.CreatedAt) / 1h`
   - `weight = 1 / (1 + hoursDiff)`
   - Insert bidirectional `temporal` edges, metadata `{sub_type:"proximity", hours_diff: "<%.2f>"}`.

Returns count of successfully-inserted edges.

#### 7.4.2 Entity Co-occurrence

Constants: `maxEntityLinks = 5` (per entity), `maxTotalEntityEdges = 50` (per insight).

For each entity in `insight.entities` (after `mergeEntities`):
- Query distinct active insight IDs that contain this entity in their JSON `entities` array, excluding the new ID, ordered `created_at DESC`, LIMIT 5:
  ```sql
  SELECT DISTINCT i.id FROM insights i, json_each(i.entities) je
   WHERE i.deleted_at IS NULL AND i.id != ? AND je.value = ?
   ORDER BY i.created_at DESC LIMIT 5
  ```
- For each result, insert bidirectional `entity` edges with weight `1.0`, metadata `{entity:"<name>"}`. Stop early once `maxTotalEntityEdges` edges have been created in total.

#### 7.4.3 Causal

Constants: `causalLookback = 10`, `minCausalOverlap = 0.15`.

Causal keyword regex (case-insensitive):
```
\b(because|therefore|due to|caused by|as a result|decided to|chosen because|so that|in order to|leads to|results in)\b
| (因为|所以|由于|导致|因此|决定|为了|以便)
```

1. Get the 10 most recent insights from the same `source` (excluding the new ID).
2. Tokenize new content (§7.1). If empty, return 0.
3. `newHasSignal = causalRegex.test(new.content)`.
4. For each prev:
   - `prevHasSignal = causalRegex.test(prev.content)`.
   - If neither has signal, skip.
   - `overlap = |intersection(newTokens, prevTokens)| / max(|newTokens|, |prevTokens|)`.
   - If `overlap < 0.15`, skip.
   - **Direction**: the side with the keyword is the *effect*; the other is the *cause*; edge direction is cause → effect.
     - new has signal → `prev → new`
     - prev has signal (and not new) → `new → prev`
   - Sub-type from `(new.content + " " + prev.content)`:
     ```
     prevents:  \b(despite|prevented|prevents|blocked)\b | (阻止|防止)
     enables:   \b(so that|in order to|enables|leads to)\b | (为了|以便)
     causes:    \b(because|caused by|due to)\b | (因为|由于)
     default:   "causes"
     ```
     (Check `prevents` first, then `enables`, then `causes`.)
   - Insert single-direction `causal` edge with `weight = overlap`, metadata `{overlap:"<%.4f>", sub_type:"<sub>"}`.

#### 7.4.4 Semantic Auto-Link

Constants: `autoSemanticThreshold = 0.80`, `maxAutoSemanticEdges = 3`.

Requires embeddings. Skipped if the new insight has no embedding.

1. Iterate the embed cache (or query `GetAllEmbeddings`). For each `(otherID, otherVec)` where `otherID != new.ID`:
   - Compute `cosSim = cosine(insightVec, otherVec)`.
   - If `cosSim ≥ 0.80`, add to candidates.
2. Sort candidates by similarity desc, keep top 3.
3. For each, insert bidirectional `semantic` edges with `weight = cosSim`, metadata `{created_by:"auto", cosine:"<%.4f>"}`.

#### 7.4.5 Semantic Candidates (read-only, for JSON response)

Constants: `reviewSemanticThreshold = 0.40`, `maxSemanticCandidates = 5`, `minSemanticSimilarity = 0.10` (token-overlap fallback).

If embeddings available:
1. Compute cosine vs every other embedding, retain those with `cosSim ≥ 0.40`, sort desc, top 5.
2. Mark `auto_linked = (cosSim ≥ 0.80)`.
3. Filter out any whose insight is missing or soft-deleted (defensive: the cache may have entries pruned mid-transaction).
4. If the filtered result is non-empty, return it; else fall through to token overlap.

Token-overlap fallback: load all active insights, compute `ContentSimilarity` (§7.3) vs each, keep those `≥ 0.10`, top 5.

#### 7.4.6 Causal Candidates (read-only, for JSON response)

Constant: `maxCausalCandidates = 10`.

Run a 2-hop BFS neighborhood from the new insight (all edge types), capped at 10 nodes. Annotate each with:
- `causal_signal` = first matching causal keyword in the neighbor's content; if empty, the keyword in the new insight's content.
- `suggested_sub_type` per §7.4.3 sub-type rule on `(new.content + " " + neighbor.content)`.

#### 7.4.7 BFS Traversal Rules

Used by `mnemon related` and the candidates above.

- Pre-load all active insights and all edges into memory (avoid N+1).
- Build adjacency lists keyed by either endpoint (treat edges as undirected for traversal even though they're stored directional).
- Visit each node at most once. Skip soft-deleted / missing nodes.
- Apply `MaxDepth` (hop count from start) and optional `MaxNodes` cap and optional `EdgeFilter`.
- Result is hop-ordered (BFS layer order).

### 7.5 Intent Detection

Detect intent from a query string. Lowercase before regex. Compute three counts:

```
why    = count of matches for: \b(why|reason|because|cause|motivation|rationale)\b
                              | (为什么|原因|理由)
when   = count of matches for: \b(when|time|date|before|after|during|timeline|history|sequence)\b
                              | (什么时候|何时|时间|之前|之后)
entity = count of matches for: \b(what is|who is|tell me about|describe|about)\b
                              | (是什么|谁是|关于|介绍)
```

Decision rule (in order):
- If `why > when` and `why > entity` and `why > 0` → `WHY`.
- Else if `when > why` and `when > entity` and `when > 0` → `WHEN`.
- Else if `entity > 0` → `ENTITY`.
- Else → `GENERAL`.

Override via `--intent`: parse case-insensitively, accept `WHY|WHEN|ENTITY|GENERAL` only.

#### 7.5.1 Intent → Edge Weight Tables

```
WHY:     causal=0.70  temporal=0.20  entity=0.05  semantic=0.05
WHEN:    temporal=0.65 causal=0.15  entity=0.10  semantic=0.10
ENTITY:  entity=0.55  semantic=0.30 temporal=0.05 causal=0.10
GENERAL: temporal=0.25 semantic=0.25 causal=0.25 entity=0.25
```

Used as `φ(edgeType, intent)` in beam-search transitions.

#### 7.5.2 Intent → Traversal Parameters

```
WHY:     beamWidth=15 maxDepth=5 maxVisited=500
WHEN:    beamWidth=10 maxDepth=5 maxVisited=400
ENTITY:  beamWidth=10 maxDepth=4 maxVisited=400
GENERAL: beamWidth=10 maxDepth=4 maxVisited=500
```

### 7.6 Intent-Aware Recall (`IntentAwareRecall`)

Inputs: query string, optional query embedding, query entities (extracted via §7.2 from the query), `limit`, optional intent override.

**Step 1 — Intent**: per §7.5. `intent_source = "auto"` or `"override"`.

**Step 2 — Anchor selection (RRF)**: `RRF_K = 60`, `anchorTopK = 20`.

Compute three signal rankings:

1. **Keyword**: §7.1 keyword search over all active insights, top 20.
2. **Vector** (only if embeddings cache is non-empty): cosine vs all embedded insights, drop similarity ≤ 0.1, top 20 by similarity desc.
3. **Time**: all active insights sorted by `created_at` desc, take top 20.

For each signal, each result gets `RRF_score = 1 / (60 + rank + 1)` (rank 0-indexed). When the same insight appears in multiple signals, sum the RRF scores. Track `via`:

```
keyword only            → "keyword"
vector only             → "vector"
time only               → "time"
keyword + vector        → "hybrid"   (set the moment a keyword/vector anchor gets a second signal)
keyword + time          → "hybrid"
vector + time           → "hybrid"
all three               → "hybrid"
```

Implementation note: when a time-signal anchor lands on an existing keyword/vector anchor, set its via to `"hybrid"`; when a new time-only anchor is created, leave via as `"time"`.

Normalize: divide all anchor scores by `max(score)` so the highest-ranked anchor is `1.0`.

`anchor_count` = number of distinct anchors.

**Step 3 — Beam search from each anchor**: per §7.5.2 traversal params. Constants: `λ₁ = 1.0` (structural), `λ₂ = 0.4` (semantic).

For each anchor, run an independent beam search seeded with `(anchor.id, anchor.score, depth=0)`:

- Maintain a `scoreMap` (id → best-known score), `viaMap` (id → via label, "<edge-type>" once a node is reached via traversal), `insightMap` (id → resolved insight).
- At each depth level, expand all current beam nodes:
  - For each edge incident to the current node (treat undirected):
    - `neighborID = the other endpoint`.
    - `structural = weights[edge.edge_type] * edge.weight`.
    - `semantic = cosine(queryVec, neighborVec)` if both present, else 0.
    - `neighborScore = currentScore + λ₁ * structural + λ₂ * semantic`.
    - If `neighborScore > scoreMap[neighborID]` (or absent), update scoreMap / viaMap (via = edge.edge_type) / insightMap.
    - If neighbor not yet visited in this beam, mark visited, push onto next-level heap.
- Stop expanding when `MaxVisited` reached or beam exhausted or `MaxDepth` reached.
- After each level, prune to `BeamWidth` highest-scored nodes for the next level.

`traversed` = `len(scoreMap)` after all anchors processed.

**Step 4 — Reranking**: compute four signals per candidate, weighted-sum.

For each candidate id in `scoreMap`:
- `kwScore` = `|Q ∩ T| / |Q|` where Q is query tokens, T is the insight's `Tokenize(content) ∪ Tokenize(tags) ∪ Tokenize(entities)`.
- `entScore` = `|matched| / max(1, |queryEntities|)` where `matched` is count of insight entities (lowercased) found in the lowercased query-entity set.
- `simScore` = cosine(queryVec, insightVec) if both present and > 0, else 0.
- `graphScore` = `(graphRaw - graphMin) / (graphMax - graphMin)` (min-max normalization across the candidate set; if range is 0, use 1.0 to avoid divide-by-zero).

Weighted sum:

```
hasEmbeddings = embedCache non-empty AND queryVec available
if hasEmbeddings:
    final = 0.30*kwScore + 0.15*entScore + 0.35*simScore + 0.20*graphScore
else:
    final = 0.45*kwScore + 0.25*entScore + 0   *simScore + 0.30*graphScore
```

Sort by `final` desc; tie-break by `importance` desc. Truncate to `limit`.

**Step 5 — WHY topological sort**: if intent is `WHY`, reorder results so causes come before effects.

- Build the adjacency among the result-set IDs from `causal` edges only (`source → target` means source causes target).
- Apply Kahn's algorithm with a max-heap tiebreaker on `final` score.
- Append any remaining nodes (cycle members) at the end in original score order.

**Step 6 — Hint**: `hint = "sparse_results"` if `len(results) == 0` or (`limit > 0` and `len(results) < limit/2`); else hint is omitted from the JSON.

### 7.7 Effective Importance & Lifecycle

Constants: `HalfLifeDays = 30.0`, `MaxInsights = 1000`, `PruneBatchSize = 10`, `MaxOplogEntries = 5000`.

#### 7.7.1 Formula

```
base_weight(importance):
    5 → 1.00
    4 → 0.80
    3 → 0.50
    2 → 0.30
    other → 0.15           (treats 1 and any out-of-range as the lowest tier)

access_factor = max(1.0, log(1 + access_count))         // ln, not log10
decay_factor  = 0.5 ^ (days_since_access / 30)
edge_factor   = 1.0 + 0.1 * min(edge_count, 5)

EI = base_weight * access_factor * decay_factor * edge_factor
```

`days_since_access` is computed from `last_accessed_at` if non-null, otherwise from `created_at`. Both timestamps must be parseable as RFC 3339; on parse failure, propagate a wrapped error.

`edge_count` = `(SELECT COUNT(*) FROM edges WHERE source_id = ?) + (SELECT COUNT(*) FROM edges WHERE target_id = ?)`. Counts both directions even though most edges are bidirectional — this is intentional (the doubling effectively rewards highly-connected nodes more, and was preserved in upstream).

Implementations should hold this as one helper `compute_effective_importance(importance, access_count, days_since_access, edge_count) -> f64`.

#### 7.7.2 Immunity

```
is_immune(importance, access_count) = importance >= 4 OR access_count >= 3
```

Immune insights are **never** auto-pruned and never appear as `gc` candidates.

#### 7.7.3 RefreshEffectiveImportance(id)

Single-row recompute and persist:

1. Read `(importance, access_count, created_at, last_accessed_at)`.
2. Compute `days_since_access`.
3. Count edges (single SQL with a sum of two subqueries).
4. Compute EI per §7.7.1.
5. `UPDATE insights SET effective_importance = ? WHERE id = ?`.
6. Return EI.

#### 7.7.4 GetRetentionCandidates(threshold, limit)

For `gc` suggest mode. Pulls everything in two bulk queries (avoid N+1):

```sql
-- 1. all active insights including last_accessed_at
SELECT id, content, category, importance, tags, entities, source, access_count,
       created_at, updated_at, deleted_at, last_accessed_at
  FROM insights WHERE deleted_at IS NULL;

-- 2. edge counts grouped by node id (single query, not N)
SELECT id, SUM(cnt) FROM (
    SELECT source_id AS id, COUNT(*) AS cnt FROM edges GROUP BY source_id
    UNION ALL
    SELECT target_id AS id, COUNT(*) AS cnt FROM edges GROUP BY target_id
) GROUP BY id;
```

Then in memory:
- For each insight, compute EI per §7.7.1.
- If `EI < threshold` and not immune → add to candidates.
- Persist all recomputed EIs in one transaction (best-effort: failure logs to stderr but the returned candidates are still emitted).
- Sort candidates by EI ascending. Apply `--limit`.

Returns `(candidates, total_active_count)`.

#### 7.7.5 AutoPrune(maxInsights, excludeIDs)

Run inside the `remember` transaction (or wrap a fresh transaction if not already in one).

1. `total = SELECT COUNT(*) FROM insights WHERE deleted_at IS NULL`.
2. If `total ≤ maxInsights`, return 0.
3. `excess = min(total - maxInsights, PruneBatchSize)`.
4. Pick prune candidates:
   ```sql
   SELECT id FROM insights
    WHERE deleted_at IS NULL AND importance < 4 AND access_count < 3
      AND id NOT IN (<excludeIDs>)
    ORDER BY effective_importance ASC LIMIT <excess>
   ```
5. For each id: soft-delete (set `deleted_at`, `updated_at`) + delete its edges. Return count actually pruned.

#### 7.7.6 BoostRetention (used by `gc --keep`)

```sql
UPDATE insights
   SET access_count = access_count + 3,
       last_accessed_at = ?,
       updated_at = ?
 WHERE id = ? AND deleted_at IS NULL
```

Then `RefreshEffectiveImportance(id)`.

---

## 8. Embedding Subsystem (Optional)

The embedding subsystem is **optional**. The binary must function fully without it. Detection is at runtime, per-invocation, with no global "Ollama mode" flag.

### 8.1 Configuration

Environment variables:

| Var | Default |
|---|---|
| `MNEMON_EMBED_ENDPOINT` | `http://localhost:11434` |
| `MNEMON_EMBED_MODEL` | `nomic-embed-text` |
| `MNEMON_EMBED_DIMENSIONS` | unset (use model native) |

### 8.2 Availability Probe

```
GET <endpoint>/api/tags
Timeout: 2 seconds (request + body)
Available iff status code is 200.
```

The probe runs at the start of `remember`, `recall`, and `embed --status`. The 2-second cap prevents an unresponsive Ollama from blocking the CLI.

### 8.3 Embed Request

```
POST <endpoint>/api/embed
Content-Type: application/json
Body: { "model": "<model>", "input": "<text>", "dimensions": <int>? }
```

`dimensions` is included only if `MNEMON_EMBED_DIMENSIONS` is set to a positive integer (used for Matryoshka models).

Response:
```
200 OK
{ "embeddings": [[<float>, <float>, ...]] }
```

Take `embeddings[0]`. If the array is empty or the inner vector is empty, error `"empty embedding returned"`.

Timeouts: 5 s connect, 30 s overall request. Disable HTTP proxy resolution (this is a localhost call by default — system proxies must not be applied or the loopback connection will be misrouted).

### 8.4 Optional, Not Required

When Ollama is unavailable:
- `remember` proceeds without embedding (no error). The new row has NULL `embedding`.
- `recall` runs without `simScore`; rerank weights redistribute (§7.6 step 4).
- `mnemon embed <id>` and `mnemon embed --all` error out with `Ollama not available at <endpoint> — install with: brew install ollama && ollama pull <model>`.
- `mnemon embed --status` reports `ollama_available: false` but still emits the rest of the stats.

---

## 9. Store Resolution

```
Effective store name:
    --store <name>           (CLI flag, highest priority)
  > $MNEMON_STORE             (environment variable)
  > <base>/active             (file contents, trimmed)
  > "default"                 (fallback)
```

Used for every command that touches a store DB. `mnemon store list` separately checks which stores exist by enumerating `<base>/data/`.

**Concurrency model**: per-process. Different processes can use different stores via `MNEMON_STORE` simultaneously without contention. SQLite WAL allows concurrent readers but only one writer at a time within a single DB file — so two writers against the *same* store will serialize at the OS level.

---

## 10. Oplog & Observability

### 10.1 Oplog Operations

The following operations are logged to the `oplog` table:

| Operation | When | Detail field |
|---|---|---|
| `remember` | New insight inserted | `<insight content>` |
| `diff-skip` | DUPLICATE skipped | `duplicate of <existing-id>` |
| `diff-replace` | Old insight soft-deleted on UPDATE/CONFLICT | `replaced by <new-id>` |
| `recall` | Smart recall completed | `q=<query> hits=<int>` |
| `recall:basic` | Basic recall completed | `q=<query> hits=<int>` |
| `search` | Token search completed | `q=<query> hits=<int>` |
| `link` | Edge created via CLI | `<src8>→<tgt8> type=<t> weight=<w>` |
| `forget` | Soft-delete via CLI | empty |
| `gc` | Suggest-mode listing | `threshold=<f> found=<n> total=<n>` |
| `gc_keep` | `--keep` invocation | `<insight content>` |
| `embed` | Single-id embedding | `dim=<n> model=<name>` |
| `embed:backfill` | `--all` completion | `succeeded=<n> failed=<n> model=<name>` |

Oplog writes are **best-effort**: log failures emit a warning to stderr but do not propagate.

### 10.2 Trim Policy

After each `INSERT INTO oplog`, run:

```sql
DELETE FROM oplog WHERE id <= (SELECT MAX(id) FROM oplog) - 5000
```

This keeps at most ~5000 entries (using monotonic AUTOINCREMENT ids — gaps after deletes are fine). Common case is a no-op when count is below the cap.

### 10.3 `mnemon log` Output

Tabular, see §6.2 `log`.

---

## 11. Visualization Output

`mnemon viz --format dot` and `--format html`.

### 11.1 Common Rules

- Pre-load all active insights and all edges.
- Filter edges where either endpoint is soft-deleted or absent.
- **Node label**: short content (truncated to 60 chars + `...` if longer).
- **Tooltip / title** (HTML only): full content with `\n` escaped to literal `\\n` in JSON.

### 11.2 Color Tables

```
Node colors (by category):
  decision    #e74c3c
  fact        #3498db
  insight     #9b59b6
  preference  #2ecc71
  context     #f39c12
  general/other #95a5a6

Edge colors (by edge_type):
  temporal  #aaaaaa
  semantic  #3498db
  causal    #e74c3c
  entity    #2ecc71
  fallback  #cccccc
```

### 11.3 DOT Output

```
digraph mnemon {
  rankdir=LR;
  node [shape=box, style="filled,rounded", fontsize=10, fontname="Helvetica"];
  edge [fontsize=8, fontname="Helvetica"];

  "<full-id>" [label="<short-id8>: [<category>] <content...>", fillcolor="<hex>", fontcolor="white"];
  ...
  "<src>" -> "<tgt>" [label="<sub_type or edge_type>", color="<hex>", fontcolor="<hex>"];
  ...
}
```

Edge label = `metadata.sub_type` if present, else `edge_type`. Quote escaping: replace `"` with `\"` in node labels before quoting; replace `\n` with space.

### 11.4 HTML Output

A self-contained HTML file using vis-network from CDN:

```html
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>Mnemon Knowledge Graph</title>
<script src="https://unpkg.com/vis-network/standalone/umd/vis-network.min.js"></script>
<style>
  body { margin: 0; padding: 0; background: #1a1a2e; font-family: sans-serif; }
  #graph { width: 100vw; height: 100vh; }
  #legend { position: fixed; top: 10px; right: 10px; background: rgba(0,0,0,0.7);
    color: white; padding: 12px; border-radius: 8px; font-size: 12px; }
  .leg-item { display: flex; align-items: center; margin: 4px 0; }
  .leg-dot { width: 12px; height: 12px; border-radius: 50%; margin-right: 8px; }
  .leg-line { width: 20px; height: 3px; margin-right: 8px; }
</style>
</head>
<body>
<div id="graph"></div>
<div id="legend"> /* node + edge legend with the §11.2 colors */ </div>
<script>
var nodes = new vis.DataSet([ /* {id,label,title,color,font:{color:"white"}} per node */ ]);
var edges = new vis.DataSet([ /* {from,to,label,color:{color},arrows:"to",font:{color,size:10}} per edge */ ]);
var container = document.getElementById("graph");
var data = { nodes: nodes, edges: edges };
var options = {
  physics: { solver: "forceAtlas2Based", forceAtlas2Based: { gravitationalConstant: -30 } },
  interaction: { hover: true, tooltipDelay: 100 },
  nodes: { shape: "box", margin: 8, borderWidth: 0, font: { size: 11 } },
  edges: { smooth: { type: "continuous" }, font: { size: 9 } }
};
new vis.Network(container, data, options);
</script>
</body>
</html>
```

Use JSON-encoded string literals (`JSON.stringify`-equivalent) for every value embedded in JS, to safely escape quotes, backslashes, and newlines.

### 11.5 Output Sink

If `-o`/`--output` is `""` or `"-"` → write to stdout. Otherwise write to the named file with mode `0644`, then write `written to <path>\n` to stderr.

---

## 12. Hook & Setup Behavior

`mnemon setup` integrates with two host environments: **Claude Code** and **OpenClaw**. NanoClaw is integrated via a separate skill in the NanoClaw repo and not driven by `mnemon setup` directly (its skill content is included in the embedded assets but is consumed by the user, not deployed by this binary).

### 12.1 Asset Corpus (verbatim copy from upstream)

The implementation embeds these files **byte-for-byte** from `setup_assets/` (kept identical to the reference monorepo’s `internal/setup/assets/`):

```
assets/
├── claude/
│   ├── SKILL.md
│   ├── guide.md
│   ├── prime.sh         (chmod 0755)
│   ├── user_prompt.sh   (chmod 0755)
│   ├── stop.sh          (chmod 0755)
│   └── compact.sh       (chmod 0755)
├── openclaw/
│   ├── SKILL.md
│   ├── hooks/mnemon-prime/
│   │   ├── HOOK.md
│   │   └── handler.js
│   └── plugin/
│       ├── index.js
│       ├── package.json
│       └── openclaw.plugin.json
└── nanoclaw/
    ├── SKILL.md
    └── container-skill.md
```

Implementation freedom: how to embed (Rust `include_bytes!`, C++ resource files / `xxd -i` / `std::embed` per C++26, or a dedicated resource compiler) is up to the implementer. The bytes themselves must be identical.

### 12.2 Detection

`DetectEnvironments(global)` returns a list of environments with `{name, display, version, configDir, detected}`:

- **claude-code**:
  - `name = "claude-code"`, `display = "Claude Code"`.
  - Detection: `claude` is on `$PATH`. Version: `claude --version` (best-effort).
  - `configDir`: `~/.claude` if `--global` else `.claude` (project-local).
- **openclaw**:
  - `name = "openclaw"`, `display = "OpenClaw"`.
  - Detection: presence of either `~/.openclaw/` directory or `openclaw` binary on `$PATH`.
  - `configDir`: `~/.openclaw` if `--global` else `.openclaw`.

### 12.3 Install — Claude Code

Phases:

1. **Skill**: write `<configDir>/skills/mnemon/SKILL.md` (from `assets/claude/SKILL.md`).
2. **Prompts**: write `~/.mnemon/prompt/guide.md` (from `assets/claude/guide.md`) and `~/.mnemon/prompt/skill.md` (from `assets/claude/SKILL.md`).
3. **Mandatory hook**: write `<configDir>/hooks/mnemon/prime.sh` (chmod 0755).
4. **Optional hooks** (interactive multi-select; defaults `Remind=true, Nudge=true, Compact=false`):
   - `Remind` → `<configDir>/hooks/mnemon/user_prompt.sh`
   - `Nudge` → `<configDir>/hooks/mnemon/stop.sh`
   - `Compact` → `<configDir>/hooks/mnemon/compact.sh`
5. **Register hooks** in `<configDir>/settings.json`. The exact JSON block injected is whatever the upstream `setup.ClaudeRegisterHooks` writes — implementations should reproduce it byte-equivalent. (Per the drop-in contract: read upstream's emitted `settings.json` from a clean install and mirror the JSON shape, key order is not contractual but value content is.)
6. **Init default store**: run §3.3 migration, ensure `<base>/data/default/` exists with an initialized DB.
7. Print summary: `Setup complete!`, the installed hook list, the prompt path, hint about `mnemon setup --eject`.

### 12.4 Install — OpenClaw

Phases:

1. **Skill**: `<configDir>/skills/mnemon/SKILL.md` from `assets/openclaw/SKILL.md`.
2. **Prompts**: same as Claude (§12.3 phase 2).
3. **Internal hook**: `<configDir>/hooks/mnemon-prime/{HOOK.md, handler.js}` from `assets/openclaw/hooks/mnemon-prime/`.
4. **Plugin**: `<configDir>/extensions/mnemon/{index.js, package.json, openclaw.plugin.json}` from `assets/openclaw/plugin/`. The `openclaw.plugin.json` should have its `version` field rewritten to match the binary's reported version.
5. **Register plugin** in `<configDir>/openclaw.config.json` (or equivalent — match upstream).
6. **Init default store**.
7. Print summary.

### 12.5 Eject

`--eject` removes everything `setup` installed for the selected environment(s):

- Delete `<configDir>/skills/mnemon/`, `<configDir>/hooks/mnemon/` (or `mnemon-prime` for OpenClaw), `<configDir>/extensions/mnemon/` (OpenClaw).
- Remove the mnemon hook block from `<configDir>/settings.json` / `openclaw.config.json` without disturbing other entries.
- Optionally (interactive prompt, default yes) remove the memory-guidance block from `./CLAUDE.md` (Claude Code) or `./AGENTS.md` (OpenClaw). The block is delimited by markdown markers that the install path inserts (match upstream `EjectMemoryBlock`).

`~/.mnemon/` is **not** touched by eject — user data is preserved.

### 12.6 Interactive vs Non-Interactive

- `--yes` accepts all defaults, makes no prompts.
- `--target <name>` skips environment selection.
- TTY detection (e.g. `isatty(stderr)` in C / `IsTerminal` in Go / `atty` crate in Rust) determines whether to render interactive selectors.
- Non-interactive without `--target` and without `--yes`: install all detected environments.

### 12.7 Console Output Conventions

Status lines use a consistent prefix/glyph scheme. The upstream `setup` package prints:

```
Detecting LLM CLI environments...

  ✔  Claude Code <version>           (~/.claude)
  ✘  OpenClaw                        (not detected)

Setting up Claude Code (.claude)...

[1/3] Skill
  ✔  Skill           .claude/skills/mnemon/SKILL.md

[2/3] Prompts
  ✔  Prompts         ~/.mnemon/prompt/

[3/3] Optional hooks
...

Setup complete!
  Hooks   prime, remind, nudge
  Prompts ~/.mnemon/prompt/ (guide.md, skill.md)

Start a new Claude Code session to activate.
Edit ~/.mnemon/prompt/guide.md to customize behavior.
Run 'mnemon setup --eject' to remove.
```

Exact glyphs and line-breaking are not contractually pinned (the `e2e_test.sh` does not parse setup output), but a clean readable format is expected.

---

## 13. Conformance Test

`scripts/e2e_test.sh` is the authoritative behavioral test. It:

- Builds the binary and exercises every command.
- Asserts JSON shapes via `jq`.
- Tests dedup, intent-aware recall, edge auto-creation, `gc`, `store` management, soft-delete, `viz`, `log`, `status`, `embed --status`.
- Uses an isolated `.testdata/` directory.

The implementation passes if `bash scripts/e2e_test.sh` exits 0 with `FAIL=0` (optional: `MNEMON_TEST_BINARY=/path/to/mnemon` to skip the default CMake build).

The implementer **may** add a language-native test suite, but the e2e harness is the cross-implementation checkpoint.

---

## 14. Implementation Guidance

### 14.1 Concurrency & Transactions

- One process holds **one** SQLite connection. SQLite is a single-writer engine; multiple connections from one process produce lock contention without parallelism.
- A `remember` transaction wraps: soft-delete (if replacing), insert insight, write embedding blob, run all edge generators, refresh effective importance, auto-prune. **The Ollama HTTP call must happen *before* `BEGIN`.** Holding a transaction across an external HTTP call risks WAL bloat and lock contention if Ollama is slow.
- The "candidates" computation for the JSON response (semantic + causal) runs *after* `COMMIT` — read-only, cheap to recompute.
- The embedding cache passed into the edge engine is mutable (the new insight's vector is added before `CreateSemanticEdges`, and any replaced insight's vector is removed). On rollback, discard the cache: it has been mutated to reflect a state that didn't commit.

### 14.2 N+1 Avoidance

The reference implementation eliminates several N+1 patterns. Match these:

- **Recall** pre-loads all embeddings once into a `HashMap<String, Vec<f64>>` instead of fetching per beam-search hop.
- **GC suggest** uses one `SELECT` for all insights (with `last_accessed_at`) and one grouped `SELECT` for edge counts, then computes EI in memory and persists in one transaction.
- **BFS** pre-loads all active insights and all edges into adjacency-lists.
- **Keyword search** computes a per-insight token set once, optionally caches it for reuse during reranking.

### 14.3 Binary Layout (single static binary)

- The reference uses `modernc.org/sqlite` (pure-Go SQLite, no CGO) so the binary has zero runtime deps.
- Rust: prefer `rusqlite` with the `bundled` feature (statically links sqlite3) for the same property.
- C++: vendor `sqlite3.c` directly, or use a well-known wrapper (`sqlite_modern_cpp`) atop a bundled amalgamation. Avoid system sqlite3 unless distributing per-platform packages.

### 14.4 Language Sidebars

#### Rust

- **Errors**: `thiserror` for typed errors per module + `anyhow::Result` at command boundaries works well. Match the upstream `fmt.Errorf("context: %w", err)` style with `.context("...")` from `anyhow` or `.map_err(|e| MyError::Wrapped(e))`.
- **CLI**: `clap` with derive macros covers cobra's surface cleanly. Replicate flag defaults, alias rules, and the hidden `--smart` flag (Cobra's `MarkHidden`).
- **JSON**: `serde` + `serde_json` with `#[serde(rename = "...")]` for Go-style snake_case keys. Preserve **two-space indent + trailing newline** to match the Go encoder.
- **SQLite**: `rusqlite` (synchronous, matches the single-connection model) or `sqlx` (async — more complex, only worth it if you have a reason). For drop-in fidelity, sync `rusqlite` is the natural fit.
- **HTTP**: `ureq` (sync, no Tokio runtime, smaller binary) is a closer match to the Go `net/http.Client` than `reqwest`.
- **UUID**: `uuid` crate, `Uuid::new_v4().to_string()` produces the exact lowercase hyphenated format.
- **Embed**: `include_bytes!` macro for the asset corpus; `include_dir` crate if you want directory tree embedding.

#### C++ (C++20 / C++23)

- **Errors**: `std::expected<T, Error>` (C++23) or `tl::expected` (C++20 backport) at internal boundaries; `std::system_error` / typed exceptions at the top level. Match upstream's "wrap every error with context" by carrying a string message.
- **CLI**: `CLI11` is the closest analog to Cobra (subcommands, flags, defaults, validation). `cxxopts` works for simpler cases. Hidden flags require `->group("")` in CLI11.
- **JSON**: `nlohmann/json` is the de-facto choice; configure to indent with two spaces + trailing newline for byte-equivalent output.
- **SQLite**: bundle `sqlite3.c` or use `sqlite_modern_cpp`. RAII the connection and prepared statements.
- **HTTP**: `cpp-httplib` (header-only, sync) for the Ollama call. Avoid pulling in libcurl unless you need it elsewhere.
- **UUID**: `stduuid` or hand-rolled with `<random>` + careful formatting; verify the output is RFC 4122 v4 lowercase hyphenated.
- **Embed**: C++23 `#embed` if your toolchain supports it; otherwise CMake `configure_file` + `xxd -i`-style codegen at build time. Some teams use a small Python script to emit a `.cpp` from each asset.

### 14.5 Things That Look Like Cleanups But Aren't

A few apparent oddities are intentional and load-bearing:

- **`access_factor = max(1, log(1 + access_count))`** — the `max(1, ...)` is the baseline for new/0-access insights. Removing it makes EI collapse to 0 for new rows.
- **`edge_count` doubles bidirectional edges** — `(SELECT ... source) + (SELECT ... target)` counts each direction independently. Capped at 5 in the formula. The reference implementation chose this; preserve it.
- **`keyword + time → "hybrid"` but `time-only → "time"`** — the via label "time" should appear only when time is the sole signal.
- **Best-effort oplog** — failures must not propagate. The user-visible operation should not fail because logging failed.
- **Best-effort batch EI update in GC** — the returned candidates already have EI computed in memory; rolling back the batch update and warning to stderr is the correct behavior.
- **`mnemon embed --all` continues past row failures** — never aborts the whole backfill on one row.
- **HTTP proxy disabled on the Ollama client** — many users have `HTTP_PROXY` set; for `localhost:11434` that is a misroute. The Go client sets `Proxy: nil`; reproduce that.

---

## 15. Out of Scope / Non-Goals

- New edge types beyond the four pinned in §5.3.
- New categories beyond §5.2.
- A stable internal API or library bindings — the contract is the CLI.
- Replacing SQLite as the storage engine.
- Authentication, multi-user, or network-exposed daemons. This binary is a local CLI.
- Re-deriving the embedded asset bytes. They are a fixed corpus copied from upstream.
- Prompts that depend on a particular host LLM beyond Claude Code / OpenClaw / NanoClaw integration patterns.
- Cross-store queries or any feature that would turn this into a multi-tenant system.

---

## Appendix A — JSON Output Sample (`mnemon remember`)

For a fresh database receiving `mnemon remember "Chose Qdrant over Milvus" --cat decision --imp 5 --entities Qdrant,Milvus`:

```json
{
  "id": "f47ac10b-58cc-4372-a567-0e02b2c3d479",
  "content": "Chose Qdrant over Milvus",
  "category": "decision",
  "importance": 5,
  "tags": [],
  "entities": ["Qdrant", "Milvus"],
  "action": "added",
  "diff_suggestion": "ADD",
  "created_at": "2026-05-10T12:34:56Z",
  "edges_created": {
    "temporal": 0,
    "entity":   0,
    "causal":   0,
    "semantic": 0
  },
  "semantic_candidates": [],
  "causal_candidates": [],
  "embedded": false,
  "effective_importance": 1.0,
  "auto_pruned": 0
}
```

## Appendix B — Reference Constants Quick-Reference

```
HalfLifeDays                = 30.0
MaxInsights                 = 1000
PruneBatchSize              = 10
MaxOplogEntries             = 5000
MaxContentChars             = 8000
MaxTagChars                 = 100
MaxTagsPerInsight           = 20
MaxEntityChars              = 200
MaxEntitiesPerInsight       = 50

temporalWindowHours         = 24.0
maxProximityEdges           = 10

maxEntityLinks              = 5         (per-entity neighbor cap)
maxTotalEntityEdges         = 50

minCausalOverlap            = 0.15
causalLookback              = 10

minSemanticSimilarity       = 0.10      (token-overlap fallback)
reviewSemanticThreshold     = 0.40
autoSemanticThreshold       = 0.80
maxSemanticCandidates       = 5
maxAutoSemanticEdges        = 3

maxCausalCandidates         = 10

anchorTopK                  = 20        (per-signal RRF cap)
RRF_K                       = 60
λ₁ (structural)             = 1.0
λ₂ (semantic)               = 0.4
vector-search threshold     = 0.1       (drop hits with cos ≤ 0.1)

rerank weights with embed   = kw 0.30, ent 0.15, sim 0.35, graph 0.20
rerank weights no embed     = kw 0.45, ent 0.25,           graph 0.30

per-intent traversal:
  WHY     beam 15  depth 5  visited 500
  WHEN    beam 10  depth 5  visited 400
  ENTITY  beam 10  depth 4  visited 400
  GENERAL beam 10  depth 4  visited 500

immune predicate            = importance ≥ 4 OR access_count ≥ 3
base_weight (importance)    = 5→1.00, 4→0.80, 3→0.50, 2→0.30, else→0.15

dedup negation list (en+zh) = §7.3
stopwords (en)              = §7.1.1
acronym stopwords           = §7.2.2
tech dictionary             = §7.2.3
intent edge weights         = §7.5.1
intent regex sets           = §7.5
causal regex                = §7.4.3
sub-type regex              = §7.4.3 (prevents/enables/causes)
entity regex (6 patterns)   = §7.2.1

UUID                        = RFC 4122 v4, lowercase hyphenated
Timestamps                  = RFC 3339 UTC
Embedding blob              = little-endian f64 sequence
Default Ollama endpoint     = http://localhost:11434
Default Ollama model        = nomic-embed-text
Probe timeout               = 2 s
Embed request timeout       = 30 s overall, 5 s connect
```
