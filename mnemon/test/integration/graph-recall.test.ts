import { randomUUID } from "node:crypto";

import { describe, expect, it } from "vitest";

import { MAX_ENTITY_LINKS } from "../../src/engine/constants.js";
import type { Mnemon } from "../../src/types.js";
import { FakeClock } from "../fake-clock.js";
import { FakeEmbeddingProvider, unitVector } from "../fake-embedding-provider.js";
import { loadEdges, postgresAvailable, withMnemon } from "./helpers.js";

const available = await postgresAvailable();
const origin = new Date("2024-06-01T12:00:00Z");
const dayMs = 25 * 60 * 60 * 1000;

function clockAt(date = origin): FakeClock {
  return new FakeClock(date);
}

function otherId(edge: { sourceId: string; targetId: string }, self: string): string {
  return edge.sourceId === self ? edge.targetId : edge.sourceId;
}

/** Space remembers so auto temporal/entity edges do not connect the intended graph. */
async function rememberIsolated(mnemon: Mnemon, clock: FakeClock, content: string) {
  clock.advance(dayMs);
  return mnemon.remember({ content, source: `src-${randomUUID()}` });
}

describe.skipIf(!available)("graph and recall (from Go integration tests)", () => {
  it("creates a bidirectional temporal backbone for the same source", async () => {
    const clock = clockAt();
    await withMnemon({ clock }, async (mnemon, { pool, schema }) => {
      const first = await mnemon.remember({
        content: "first insight",
        source: "user",
        createdAt: "2024-06-01T11:00:00Z",
      });
      const second = await mnemon.remember({
        content: "second insight",
        source: "user",
        createdAt: "2024-06-01T12:00:00Z",
      });
      expect(second.edgeCounts.temporal).toBeGreaterThanOrEqual(2);
      const temporal = (await loadEdges(pool, schema, second.insight.id)).filter((e) => e.edgeType === "temporal");
      const forward = temporal.find(
        (e) => e.sourceId === first.insight.id && e.targetId === second.insight.id,
      );
      const reverse = temporal.find(
        (e) => e.sourceId === second.insight.id && e.targetId === first.insight.id,
      );
      expect(forward?.metadata.sub_type).toBe("backbone");
      expect(reverse?.metadata.sub_type).toBe("backbone");
      expect(forward?.weight).toBe(1);
      expect(reverse?.weight).toBe(1);
    });
  });

  it("decays temporal proximity weight with time distance", async () => {
    const clock = clockAt();
    await withMnemon({ clock }, async (mnemon, { pool, schema }) => {
      const close = await mnemon.remember({
        content: "close in time",
        source: "src-close",
        createdAt: "2024-06-01T11:30:00Z",
      });
      const far = await mnemon.remember({
        content: "far in time",
        source: "src-far",
        createdAt: "2024-05-31T16:00:00Z",
      });
      const added = await mnemon.remember({
        content: "new insight",
        source: "src-new",
        createdAt: "2024-06-01T12:00:00Z",
      });
      const proximity = (await loadEdges(pool, schema, added.insight.id)).filter(
        (e) => e.edgeType === "temporal" && e.metadata.sub_type === "proximity",
      );
      const closeWeight = proximity.find((e) => otherId(e, added.insight.id) === close.insight.id)?.weight;
      const farWeight = proximity.find((e) => otherId(e, added.insight.id) === far.insight.id)?.weight;
      expect(closeWeight).toBeGreaterThan(0);
      expect(farWeight).toBeGreaterThan(0);
      expect(closeWeight).toBeGreaterThan(farWeight!);
    });
  });

  it("creates no temporal edges for a lone insight", async () => {
    await withMnemon({ clock: clockAt() }, async (mnemon) => {
      const added = await mnemon.remember({ content: "only insight", source: "user" });
      expect(added.edgeCounts.temporal).toBe(0);
    });
  });

  it("creates bidirectional entity edges for co-occurrence and none when entities do not overlap", async () => {
    await withMnemon({ clock: clockAt() }, async (mnemon, { pool, schema }) => {
      const shared = await mnemon.remember({
        content: "first note about the shared topic",
        source: "user",
        entities: ["Zogblat"],
      });
      const next = await mnemon.remember({
        content: "second note about the shared topic",
        source: "user",
        entities: ["Zogblat"],
      });
      const entityEdges = (await loadEdges(pool, schema, next.insight.id)).filter((e) => e.edgeType === "entity");
      expect(entityEdges.length).toBeGreaterThanOrEqual(2);
      expect(entityEdges.every((e) => e.metadata.entity === "Zogblat")).toBe(true);
      expect(entityEdges.some((e) => otherId(e, next.insight.id) === shared.insight.id)).toBe(true);

      const other = await mnemon.remember({
        content: "unrelated note about a different topic",
        source: "other",
        entities: ["Quonex"],
      });
      const otherEntity = (await loadEdges(pool, schema, other.insight.id)).filter((e) => e.edgeType === "entity");
      expect(otherEntity).toEqual([]);
    });
  });

  it("creates no entity edges when none are stored", async () => {
    await withMnemon({ clock: clockAt() }, async (mnemon) => {
      const added = await mnemon.remember({ content: "plain sentence without named things", source: "user" });
      expect(added.edgeCounts.entity).toBe(0);
    });
  });

  it("caps entity co-occurrence links per entity", async () => {
    const clock = clockAt();
    await withMnemon({ clock }, async (mnemon) => {
      for (let i = 0; i < 10; i++) {
        clock.advance(60_000);
        await mnemon.remember({
          content: `note number ${i} about the shared topic`,
          source: "user",
          entities: ["Zogblat"],
        });
      }
      const added = await mnemon.remember({
        content: "another note about the shared topic",
        source: "user",
        entities: ["Zogblat"],
      });
      expect(added.edgeCounts.entity).toBeLessThanOrEqual(MAX_ENTITY_LINKS * 2);
    });
  });

  it("points causal edges from cause to effect when the new text has because", async () => {
    await withMnemon({ clock: clockAt() }, async (mnemon, { pool, schema }) => {
      const cause = await mnemon.remember({
        content: "SQLite has low latency and small footprint",
        source: "user",
      });
      const effect = await mnemon.remember({
        content: "chose SQLite because of low latency and small footprint",
        source: "user",
      });
      const causal = (await loadEdges(pool, schema, effect.insight.id)).filter((e) => e.edgeType === "causal");
      expect(causal.length).toBeGreaterThanOrEqual(1);
      expect(causal[0]?.sourceId).toBe(cause.insight.id);
      expect(causal[0]?.targetId).toBe(effect.insight.id);
      expect(causal[0]?.metadata.sub_type).toBeTruthy();
    });
  });

  it("creates no causal edge without a causal phrase or with insufficient overlap", async () => {
    await withMnemon({ clock: clockAt() }, async (mnemon, { pool, schema }) => {
      await mnemon.remember({ content: "Go is a programming language", source: "user" });
      const none = await mnemon.remember({ content: "SQLite is a database engine", source: "user" });
      expect((await loadEdges(pool, schema, none.insight.id)).filter((e) => e.edgeType === "causal")).toEqual([]);

      await mnemon.remember({
        content: "apple banana cherry mango peach grape because fruit",
        source: "overlap",
      });
      const low = await mnemon.remember({
        content: "therefore dog elephant fox giraffe zebra lion tiger",
        source: "overlap",
      });
      expect((await loadEdges(pool, schema, low.insight.id)).filter((e) => e.edgeType === "causal")).toEqual([]);
    });
  });

  it("walks related BFS hops, respects maxDepth and limit, and skips forgotten nodes", async () => {
    const clock = clockAt();
    await withMnemon({ clock }, async (mnemon) => {
      const a = await rememberIsolated(mnemon, clock, "node A");
      const b = await rememberIsolated(mnemon, clock, "node B");
      const c = await rememberIsolated(mnemon, clock, "node C");
      const d = await rememberIsolated(mnemon, clock, "node D disconnected");
      await mnemon.link({
        sourceId: a.insight.id,
        targetId: b.insight.id,
        edgeType: "semantic",
        weight: 0.9,
      });
      await mnemon.link({
        sourceId: b.insight.id,
        targetId: c.insight.id,
        edgeType: "temporal",
        weight: 1,
      });

      const twoHops = await mnemon.related(a.insight.id, { maxDepth: 2, limit: 10 });
      expect(twoHops.find((n) => n.id === b.insight.id)?.depth).toBe(1);
      expect(twoHops.find((n) => n.id === c.insight.id)?.depth).toBe(2);
      expect(twoHops.some((n) => n.id === d.insight.id)).toBe(false);

      const oneHop = await mnemon.related(a.insight.id, { maxDepth: 1, limit: 10 });
      expect(oneHop.some((n) => n.id === c.insight.id)).toBe(false);

      const leaves = [];
      for (let i = 0; i < 10; i++) {
        leaves.push(await rememberIsolated(mnemon, clock, `leaf ${i}`));
        await mnemon.link({
          sourceId: a.insight.id,
          targetId: leaves[i]!.insight.id,
          edgeType: "entity",
          weight: 1,
        });
      }
      const capped = await mnemon.related(a.insight.id, { maxDepth: 1, limit: 3 });
      expect(capped.length).toBeLessThanOrEqual(3);

      await mnemon.forget(b.insight.id);
      const afterForget = await mnemon.related(a.insight.id, { maxDepth: 2, limit: 20 });
      expect(afterForget.some((n) => n.id === b.insight.id)).toBe(false);
    });
  });

  it("creates semantic edges above the cosine floor and none below it or without embeddings", async () => {
    const high = new FakeEmbeddingProvider("fixture", 4, {
      "document:Go concurrency patterns": [1, 0.9, 0.8, 0.7],
      "document:Go goroutine patterns": [1, 0.85, 0.82, 0.71],
    });
    await withMnemon({ clock: clockAt(), embeddingProvider: high }, async (mnemon, { pool, schema }) => {
      await mnemon.remember({ content: "Go concurrency patterns", source: "user" });
      const second = await mnemon.remember({ content: "Go goroutine patterns", source: "user" });
      const semantic = (await loadEdges(pool, schema, second.insight.id)).filter((e) => e.edgeType === "semantic");
      expect(semantic.length).toBeGreaterThan(0);
      expect(semantic.every((e) => e.metadata.created_by === "auto")).toBe(true);
    });

    const low = new FakeEmbeddingProvider("fixture", 4, {
      "document:completely different": unitVector(4, 0),
      "document:unrelated topic": unitVector(4, 3),
    });
    await withMnemon({ clock: clockAt(), embeddingProvider: low }, async (mnemon, { pool, schema }) => {
      await mnemon.remember({ content: "completely different", source: "user" });
      const second = await mnemon.remember({ content: "unrelated topic", source: "user" });
      expect((await loadEdges(pool, schema, second.insight.id)).filter((e) => e.edgeType === "semantic")).toEqual([]);
    });

    await withMnemon({ clock: clockAt() }, async (mnemon) => {
      await mnemon.remember({ content: "prior without vectors", source: "user" });
      const added = await mnemon.remember({ content: "new without vectors", source: "user" });
      expect(added.edgeCounts.semantic).toBe(0);
    });
  });

  it("recalls with auto GENERAL intent, override, limit, sparse hint, and descending scores", async () => {
    await withMnemon({ clock: clockAt() }, async (mnemon) => {
      const sqlite = await mnemon.remember({
        content: "Go uses SQLite for persistent graph storage",
        source: "user",
        entities: ["Go", "SQLite"],
        createdAt: "2024-06-01T10:00:00Z",
      });
      await mnemon.remember({
        content: "Python web framework with Django",
        source: "user",
        entities: ["Python", "Django"],
        createdAt: "2024-06-01T11:00:00Z",
      });
      await mnemon.remember({
        content: "Go concurrency goroutine patterns",
        source: "user",
        entities: ["Go"],
        createdAt: "2024-06-01T12:00:00Z",
      });

      const basic = await mnemon.recall({ query: "Go SQLite storage", limit: 5 });
      expect(basic.results.length).toBeGreaterThan(0);
      expect(basic.meta.intent).toBe("GENERAL");
      expect(basic.meta.intentSource).toBe("auto");
      expect(basic.results[0]?.insight.id).toBe(sqlite.insight.id);

      const overridden = await mnemon.recall({ query: "test query", intent: "WHY", limit: 5 });
      expect(overridden.meta.intent).toBe("WHY");
      expect(overridden.meta.intentSource).toBe("override");

      for (let i = 0; i < 10; i++) {
        await mnemon.remember({
          content: `common shared keyword content lim-${i}`,
          source: "limit",
        });
      }
      const limited = await mnemon.recall({ query: "common shared keyword content", limit: 3 });
      expect(limited.results.length).toBeLessThanOrEqual(3);

      const ranked = await mnemon.recall({ query: "Go SQLite database", limit: 10 });
      for (let i = 1; i < ranked.results.length; i++) {
        expect(ranked.results[i]!.score).toBeLessThanOrEqual(ranked.results[i - 1]!.score);
      }
    });

    await withMnemon({ clock: clockAt() }, async (mnemon) => {
      await mnemon.remember({ content: "completely unrelated cooking recipe", source: "user" });
      const sparse = await mnemon.recall({ query: "quantum computing algorithms", limit: 10 });
      expect(sparse.meta.hint).toBe("sparse_results");
    });
  });

  it("ranks embedding-aligned recall hits with a non-zero similarity signal", async () => {
    const provider = new FakeEmbeddingProvider("fixture", 3, {
      "document:Go memory management garbage collector internals": [0.9, 0.8, 0.1],
      "document:cooking pasta recipe with tomato sauce": [0.1, 0.1, 0.9],
      "query:Go memory internals": [0.85, 0.75, 0.15],
    });
    await withMnemon({ clock: clockAt(), embeddingProvider: provider }, async (mnemon) => {
      const hit = await mnemon.remember({
        content: "Go memory management garbage collector internals",
        source: "user",
        entities: ["Go"],
      });
      await mnemon.remember({ content: "cooking pasta recipe with tomato sauce", source: "user" });
      const recalled = await mnemon.recall({ query: "Go memory internals", limit: 5 });
      expect(recalled.results[0]?.insight.id).toBe(hit.insight.id);
      expect(recalled.results[0]?.signals.similarity).toBeGreaterThan(0);
    });
  });
});
