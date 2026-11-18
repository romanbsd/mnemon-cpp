import { createHash } from "node:crypto";
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const dir = dirname(fileURLToPath(import.meta.url));
const corpus = JSON.parse(readFileSync(join(dir, "corpus.json"), "utf8")) as {
  memories: { content: string }[];
};
const queries = JSON.parse(readFileSync(join(dir, "queries.json"), "utf8")) as {
  queries: { query: string }[];
};

const clusters: Array<{ match: (text: string) => boolean; base: number[] }> = [
  { match: (t) => /dark mode|dim interface|color theme|theme preference|editor color|prefer dark/i.test(t), base: [1, 0, 0, 0] },
  { match: (t) => /coffee|roast/i.test(t), base: [0.2, 0.9, 0, 0] },
  { match: (t) => /deploy|rollback|checkout/i.test(t), base: [0, 1, 0, 0] },
  { match: (t) => /storage layer|parameterized SQL|talk to postgres/i.test(t), base: [0.1, 0.1, 0.2, 0.9] },
  { match: (t) => /PostgreSQL|pgvector|postgres/i.test(t), base: [0, 0, 1, 0] },
  { match: (t) => /TypeScript|Vitest|tsconfig/i.test(t), base: [0, 0, 0, 1] },
  { match: (t) => /qzxtoken/i.test(t), base: [0.5, 0.5, 0.5, 0.5] },
  { match: (t) => /Ollama|embedding|network/i.test(t), base: [0.7, 0, 0.7, 0] },
  { match: (t) => /beam|traverse|graph/i.test(t), base: [0, 0.6, 0, 0.8] },
  { match: (t) => /duplicate|hash/i.test(t), base: [0.8, 0, 0, 0.6] },
];

function vectorFor(text: string): number[] {
  const cluster = clusters.find((c) => c.match(text));
  const hash = createHash("sha256").update(text).digest();
  const jitter = [0, 1, 2, 3].map((i) => ((hash[i] ?? 0) / 255) * 0.05);
  const base = cluster?.base ?? [0, 1, 2, 3].map((i) => (hash[i + 4] ?? 0) / 255);
  const v = base.map((n, i) => n + jitter[i]!);
  const norm = Math.hypot(...v) || 1;
  return v.map((n) => n / norm);
}

const table: Record<string, number[]> = {};
for (const m of corpus.memories) {
  table[m.content] = vectorFor(m.content);
  table[`document:${m.content}`] = table[m.content]!;
}
for (const q of queries.queries) {
  table[q.query] = vectorFor(q.query);
  table[`query:${q.query}`] = table[q.query]!;
  table[`document:${q.query}`] = table[q.query]!;
}

writeFileSync(join(dir, "embeddings.json"), `${JSON.stringify(table, null, 2)}\n`);
