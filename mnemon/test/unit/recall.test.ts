import { describe, expect, it } from "vitest";

import {
  causalTopologicalOrder,
  composeFinalScore,
  fuseRrf,
  minMaxNormalize,
  normalizeEliteGraph,
  transitionScore,
  traverseGraph,
} from "../../src/engine/recall.js";
import { effectiveImportance } from "../../src/engine/retention.js";

describe("RRF fusion", () => {
  it("sums reciprocal ranks and labels hybrids", () => {
    const fused = fuseRrf([
      { id: "a", signal: "keyword", rank: 1 },
      { id: "a", signal: "vector", rank: 2 },
      { id: "b", signal: "time", rank: 1 },
      { id: "c", signal: "fts", rank: 1 },
    ]);
    const a = fused.find((x) => x.id === "a")!;
    const b = fused.find((x) => x.id === "b")!;
    const c = fused.find((x) => x.id === "c")!;
    expect(a.matchedVia).toBe("hybrid");
    expect(b.matchedVia).toBe("time");
    expect(c.matchedVia).toBe("fts");
    expect(a.score).toBeGreaterThan(b.score);
  });
});

describe("beam traversal", () => {
  it("improves scores, respects beam width, and ties on id", () => {
    const state = traverseGraph({
      anchors: [{ id: "a", score: 1, matchedVia: "keyword" }],
      edges: [
        { sourceId: "a", targetId: "b", edgeType: "causal", weight: 1 },
        { sourceId: "a", targetId: "c", edgeType: "temporal", weight: 1 },
        { sourceId: "b", targetId: "d", edgeType: "causal", weight: 1 },
      ],
      intent: "WHY",
      maxCandidates: 500,
    });
    expect(state.scores.get("b")!).toBeGreaterThan(state.scores.get("c")!);
    expect(state.via.get("b")).toBe("causal");
    expect(state.traversed).toBeGreaterThan(1);
  });

  it("replaces a same-depth queued score when a later path is better", () => {
    const state = traverseGraph({
      anchors: [{ id: "a", score: 1, matchedVia: "keyword" }],
      edges: [
        { sourceId: "a", targetId: "d", edgeType: "temporal", weight: 1 },
        { sourceId: "a", targetId: "d", edgeType: "causal", weight: 1 },
        { sourceId: "d", targetId: "e", edgeType: "causal", weight: 1 },
      ],
      intent: "WHY",
      maxCandidates: 500,
    });
    const causalStep = transitionScore(0.7, 0);
    expect(state.via.get("d")).toBe("causal");
    expect(state.scores.get("e")).toBeCloseTo(1 + causalStep + causalStep);
  });

  it("runs a beam from each anchor and adds cosine to the transition", () => {
    const state = traverseGraph({
      anchors: [
        { id: "a", score: 1, matchedVia: "keyword" },
        { id: "z", score: 0.2, matchedVia: "time" },
      ],
      edges: [
        { sourceId: "a", targetId: "b", edgeType: "semantic", weight: 1 },
        { sourceId: "z", targetId: "y", edgeType: "temporal", weight: 1 },
      ],
      intent: "GENERAL",
      maxCandidates: 500,
      queryVector: [1, 0],
      embeddings: new Map([
        ["b", [1, 0]],
        ["y", [0, 1]],
      ]),
    });
    expect(state.scores.has("y")).toBe(true);
    expect(state.scores.get("b")!).toBeGreaterThan(state.scores.get("y")!);
  });
});

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
