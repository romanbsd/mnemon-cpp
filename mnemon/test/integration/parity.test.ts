import { describe, expect, it } from "vitest";

import { OllamaEmbeddingProvider } from "../../src/http-embedding-provider.js";
import {
  compareSnapshots,
  harmfulMismatches,
  loadFixtures,
  OLLAMA_ENDPOINT,
  OLLAMA_MODEL,
  ollamaAvailable,
  resolveCppBinary,
  resolveGoBinary,
  runReferenceCli,
  runTypescript,
  writeReports,
  type CliRunOptions,
} from "../parity/harness.js";
import { postgresAvailable, withMnemon } from "./helpers.js";

const available = await postgresAvailable();
const goBin = resolveGoBinary();
const cppBin = resolveCppBinary();
const ollama = await ollamaAvailable();

function assertSafety(typescript: { remembers: Array<{ key: string; action: string }>; activeKeys: string[] }): void {
  expect(typescript.remembers.find((r) => r.key === "dark-mode-dup")?.action).toBe("skipped");
  expect(typescript.remembers.find((r) => r.key === "dark-mode-ext")?.action).toBe("added");
  expect(typescript.remembers.find((r) => r.key === "dark-mode-neg")?.action).toBe("added");
  expect(typescript.activeKeys).not.toContain("soft-delete-me");
}

describe.skipIf(!available || !goBin)("reference parity", () => {
  it("compares TypeScript against Go and C++ without embeddings", async () => {
    const fixtures = loadFixtures();
    await withMnemon({}, async (mnemon, { schema, pool }) => {
      const typescript = await runTypescript(mnemon, pool, schema, fixtures);
      const references = [runReferenceCli("go", goBin!, fixtures)];
      if (cppBin) {
        references.push(runReferenceCli("cpp", cppBin, fixtures));
      }

      const report = compareSnapshots(typescript, references, fixtures);
      const paths = writeReports(report);

      expect(report.sides).toContain("typescript");
      expect(report.sides).toContain("go");
      expect(harmfulMismatches(report), `harmful mismatches; see ${paths.markdownPath}`).toEqual([]);
      assertSafety(typescript);
    });
  });
});

describe.skipIf(!available || !goBin || !ollama)("reference parity with Ollama", () => {
  it("compares TypeScript against Go using nomic-embed-text", { timeout: 180_000 }, async () => {
    const fixtures = loadFixtures();
    const cli: CliRunOptions = {
      embeddings: "ollama",
      endpoint: OLLAMA_ENDPOINT,
      model: OLLAMA_MODEL,
    };
    const provider = new OllamaEmbeddingProvider({
      endpoint: OLLAMA_ENDPOINT,
      model: OLLAMA_MODEL,
      dimensions: 768,
    });
    await withMnemon({ embeddingProvider: provider }, async (mnemon, { schema, pool }) => {
      const typescript = await runTypescript(mnemon, pool, schema, fixtures);
      const go = runReferenceCli("go", goBin!, fixtures, cli);
      const report = compareSnapshots(typescript, [go], fixtures, cli);
      const paths = writeReports(report);

      expect(report.embeddings).toBe("ollama");
      expect(harmfulMismatches(report), `harmful mismatches; see ${paths.markdownPath}`).toEqual([]);
      assertSafety(typescript);
    });
  });
});
