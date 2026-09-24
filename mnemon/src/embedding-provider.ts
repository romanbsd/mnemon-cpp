export interface EmbeddingProvider {
  readonly model: string;
  readonly dimensions: number;
  embed(text: string, purpose: "document" | "query"): Promise<readonly number[]>;
}
