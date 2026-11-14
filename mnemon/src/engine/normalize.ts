import { createHash } from "node:crypto";

export function codePointLength(value: string): number {
  return [...value].length;
}

export function normalizeContent(content: string): string {
  return content
    .normalize("NFKC")
    .toLocaleLowerCase("en-US")
    .trim()
    .replace(/\s+/gu, " ");
}

export function contentHash(normalizedContent: string): string {
  return createHash("sha256").update(normalizedContent, "utf8").digest("hex");
}

export function uniquePreserveOrder(values: readonly string[]): string[] {
  const seen = new Set<string>();
  const out: string[] = [];
  for (const value of values) {
    if (!seen.has(value)) {
      seen.add(value);
      out.push(value);
    }
  }
  return out;
}
