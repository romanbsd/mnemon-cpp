import { randomUUID } from "node:crypto";

import { Pool } from "pg";
import { describe, expect, it } from "vitest";

import { createMnemon } from "../../src/mnemon.js";
import { FakeClock } from "../fake-clock.js";
import { FakeEmbeddingProvider, unitVector } from "../fake-embedding-provider.js";
import { postgresAvailable, TEST_DATABASE_URL, withMnemon } from "./helpers.js";

const available = await postgresAvailable();

describe.skipIf(!available)("postgres integration", () => {
  const clock = new FakeClock(new Date("2024-06-01T00:00:00Z"));

  it("runs migrations idempotently", async () => {
    await withMnemon({ clock }, async (mnemon) => {
      await mnemon.initialize();
      await mnemon.initialize();
    });
  });

  it("round-trips insights and skips exact duplicates", async () => {
    await withMnemon({ clock }, async (mnemon) => {
      const first = await mnemon.remember({ content: "Exact hash body", createdAt: "2024-06-01T00:00:00Z" });
      expect(first.action).toBe("added");
      const second = await mnemon.remember({ content: " exact   HASH body " });
      expect(second.action).toBe("skipped");
      expect(second.duplicateOf).toBe(first.insight.id);
      const loaded = await mnemon.get(first.insight.id);
      expect(loaded?.content).toBe("Exact hash body");
    });
  });

  it("does not discard extensions or corrections", async () => {
    await withMnemon({ clock }, async (mnemon) => {
      const base = await mnemon.remember({ content: "Prefer TypeScript for services" });
      const ext = await mnemon.remember({
        content: "Prefer TypeScript for services because the team already knows it",
      });
      const neg = await mnemon.remember({ content: "Do not prefer TypeScript for services" });
      expect(base.action).toBe("added");
      expect(ext.action).toBe("added");
      expect(neg.action).toBe("added");
      expect(neg.suggestion).toBe("DUPLICATE");
      expect(await mnemon.get(base.insight.id)).not.toBeNull();
      const conflict = await mnemon.remember({ content: "No longer prefer TypeScript for services" });
      expect(conflict.action).toBe("added");
      expect(conflict.suggestion).toBe("CONFLICT");
      expect(await mnemon.get(base.insight.id)).not.toBeNull();
    });
  });

  it("scores entity overlap case-insensitively", async () => {
    await withMnemon({ clock }, async (mnemon) => {
      const added = await mnemon.remember({
        content: "Prefers the language for services",
        entities: ["typescript"],
      });
      const recalled = await mnemon.recall({ query: "tell me about TypeScript", intent: "ENTITY" });
      const hit = recalled.results.find((r) => r.insight.id === added.insight.id);
      expect(hit?.signals.entity).toBeGreaterThan(0);
    });
  });

  it("stores embeddings and returns cosine neighbors", async () => {
    const provider = new FakeEmbeddingProvider("fixture", 4, {
      "document:alpha": unitVector(4, 0),
      "query:alpha": unitVector(4, 0),
      alpha: unitVector(4, 0),
      "document:beta": [0.99, 0.1, 0, 0],
      "document:gamma": unitVector(4, 2),
    });
    await withMnemon({ clock, embeddingProvider: provider }, async (mnemon) => {
      const a = await mnemon.remember({ content: "alpha" });
      await mnemon.remember({ content: "beta" });
      await mnemon.remember({ content: "gamma" });
      expect(a.action).toBe("added");
      const recalled = await mnemon.recall({ query: "alpha", limit: 3 });
      expect(recalled.results.some((r) => r.insight.content === "beta")).toBe(true);
    });
  });

  it("forgets atomically and hides the insight from recall", async () => {
    await withMnemon({ clock }, async (mnemon) => {
      const added = await mnemon.remember({ content: "forgettable lunar fact" });
      await mnemon.link({
        sourceId: added.insight.id,
        targetId: (await mnemon.remember({ content: "other node" })).insight.id,
        edgeType: "entity",
      });
      const result = await mnemon.forget(added.insight.id);
      expect(result.forgotten).toBe(true);
      expect(await mnemon.get(added.insight.id)).toBeNull();
      const again = await mnemon.forget(added.insight.id);
      expect(again.forgotten).toBe(false);
      const recalled = await mnemon.recall({ query: "lunar fact" });
      expect(recalled.results.every((r) => r.insight.id !== added.insight.id)).toBe(true);
    });
  });

  it("projects a brief excerpt and keeps the full insight behind get", async () => {
    await withMnemon({ clock }, async (mnemon) => {
      const added = await mnemon.remember({
        content: "Prefer TypeScript for new services because the team already knows it well",
      });
      const recalled = await mnemon.recall({ query: "TypeScript services", brief: true, excerptChars: 24 });
      const hit = recalled.results.find((r) => r.insight.id === added.insight.id);
      expect(hit?.excerpt).toBe("Prefer TypeScript for n\u2026");
      expect(hit?.insight.content).toBe(hit?.excerpt);
      const full = await mnemon.get(added.insight.id);
      expect(full?.content).toBe(
        "Prefer TypeScript for new services because the team already knows it well",
      );
    });
  });

  it("links, walks related, and leaves an injected pool open", async () => {
    const pool = new Pool({ connectionString: TEST_DATABASE_URL });
    try {
      await withMnemon({ clock, pool }, async (mnemon) => {
        const a = await mnemon.remember({ content: "node a about widgets" });
        const b = await mnemon.remember({ content: "node b about widgets" });
        await mnemon.link({ sourceId: a.insight.id, targetId: b.insight.id, edgeType: "entity", weight: 0.8 });
        const related = await mnemon.related(a.insight.id);
        expect(related.map((r) => r.id)).toContain(b.insight.id);
        const hit = related.find((r) => r.id === b.insight.id);
        expect(hit?.depth).toBe(1);
        expect(hit?.viaEdgeType).toBe("temporal");
        const entityOnly = await mnemon.related(a.insight.id, { edgeType: "entity" });
        expect(entityOnly.find((r) => r.id === b.insight.id)?.viaEdgeType).toBe("entity");
        const first = await mnemon.remember({
          content: "Hestia owns the billing store",
          entities: ["Hestia"],
        });
        expect(first.insight.entities).toContain("Hestia");
        const second = await mnemon.remember({ content: "Talk to Hestia about invoices" });
        expect(second.insight.entities).toContain("Hestia");
      });
      await pool.query("SELECT 1");
    } finally {
      await pool.end();
    }
  });

  it("rolls back a failed transaction without leftover rows", async () => {
    await withMnemon({ clock }, async (mnemon, { pool, schema }) => {
      await mnemon.remember({ content: "stable row" });
      await expect(
        (mnemon as unknown as { store: { withTransaction: Function } }).store.withTransaction(async () => {
          throw new Error("boom");
        }),
      ).rejects.toThrow();
      const count = await pool.query(`SELECT count(*)::int AS n FROM "${schema}".insights`);
      expect(count.rows[0]?.n).toBe(1);
    });
  });

  it("rejects a second provider with a different dimension", async () => {
    const first = new FakeEmbeddingProvider("a", 4, { "document:dim": unitVector(4, 0) });
    await withMnemon({ clock, embeddingProvider: first }, async (mnemon, { pool, schema }) => {
      await mnemon.remember({ content: "dim" });
      const second = createMnemon({
        pool,
        schema,
        embeddingProvider: new FakeEmbeddingProvider("b", 8, { "document:other": unitVector(8, 0) }),
      });
      await expect(second.initialize()).rejects.toThrow(/dimension/);
    });
  });

  it("lists active insights by recency and filters", async () => {
    await withMnemon({ clock }, async (mnemon) => {
      clock.set(new Date("2024-06-01T00:00:00Z"));
      const older = await mnemon.remember({
        content: "older ops note",
        source: "ops",
        category: "fact",
        createdAt: "2024-06-01T00:00:00Z",
      });
      clock.set(new Date("2024-06-02T00:00:00Z"));
      const newer = await mnemon.remember({
        content: "newer agent note",
        source: "agent",
        category: "insight",
        createdAt: "2024-06-02T00:00:00Z",
      });
      await mnemon.forget(older.insight.id);
      const listed = await mnemon.list();
      expect(listed.map((r) => r.id)).toEqual([newer.insight.id]);
      const filtered = await mnemon.list({
        source: "agent",
        category: "insight",
        since: "2024-06-02T00:00:00Z",
        limit: 1,
      });
      expect(filtered.map((r) => r.id)).toEqual([newer.insight.id]);
      expect(await mnemon.list({ source: "ops" })).toEqual([]);
    });
  });

  it("searches with stemming, reads the oplog, and reports status", async () => {
    await withMnemon({ clock }, async (mnemon) => {
      await mnemon.remember({ content: "Ollama runs locally for embeddings when no cloud key exists." });
      await mnemon.remember({ content: "The library makes no network calls except the injected embedding provider." });
      const stemmed = await mnemon.search({ query: "where do embeddings run" });
      expect(stemmed.results.some((r) => /Ollama runs/.test(r.insight.content))).toBe(true);
      expect(stemmed.results.some((r) => /embedding/.test(r.insight.content))).toBe(true);
      const ops = await mnemon.log({ operation: "remember", limit: 10 });
      expect(ops.length).toBe(2);
      expect(ops.every((o) => o.operation === "remember")).toBe(true);
      const status = await mnemon.status();
      expect(status.insights).toBe(2);
      expect(status.embeddings).toBe(0);
      expect(status.edges).toBeGreaterThan(0);
      expect(status.algorithmVersion).toBe("mnemon-ts-v1");
    });
  });

  it("handles concurrent exact-hash inserts", async () => {
    await withMnemon({ clock }, async (mnemon) => {
      const content = `race ${randomUUID()}`;
      const [a, b] = await Promise.all([
        mnemon.remember({ content }),
        mnemon.remember({ content }),
      ]);
      const actions = [a.action, b.action].sort();
      expect(actions).toEqual(["added", "skipped"]);
    });
  });
});
