import { DUPLICATE_MAX_LENGTH_RATIO, DUPLICATE_TOKEN_SIMILARITY } from "./constants.js";
import { codePointLength } from "./normalize.js";
import { cosineSimilarity, jaccardTokenSimilarity, setsEqual, symmetricTokenSimilarity } from "./similarity.js";
import { tokenize } from "./tokenize.js";

export interface DuplicateCandidate {
  id: string;
  content: string;
  tokenSimilarity: number;
  cosineSimilarity: number;
}

const NEGATION = new Set(["not", "no", "never"]);

function negationMarks(text: string): string {
  const marks: string[] = [];
  for (const word of text.toLowerCase().match(/\p{L}+/gu) ?? []) {
    if (NEGATION.has(word)) {
      marks.push(word);
    }
  }
  return marks.sort().join(",");
}

export function isSafeDuplicate(newContent: string, existingContent: string, cosine = 0): boolean {
  const newTokens = tokenize(newContent);
  const existingTokens = tokenize(existingContent);
  const tokenSim = symmetricTokenSimilarity(newTokens, existingTokens);
  if (tokenSim <= DUPLICATE_TOKEN_SIMILARITY) {
    return false;
  }
  if (codePointLength(newContent) > codePointLength(existingContent) * DUPLICATE_MAX_LENGTH_RATIO) {
    return false;
  }
  if (negationMarks(newContent) !== negationMarks(existingContent)) {
    return false;
  }
  void cosine;
  return setsEqual(newTokens, existingTokens);
}

export function scoreDuplicateCandidate(
  newContent: string,
  existingContent: string,
  newEmbedding?: readonly number[],
  existingEmbedding?: readonly number[],
): { tokenSimilarity: number; cosineSimilarity: number } {
  const tokenSimilarity = symmetricTokenSimilarity(tokenize(newContent), tokenize(existingContent));
  const cosine =
    newEmbedding && existingEmbedding ? cosineSimilarity(newEmbedding, existingEmbedding) : 0;
  return { tokenSimilarity, cosineSimilarity: cosine };
}

export function classifySafeDuplicate(
  newContent: string,
  candidates: readonly DuplicateCandidate[],
): DuplicateCandidate | undefined {
  for (const candidate of candidates) {
    if (isSafeDuplicate(newContent, candidate.content, candidate.cosineSimilarity)) {
      return candidate;
    }
  }
  return undefined;
}

export type DiffSuggestion = "ADD" | "DUPLICATE" | "CONFLICT" | "UPDATE";

export interface DiffMatch {
  id: string;
  content: string;
  tokenSimilarity: number;
  cosineSimilarity: number;
  similarity: number;
  suggestion: DiffSuggestion;
}

export interface DiffResult {
  suggestion: DiffSuggestion;
  matches: DiffMatch[];
}

/** Same phrase list as Go. Bare "not" is excluded so scientific text does not look like CONFLICT. */
export const DIFF_NEGATION_PHRASES = [
  "no longer",
  "don't",
  "doesn't",
  "never",
  "switched from",
  "instead of",
  "rather than",
  "replaced",
  "deprecated",
  "不再",
  "放弃",
  "替换",
  "取消",
] as const;

function isExtension(newText: string, existingText: string): boolean {
  const n = codePointLength(newText);
  const e = codePointLength(existingText);
  return n > e + Math.floor(e / 4);
}

function hasConflictPhrase(newText: string, existingText: string): boolean {
  const newer = newText.toLowerCase();
  const older = existingText.toLowerCase();
  return DIFF_NEGATION_PHRASES.some((phrase) => newer.includes(phrase) || older.includes(phrase));
}

export function classifySuggestion(
  tokenSim: number,
  similarity: number,
  newText: string,
  existingText: string,
): DiffSuggestion {
  if (similarity < 0.5) {
    return "ADD";
  }
  const extension = isExtension(newText, existingText);
  if (tokenSim > 0.9 && !extension) {
    return "DUPLICATE";
  }
  if (similarity >= 0.7 && hasConflictPhrase(newText, existingText)) {
    return "CONFLICT";
  }
  if (similarity > 0.9 && !extension) {
    return "DUPLICATE";
  }
  return "UPDATE";
}

function combinedSimilarity(tokenSim: number, cosineSim: number): number {
  if (cosineSim >= 0.85 && cosineSim > tokenSim) {
    return cosineSim;
  }
  return tokenSim;
}

export function classifyDiff(
  newContent: string,
  candidates: readonly { id: string; content: string; cosineSimilarity?: number }[],
): DiffResult {
  const newTokens = tokenize(newContent);
  const matches: DiffMatch[] = candidates.map((candidate) => {
    const tokenSimilarity = jaccardTokenSimilarity(newTokens, tokenize(candidate.content));
    const cosineSimilarity = candidate.cosineSimilarity ?? 0;
    const similarity = combinedSimilarity(tokenSimilarity, cosineSimilarity);
    return {
      id: candidate.id,
      content: candidate.content,
      tokenSimilarity,
      cosineSimilarity,
      similarity,
      suggestion: classifySuggestion(tokenSimilarity, similarity, newContent, candidate.content),
    };
  });
  matches.sort((a, b) => b.similarity - a.similarity || a.id.localeCompare(b.id));
  let suggestion: DiffSuggestion = matches[0]?.suggestion ?? "ADD";
  if (matches.some((m) => m.suggestion === "DUPLICATE")) {
    suggestion = "DUPLICATE";
  }
  return { suggestion, matches };
}
