import { STOPWORDS } from "./constants.js";

function isHan(ch: string): boolean {
  return /\p{Script=Han}/u.test(ch);
}

function isLetterOrDigit(ch: string): boolean {
  return /\p{L}|\p{N}/u.test(ch);
}

function flushCjk(buf: string[], tokens: Set<string>): void {
  if (buf.length === 1) {
    tokens.add(buf[0]!);
  } else if (buf.length >= 2) {
    for (let i = 0; i + 1 < buf.length; i++) {
      tokens.add(buf[i]! + buf[i + 1]!);
    }
  }
  buf.length = 0;
}

export function tokenize(text: string): Set<string> {
  const tokens = new Set<string>();
  let word = "";
  const cjk: string[] = [];

  for (const ch of text.toLowerCase()) {
    if (isHan(ch)) {
      if (word.length > 0) {
        if (!STOPWORDS.has(word)) {
          tokens.add(word);
        }
        word = "";
      }
      cjk.push(ch);
      continue;
    }
    if (cjk.length > 0) {
      flushCjk(cjk, tokens);
    }
    if (isLetterOrDigit(ch)) {
      word += ch;
    } else if (word.length > 0) {
      if (!STOPWORDS.has(word)) {
        tokens.add(word);
      }
      word = "";
    }
  }

  if (word.length > 0 && !STOPWORDS.has(word)) {
    tokens.add(word);
  }
  if (cjk.length > 0) {
    flushCjk(cjk, tokens);
  }
  return tokens;
}

export function insightTokens(content: string, tags: readonly string[], entities: readonly string[]): Set<string> {
  const tokens = tokenize(content);
  for (const tag of tags) {
    for (const t of tokenize(tag)) {
      tokens.add(t);
    }
  }
  for (const entity of entities) {
    for (const t of tokenize(entity)) {
      tokens.add(t);
    }
  }
  return tokens;
}

export function sortedSearchTokens(content: string, tags: readonly string[], entities: readonly string[]): string[] {
  return [...insightTokens(content, tags, entities)].sort();
}

export function sortedTokens(text: string): string[] {
  return [...tokenize(text)].sort();
}
