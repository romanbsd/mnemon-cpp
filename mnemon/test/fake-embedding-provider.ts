import { MnemonEmbeddingError } from "../src/errors.js";
import type { EmbeddingProvider } from "../src/embedding-provider.js";

export class FakeEmbeddingProvider implements EmbeddingProvider {
  constructor(
    readonly model: string,
    readonly dimensions: number,
    private readonly table: Record<string, readonly number[]>,
  ) {}

  async embed(text: string, purpose: "document" | "query"): Promise<readonly number[]> {
    const key = `${purpose}:${text}`;
    const exact = this.table[key] ?? this.table[text];
    if (!exact) {
      throw new MnemonEmbeddingError(`no fixture embedding for ${purpose} ${JSON.stringify(text)}`);
    }
    return exact;
  }
}

export function unitVector(dimensions: number, hotIndex: number): number[] {
  const v = Array.from({ length: dimensions }, () => 0);
  v[hotIndex % dimensions] = 1;
  return v;
}
