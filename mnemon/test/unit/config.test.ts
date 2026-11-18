import { describe, expect, it } from "vitest";

import { resolveConfig } from "../../src/config.js";
import { MnemonConfigurationError } from "../../src/errors.js";

describe("resolveConfig", () => {
  it("requires exactly one of databaseUrl or pool", () => {
    expect(() => resolveConfig({})).toThrow(MnemonConfigurationError);
    expect(() => resolveConfig({ databaseUrl: "postgres://x", pool: {} as never })).toThrow(
      MnemonConfigurationError,
    );
  });

  it("rejects invalid schema names", () => {
    expect(() => resolveConfig({ databaseUrl: "postgres://x", schema: "Mnemon" })).toThrow(
      MnemonConfigurationError,
    );
    expect(() => resolveConfig({ databaseUrl: "postgres://x", schema: "drop table" })).toThrow(
      MnemonConfigurationError,
    );
  });

  it("rejects mismatched dimensions", () => {
    expect(() =>
      resolveConfig({
        databaseUrl: "postgres://x",
        embeddingDimensions: 8,
        embeddingProvider: { model: "x", dimensions: 4, embed: async () => [1, 2, 3, 4] },
      }),
    ).toThrow(MnemonConfigurationError);
  });

  it("fills defaults", () => {
    const cfg = resolveConfig({ databaseUrl: "postgres://x" });
    expect(cfg.schema).toBe("mnemon");
    expect(cfg.defaults).toEqual({ category: "general", importance: 3, source: "agent", recallLimit: 10 });
    expect(cfg.limits.maxRecallCandidates).toBe(500);
  });
});
