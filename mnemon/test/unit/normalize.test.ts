import { describe, expect, it } from "vitest";

import { contentHash, normalizeContent } from "../../src/engine/normalize.js";
import { validateRememberInput } from "../../src/engine/validate.js";
import { MnemonValidationError } from "../../src/errors.js";

const defaults = { category: "general" as const, importance: 3 as const, source: "agent" };

describe("normalizeContent", () => {
  it("applies NFKC, lowercases, trims, and collapses whitespace", () => {
    expect(normalizeContent("  Café\t\nTEST  ")).toBe("café test");
    expect(normalizeContent("ＡＢＣ")).toBe("abc");
  });

  it("hashes the UTF-8 bytes of normalized content", () => {
    expect(contentHash("hello")).toBe("2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
  });
});

describe("validateRememberInput", () => {
  it("rejects empty and oversized content", () => {
    expect(() => validateRememberInput({ content: "  " }, defaults)).toThrow(MnemonValidationError);
    expect(() => validateRememberInput({ content: "x".repeat(8001) }, defaults)).toThrow(MnemonValidationError);
  });

  it("rejects non-finite importance and invalid category", () => {
    expect(() => validateRememberInput({ content: "ok", importance: 0 as 1 }, defaults)).toThrow(
      MnemonValidationError,
    );
    expect(() => validateRememberInput({ content: "ok", category: "nope" as "fact" }, defaults)).toThrow(
      MnemonValidationError,
    );
  });

  it("requires an explicit timezone on createdAt", () => {
    expect(() => validateRememberInput({ content: "ok", createdAt: "2020-01-01T00:00:00" }, defaults)).toThrow(
      MnemonValidationError,
    );
    expect(validateRememberInput({ content: "ok", createdAt: "2020-01-01T00:00:00Z" }, defaults).createdAt).toEqual(
      new Date("2020-01-01T00:00:00Z"),
    );
  });
});
