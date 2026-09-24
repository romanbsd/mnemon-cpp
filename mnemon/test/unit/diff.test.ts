import { describe, expect, it } from "vitest";

import { classifyDiff, classifySafeDuplicate, classifySuggestion, isSafeDuplicate } from "../../src/engine/diff.js";

describe("safe duplicate classification", () => {
  it("treats near-verbatim token-equal text as a duplicate", () => {
    expect(isSafeDuplicate("Prefer TypeScript for services", "Prefer TypeScript for services")).toBe(true);
  });

  it("does not discard a longer extension", () => {
    expect(
      isSafeDuplicate(
        "Prefer TypeScript for services because the team already knows it",
        "Prefer TypeScript for services",
      ),
    ).toBe(false);
  });

  it("does not discard a negated correction", () => {
    expect(isSafeDuplicate("Do not use TypeScript for services", "Prefer TypeScript for services")).toBe(false);
    expect(isSafeDuplicate("Do not prefer TypeScript for services", "Prefer TypeScript for services")).toBe(false);
  });

  it("picks the first safe candidate", () => {
    const hit = classifySafeDuplicate("alpha beta gamma", [
      { id: "1", content: "unrelated text here", tokenSimilarity: 0.1, cosineSimilarity: 0 },
      { id: "2", content: "alpha beta gamma", tokenSimilarity: 1, cosineSimilarity: 0.99 },
    ]);
    expect(hit?.id).toBe("2");
  });
});

describe("Go Diff classifier (suggestion only)", () => {
  it("returns ADD below 0.5 and UPDATE at the 0.5 boundary", () => {
    expect(classifySuggestion(0.3, 0.3, "completely new content", "existing different content")).toBe("ADD");
    expect(classifySuggestion(0.5, 0.5, "some content here", "other content here")).toBe("UPDATE");
  });

  it("returns DUPLICATE only when token similarity is above 0.9 and the text is not an extension", () => {
    expect(classifySuggestion(0.95, 0.95, "very similar content here", "very similar content here indeed")).toBe(
      "DUPLICATE",
    );
    expect(classifySuggestion(0.9, 0.9, "some content here", "other content here")).toBe("UPDATE");
  });

  it("classifies same-domain rewrites as UPDATE", () => {
    expect(classifySuggestion(0.7, 0.7, "Go uses SQLite for storage", "Go uses PostgreSQL for storage")).toBe(
      "UPDATE",
    );
  });

  it("classifies listed negation phrases as CONFLICT at similarity >= 0.7", () => {
    expect(classifySuggestion(0.7, 0.7, "no longer supports Python 2", "supports Python 2")).toBe("CONFLICT");
    expect(classifySuggestion(0.7, 0.7, "replaced Flask with FastAPI", "uses Flask for API")).toBe("CONFLICT");
    expect(classifySuggestion(0.7, 0.7, "不再使用Redis", "使用Redis")).toBe("CONFLICT");
  });

  it("does not treat bare 'not' as CONFLICT", () => {
    expect(
      classifySuggestion(0.7, 0.7, "species not recorded at this site", "species recorded at Kinabalu"),
    ).not.toBe("CONFLICT");
  });

  it("ignores conflict phrases when similarity is below 0.7", () => {
    expect(classifySuggestion(0.6, 0.6, "no longer present at Raub site", "butterfly survey Kinabalu")).not.toBe(
      "CONFLICT",
    );
  });

  it("picks the strongest match and promotes DUPLICATE over UPDATE", () => {
    const result = classifyDiff("Go uses SQLite for persistent memory storage", [
      { id: "1", content: "Go uses SQLite for persistent memory storage", cosineSimilarity: 0 },
      { id: "2", content: "Python machine learning with TensorFlow", cosineSimilarity: 0 },
    ]);
    expect(result.suggestion).toBe("DUPLICATE");
    expect(result.matches[0]?.id).toBe("1");
  });
});
