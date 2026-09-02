import { describe, expect, it } from "vitest";

import { flattenWhitespace, makeBriefExcerpt } from "../../src/engine/brief.js";
import {
  validateListInput,
  validateLogInput,
  validateRecallInput,
  validateSearchInput,
} from "../../src/engine/validate.js";
import { MnemonValidationError } from "../../src/errors.js";

describe("makeBriefExcerpt", () => {
  it("flattens whitespace runs", () => {
    expect(flattenWhitespace("  prefer\n\nTypeScript   for  services \t")).toBe(
      "prefer TypeScript for services",
    );
  });

  it("returns flattened text when it fits", () => {
    expect(makeBriefExcerpt("short memory", 240)).toBe("short memory");
  });

  it("truncates on a code-point boundary and appends an ellipsis", () => {
    expect(makeBriefExcerpt("abcdefghij", 5)).toBe("abcd\u2026");
    expect(makeBriefExcerpt("日本語テスト", 3)).toBe("日本\u2026");
  });

  it("is only an ellipsis when the limit is 1", () => {
    expect(makeBriefExcerpt("hello", 1)).toBe("\u2026");
  });

  it("strips trailing whitespace from the prefix before the ellipsis", () => {
    expect(makeBriefExcerpt("abc de", 5)).toBe("abc\u2026");
  });
});

describe("validateRecallInput brief", () => {
  it("defaults excerptChars to 240", () => {
    const validated = validateRecallInput({ query: "widgets", brief: true }, 10);
    expect(validated.brief).toBe(true);
    expect(validated.excerptChars).toBe(240);
  });

  it("rejects a non-positive excerptChars when brief is on", () => {
    expect(() => validateRecallInput({ query: "widgets", brief: true, excerptChars: 0 }, 10)).toThrow(
      MnemonValidationError,
    );
  });
});

describe("validateSearchInput and validateLogInput", () => {
  it("defaults search and log limits", () => {
    expect(validateSearchInput({ query: "embeddings" }).limit).toBe(20);
    expect(validateLogInput().limit).toBe(50);
    expect(validateListInput().limit).toBe(20);
  });

  it("rejects empty search and out-of-range log limits", () => {
    expect(() => validateSearchInput({ query: "   " })).toThrow(MnemonValidationError);
    expect(() => validateLogInput({ limit: 0 })).toThrow(MnemonValidationError);
    expect(() => validateListInput({ limit: 0 })).toThrow(MnemonValidationError);
    expect(() => validateListInput({ category: "nope" as never })).toThrow(MnemonValidationError);
    expect(() =>
      validateListInput({ since: "2024-06-02T00:00:00Z", until: "2024-06-01T00:00:00Z" }),
    ).toThrow(MnemonValidationError);
  });
});
