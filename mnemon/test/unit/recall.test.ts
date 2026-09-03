import { describe, expect, it } from "vitest";

import {
  causalTopologicalOrder,
  composeFinalScore,
  minMaxNormalize,
  normalizeEliteGraph,
} from "../../src/engine/recall.js";
import { effectiveImportance } from "../../src/engine/retention.js";

describe("causal topological order", () => {
  it("puts causes before effects and appends cycles in original order", () => {
    const ordered = causalTopologicalOrder(
      [
        { id: "effect", score: 0.9 },
        { id: "cause", score: 0.5 },
        { id: "loop-a", score: 0.2 },
        { id: "loop-b", score: 0.3 },
      ],
      [
        { sourceId: "cause", targetId: "effect" },
        { sourceId: "loop-a", targetId: "loop-b" },
        { sourceId: "loop-b", targetId: "loop-a" },
      ],
    );
    expect(ordered.map((x) => x.id).slice(0, 2)).toEqual(["cause", "effect"]);
    expect(ordered.map((x) => x.id).slice(2)).toEqual(["loop-a", "loop-b"]);
  });
});

describe("elite graph normalization", () => {
  it("zeros graph for time-only nodes when embeddings are off", () => {
    const graph = normalizeEliteGraph(
      [
        { id: "kw-low", keyword: 0.4, similarity: 0, graphRaw: 1 },
        { id: "kw-high", keyword: 0.5, similarity: 0, graphRaw: 3 },
        { id: "time", keyword: 0, similarity: 0, graphRaw: 9 },
      ],
      false,
    );
    expect(graph.get("kw-high")).toBe(1);
    expect(graph.get("kw-low")).toBe(0);
    expect(graph.get("time")).toBe(0);
  });

  it("keeps graph among keyword hits and strong cosine hits", () => {
    const graph = normalizeEliteGraph(
      [
        { id: "kw", keyword: 0.5, similarity: 0.2, graphRaw: 1 },
        { id: "vec", keyword: 0, similarity: 0.99, graphRaw: 3 },
        { id: "weak", keyword: 0, similarity: 0.7, graphRaw: 5 },
      ],
      true,
    );
    expect(graph.get("vec")).toBe(1);
    expect(graph.get("kw")).toBe(0);
    expect(graph.get("weak")).toBe(0);
  });
});

describe("final score and retention", () => {
  it("composes weighted scores", () => {
    expect(composeFinalScore({ keyword: 1, entity: 0, similarity: 0, graph: 0, hasQueryEmbedding: true })).toBe(0.3);
    expect(composeFinalScore({ keyword: 1, entity: 0, similarity: 0, graph: 0, hasQueryEmbedding: false })).toBe(0.45);
  });

  it("normalizes graph scores", () => {
    expect(minMaxNormalize([2, 4, 6])).toEqual([0, 0.5, 1]);
    expect(minMaxNormalize([0.4, 0.4, 0.4])).toEqual([0, 0, 0]);
    expect(minMaxNormalize([0, 0])).toEqual([0, 0]);
  });

  it("matches the effective-importance formula", () => {
    const ei = effectiveImportance({ importance: 3, accessCount: 0, daysSinceAccess: 0, edgeCount: 0 });
    expect(ei).toBeCloseTo(0.5);
    const decayed = effectiveImportance({ importance: 5, accessCount: 0, daysSinceAccess: 30, edgeCount: 5 });
    expect(decayed).toBeCloseTo(1 * 1 * 0.5 * 1.5);
  });
});
