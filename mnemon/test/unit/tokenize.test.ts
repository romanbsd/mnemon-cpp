import { describe, expect, it } from "vitest";

import { tokenize } from "../../src/engine/tokenize.js";

describe("tokenize", () => {
  it("tokenizes English and drops stopwords", () => {
    expect(tokenize("The quick brown fox is fast")).toEqual(new Set(["quick", "brown", "fox", "fast"]));
  });

  it("splits on punctuation", () => {
    expect(tokenize("hello, world!")).toEqual(new Set(["hello", "world"]));
  });

  it("treats emoji as a boundary", () => {
    expect(tokenize("hello😀world")).toEqual(new Set(["hello", "world"]));
  });

  it("emits a single Han character as one token", () => {
    expect(tokenize("中")).toEqual(new Set(["中"]));
  });

  it("emits overlapping CJK bigrams", () => {
    expect(tokenize("中文测试")).toEqual(new Set(["中文", "文测", "测试"]));
  });

  it("treats CJK compatibility ideographs as Han", () => {
    expect(tokenize("\uF900")).toEqual(new Set(["\uF900"]));
  });

  it("handles mixed scripts", () => {
    expect(tokenize("Use 中文 in TypeScript")).toEqual(new Set(["use", "中文", "typescript"]));
  });

  it("returns empty for all-stopword input", () => {
    expect(tokenize("the and or but")).toEqual(new Set());
  });
});
