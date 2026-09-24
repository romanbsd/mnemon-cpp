# mnemon

In-process memory library for TypeScript agents. Stores insights, edges, and
embeddings in PostgreSQL with pgvector. No CLI, no SQLite, no network except
the embedding provider you inject.

```ts
import { createMnemon, LlamaCppEmbeddingProvider } from "mnemon";

const mnemon = createMnemon({
  databaseUrl: process.env.MNEMON_DATABASE_URL,
  embeddingProvider: new LlamaCppEmbeddingProvider(),
});

await mnemon.remember({ content: "Prefer TypeScript for new services." });
const { results } = await mnemon.recall({ query: "language preference" });
const brief = await mnemon.recall({ query: "language preference", brief: true });
await mnemon.close();
```

Requires Node.js 22+ and PostgreSQL with the `vector` extension.

```bash
npm install
npm run db:up          # postgres:18-alpine + pgvector on :55432
npm run embed:up       # llama.cpp + nomic-embed-text-v1.5 (768-d) on :8080
export MNEMON_DATABASE_URL=postgresql://mnemon:mnemon@127.0.0.1:55432/mnemon
export MNEMON_EMBED_ENDPOINT=http://127.0.0.1:8080
npm test
```

`nomic-embed-text-v1.5` is downloaded from Hugging Face on first start and cached in the `llama-models` volume. Document texts are prefixed with `search_document: `, queries with `search_query: `. `OpenAIEmbeddingProvider` talks to any OpenAI-compatible `/v1/embeddings` server (optional `MNEMON_EMBED_API_KEY`). `recall({ brief: true })` flattens whitespace and truncates content to 240 code points; full text remains available via `get(id)`.

Comparison against the Go (and optional C++) CLI is `npm run test:parity`. That harness disables embeddings and uses wall-clock timestamps because the CLIs cannot ingest fixture vectors or `createdAt`. Architectural mismatches — no auto-replace, no auto-prune — are documented in [`docs/parity.md`](docs/parity.md). Only forgotten-row leaks, dropped extensions/negations, and exact-dup misses fail the suite.

## Public API

Everything below is exported from `"mnemon"`.

### `createMnemon(config): Mnemon`

Builds a client. Pass exactly one of `databaseUrl` or `pool`. Schema migrations
run on first use (`initialize` is also public if you want to fail fast).

| Option | Default | Notes |
|---|---|---|
| `databaseUrl` | — | PostgreSQL connection string |
| `pool` | — | Existing `pg.Pool` (caller owns lifecycle) |
| `schema` | `"mnemon"` | Must match `^[a-z_][a-z0-9_]*$` |
| `embeddingProvider` | none | Keyword-only recall/search if omitted |
| `embeddingDimensions` | provider's | Must match the provider when both are set |
| `clock` | system clock | `{ now(): Date }` for tests |
| `defaults.category` | `"general"` | `preference` · `decision` · `fact` · `insight` · `context` · `general` |
| `defaults.importance` | `3` | 1–5 |
| `defaults.source` | `"agent"` | |
| `defaults.recallLimit` | `10` | Max 100 |
| `limits.activeInsightSoftLimit` | `5000` | Informational; v1 does not auto-prune |
| `limits.maxRecallCandidates` | `500` | 50–5000 |

### Instance methods

`initialize()` is invoked automatically by every other method. After `close()`,
further calls throw `MnemonConfigurationError`.

#### `initialize(): Promise<void>`

Runs schema migrations and checks that a stored embedding model/dimension
matches the injected provider.

#### `remember(input): Promise<RememberResult>`

Inserts an insight and generated edges. Exact content-hash matches are skipped.
`deduplicate` (default `true`) also skips conservative near-duplicates. Returns
a Go-compatible Diff `suggestion` (`ADD` / `UPDATE` / `CONFLICT` / `DUPLICATE`)
but never auto-replaces; call `forget` on `diff[0].id` if you want that.

```ts
await mnemon.remember({
  content: "Ship behind a feature flag.",
  category: "decision",
  importance: 4,
  tags: ["release"],
  entities: ["feature-flag"],
  source: "agent",
  createdAt: "2026-09-03T00:00:00Z", // optional; must include a timezone
  deduplicate: true,
});
```

Result: `{ action: "added" | "skipped", insight, suggestion, diff, semanticCandidates, edgeCounts, duplicateOf? }`.

#### `recall(input): Promise<RecallResult>`

Graph-expanded retrieval. Anchors are keyword / vector / time; expansion is a
per-anchor beam scored in PostgreSQL. Intent is auto-detected unless overridden
(`WHY` · `WHEN` · `ENTITY` · `GENERAL`).

```ts
await mnemon.recall({
  query: "why did we choose TypeScript?",
  limit: 10,
  intent: "WHY",
  source: "agent",
  brief: true,
  excerptChars: 240,
});
```

Result: `{ results: RecallHit[], meta }`. Each hit has `insight`, `score`,
`intent`, `matchedVia`, `signals` (`keyword` / `entity` / `similarity` /
`graph`), and `excerpt` when `brief` is true. Sparse result sets set
`meta.hint` to `"sparse_results"`.

#### `search(input): Promise<SearchResult>`

Browse ranking: English `tsvector` plus stored token overlap. FTS is a browse
signal only — it is not a recall anchor.

```ts
await mnemon.search({ query: "feature flag", limit: 20, source: "agent" });
```

Result: `{ results: SearchHit[] }` with `matchedVia` of `keyword` · `fts` ·
`hybrid`. Default limit 20, max 100.

#### `list(input?): Promise<Insight[]>`

Active insights by `created_at` descending.

```ts
await mnemon.list({
  limit: 20,
  source: "agent",
  category: "decision",
  since: "2026-01-01T00:00:00Z",
  until: "2026-12-31T23:59:59Z",
});
```

Default limit 20, max 100.

#### `get(id): Promise<Insight | null>`

Active insight by UUID, or `null`. Soft-deleted rows are not returned.

#### `related(id, options?): Promise<RelatedInsight[]>`

BFS from an insight. Throws `MnemonNotFoundError` if `id` is missing.

```ts
await mnemon.related(id, { maxDepth: 2, limit: 20, edgeType: "causal" });
```

Defaults: depth 2 (max 5), limit 20 (max 100). Each row is an `Insight` plus
`depth` and optional `viaEdgeType`.

#### `link(input): Promise<Edge>`

Manual edge between two distinct UUIDs. Types: `temporal` · `semantic` ·
`causal` · `entity`. Weight defaults to `1` (0–1).

```ts
await mnemon.link({
  sourceId,
  targetId,
  edgeType: "causal",
  weight: 0.8,
  metadata: { reason: "decision followed the incident" },
});
```

#### `forget(id): Promise<ForgetResult>`

Soft-deletes an insight. `{ forgotten: true, id }` if it was active;
`forgotten: false` if already gone.

#### `log(input?): Promise<OpLogEntry[]>`

Durable oplog, newest first.

```ts
await mnemon.log({ limit: 50, operation: "remember" });
```

Default limit 50, max 500. Entries: `{ id, operation, insightId?, detail, createdAt }`.

#### `status(): Promise<MnemonStatus>`

Store counts and embedding settings:

`{ schema, algorithmVersion, insights, embeddings, edges, embeddingModel?, embeddingDimensions? }`.

#### `close(): Promise<void>`

Marks the client closed. Ends the pool only when `createMnemon` created it.

### Embedding providers

Implement `EmbeddingProvider` (`model`, `dimensions`,
`embed(text, purpose)`) or use a built-in HTTP client. Purpose is `"document"`
on write and `"query"` on recall.

| Class | Protocol | Default endpoint | Default dimensions |
|---|---|---|---|
| `LlamaCppEmbeddingProvider` | llama.cpp `/v1/embeddings` | `http://127.0.0.1:8080` | 768 (`NOMIC_EMBED_TEXT_DIMENSIONS`) |
| `OpenAIEmbeddingProvider` | OpenAI `/v1/embeddings` | `http://127.0.0.1:8080` | 1536 |
| `OllamaEmbeddingProvider` | Ollama `/api/embed` | `http://127.0.0.1:11434` | 768 |
| `HttpEmbeddingProvider` | any of the above via `protocol` | llama.cpp | 768 |

Shared options: `endpoint`, `model`, `dimensions`, `apiKey`. Env fallbacks:
`MNEMON_EMBED_ENDPOINT`, `MNEMON_EMBED_MODEL`, `MNEMON_EMBED_DIMENSIONS`,
`MNEMON_EMBED_API_KEY`.

### `makeBriefExcerpt(content, maxChars): string`

Flattens whitespace and truncates on a Unicode code-point boundary with an
ellipsis. Same projection as `recall({ brief: true })`.

### Errors

All extend `MnemonError`:

| Class | When |
|---|---|
| `MnemonValidationError` | Bad input (`field`, `code`) |
| `MnemonConfigurationError` | Config / closed client / provider mismatch |
| `MnemonDatabaseError` | PostgreSQL failure |
| `MnemonEmbeddingError` | Provider HTTP or shape failure |
| `MnemonNotFoundError` | `related` on a missing id |
