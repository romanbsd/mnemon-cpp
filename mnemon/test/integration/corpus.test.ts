import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

import { FakeClock } from "../fake-clock.js";
import { FakeEmbeddingProvider } from "../fake-embedding-provider.js";
import { postgresAvailable, withMnemon } from "./helpers.js";

const dir = dirname(fileURLToPath(import.meta.url));
const fixtureDir = join(dir, "../fixtures");
const corpus = JSON.parse(readFileSync(join(fixtureDir, "corpus.json"), "utf8")) as {
  memories: Array<{
    key: string;
    content: string;
    category?: "preference" | "decision" | "fact" | "insight" | "context" | "general";
    source?: string;
    createdAt: string;
  }>;
};
const queries = JSON.parse(readFileSync(join(fixtureDir, "queries.json"), "utf8")) as {
  queries: Array<{
    id: string;
    query: string;
    intent?: "WHY" | "WHEN" | "ENTITY" | "GENERAL";
    expected: Array<{ key: string; maxRank: number }>;
    forbidden?: string[];
    mandatory?: boolean;
  }>;
};
const embeddings = JSON.parse(readFileSync(join(fixtureDir, "embeddings.json"), "utf8")) as Record<
  string,
  number[]
>;

const available = await postgresAvailable();

describe.skipIf(!available)("deterministic corpus", () => {
  it("meets the v1 quality gates", async () => {
    const clock = new FakeClock(new Date("2024-06-01T00:00:00Z"));
    const provider = new FakeEmbeddingProvider("fixture", 4, embeddings);
    await withMnemon({ clock, embeddingProvider: provider }, async (mnemon) => {
      const keyToId = new Map<string, string>();
      const idToKey = new Map<string, string>();
      for (const memory of corpus.memories) {
        clock.set(new Date(memory.createdAt));
        const result = await mnemon.remember({
          content: memory.content,
          category: memory.category,
          source: memory.source,
          createdAt: memory.createdAt,
        });
        if (memory.key === "dark-mode-dup") {
          expect(result.action).toBe("skipped");
          keyToId.set(memory.key, result.insight.id);
          continue;
        }
        expect(result.action).toBe("added");
        keyToId.set(memory.key, result.insight.id);
        idToKey.set(result.insight.id, memory.key);
      }

      const forgotten = keyToId.get("soft-delete-me");
      if (forgotten) {
        await mnemon.forget(forgotten);
      }

      let expectedHits = 0;
      let top5Hits = 0;
      for (const q of queries.queries) {
        const recalled = await mnemon.recall({ query: q.query, intent: q.intent, limit: 10 });
        const keys = recalled.results.map((r) => idToKey.get(r.insight.id));
        expect(keys).not.toContain("soft-delete-me");
        for (const exp of q.expected) {
          expectedHits++;
          const rank = keys.indexOf(exp.key) + 1;
          if (rank > 0 && rank <= 5) {
            top5Hits++;
          }
          if (q.mandatory) {
            expect(rank, `${q.id} missing ${exp.key}`).toBeGreaterThan(0);
            expect(rank, `${q.id} ${exp.key} rank ${rank}`).toBeLessThanOrEqual(exp.maxRank);
          }
        }
        for (const forbidden of q.forbidden ?? []) {
          expect(keys, `${q.id} leaked ${forbidden}`).not.toContain(forbidden);
        }
        const again = await mnemon.recall({ query: q.query, intent: q.intent, limit: 10 });
        expect(again.results.map((r) => r.insight.id)).toEqual(recalled.results.map((r) => r.insight.id));
      }
      expect(top5Hits / expectedHits).toBeGreaterThanOrEqual(0.9);
    });
  });

  it("works without an embedding provider", async () => {
    const clock = new FakeClock(new Date("2024-06-01T00:00:00Z"));
    await withMnemon({ clock }, async (mnemon) => {
      await mnemon.remember({
        content: "New services are written in TypeScript.",
        createdAt: "2024-03-02T09:00:00Z",
      });
      const recalled = await mnemon.recall({ query: "TypeScript services" });
      expect(recalled.results.length).toBeGreaterThan(0);
      expect(recalled.results[0]?.signals.similarity).toBe(0);
    });
  });
});
