import { describe, expect, it } from "vitest";

import {
  cosineSimilarity,
  jaccardTokenSimilarity,
  symmetricTokenSimilarity,
} from "../../src/engine/similarity.js";

describe("similarity", () => {
  it("computes symmetric token similarity", () => {
    expect(symmetricTokenSimilarity(new Set(["a", "b"]), new Set(["a"]))).toBe(1);
    expect(symmetricTokenSimilarity(new Set(), new Set(["a"]))).toBe(0);
  });

  it("computes Jaccard as intersection over union", () => {
    expect(jaccardTokenSimilarity(new Set(["a", "b"]), new Set(["a"]))).toBe(0.5);
    expect(jaccardTokenSimilarity(new Set(), new Set(["a"]))).toBe(0);
  });

  it("returns 0 for empty, mismatched, zero-norm, or non-finite cosine", () => {
    expect(cosineSimilarity([], [1])).toBe(0);
    expect(cosineSimilarity([1], [1, 2])).toBe(0);
    expect(cosineSimilarity([0, 0], [1, 0])).toBe(0);
    expect(cosineSimilarity([Number.NaN], [1])).toBe(0);
    expect(cosineSimilarity([1, 0], [1, 0])).toBe(1);
  });
});
