import { ACRONYM_STOPWORDS, MAX_ENTITIES, TECH_DICTIONARY } from "./constants.js";
import { uniquePreserveOrder } from "./normalize.js";

const RE_CAMEL = /\b([A-Z][a-z]+(?:[A-Z][a-z]+)+)\b/g;
const RE_CAPS = /\b([A-Z]{2,6})\b/g;
const RE_PATH = /(?:^|[\s"'(])([.\w/-]+\.\w{1,10})(?:[\s"'),.]|$)/g;
const RE_URL = /https?:\/\/[^\s"'<>)]+/g;
const RE_MENTION = /@([a-zA-Z_]\w+)/g;

function addMatches(text: string, pattern: RegExp, capture: number, add: (value: string) => void): void {
  pattern.lastIndex = 0;
  for (const match of text.matchAll(pattern)) {
    const value = capture === 0 ? match[0] : match[capture];
    if (value) {
      add(value);
    }
  }
}

function extractCjkTitles(text: string, add: (value: string) => void): void {
  const pairs: Array<[string, string]> = [
    ["《", "》"],
    ["「", "」"],
  ];
  for (const [open, close] of pairs) {
    let pos = 0;
    while (pos < text.length) {
      const i = text.indexOf(open, pos);
      if (i === -1) {
        break;
      }
      const start = i + open.length;
      const j = text.indexOf(close, start);
      if (j === -1) {
        break;
      }
      add(text.slice(start, j));
      pos = j + close.length;
    }
  }
}

const RE_WIDE_CAPITAL = /\b([A-Z][a-zA-Z0-9]+)\b/g;

export function extractEntities(text: string): string[] {
  const out: string[] = [];
  const seen = new Set<string>();
  const add = (entity: string): void => {
    if (entity.length === 0 || seen.has(entity) || ACRONYM_STOPWORDS.has(entity)) {
      return;
    }
    seen.add(entity);
    out.push(entity);
  };

  addMatches(text, RE_CAMEL, 1, add);
  addMatches(text, RE_CAPS, 1, add);
  addMatches(text, RE_PATH, 1, add);
  addMatches(text, RE_URL, 0, add);
  addMatches(text, RE_MENTION, 1, add);
  extractCjkTitles(text, add);

  for (const word of text.split(/[^a-zA-Z0-9]+/u)) {
    if (TECH_DICTIONARY.has(word) && !seen.has(word)) {
      seen.add(word);
      out.push(word);
    }
  }
  return out.slice(0, MAX_ENTITIES);
}

export function extractEntitiesIndexed(text: string, knownEntities: ReadonlySet<string>): string[] {
  const entities = extractEntities(text);
  if (knownEntities.size === 0) {
    return entities;
  }
  const seen = new Set(entities);
  addMatches(text, RE_WIDE_CAPITAL, 1, (cand) => {
    if (seen.has(cand) || ACRONYM_STOPWORDS.has(cand) || !knownEntities.has(cand)) {
      return;
    }
    seen.add(cand);
    entities.push(cand);
  });
  for (const word of text.split(/[^a-zA-Z0-9]+/u)) {
    if (!word || seen.has(word) || ACRONYM_STOPWORDS.has(word) || !knownEntities.has(word)) {
      continue;
    }
    seen.add(word);
    entities.push(word);
  }
  return entities.slice(0, MAX_ENTITIES);
}

export function mergeEntities(provided: readonly string[], extracted: readonly string[]): string[] {
  return uniquePreserveOrder([...provided, ...extracted]).slice(0, MAX_ENTITIES);
}
