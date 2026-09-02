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

`search` ranks by English `tsvector` plus stored token overlap. `list` returns active insights by `created_at` (optional source, category, since/until). `status` and `log` expose store counts and the durable oplog. Recall graph expansion is a per-anchor beam scored in PostgreSQL. `remember` returns a Go-compatible Diff `suggestion` (`ADD` / `UPDATE` / `CONFLICT` / `DUPLICATE`) but never auto-replaces; call `forget` on `diff[0].id` if you want that.
