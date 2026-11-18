import { ENTITY_TERMS, INTENT_WEIGHTS, TRAVERSAL_LIMITS, WHEN_TERMS, WHY_TERMS } from "./constants.js";
import type { EdgeType, RecallIntent } from "../types.js";

function countTerm(haystack: string, term: string): number {
  if (term.length === 0) {
    return 0;
  }
  if (/[^\u0000-\u007f]/u.test(term)) {
    let count = 0;
    let pos = 0;
    while (pos <= haystack.length - term.length) {
      const i = haystack.indexOf(term, pos);
      if (i === -1) {
        break;
      }
      count++;
      pos = i + term.length;
    }
    return count;
  }
  if (term.includes(" ")) {
    let count = 0;
    let pos = 0;
    while (pos <= haystack.length - term.length) {
      const i = haystack.indexOf(term, pos);
      if (i === -1) {
        break;
      }
      count++;
      pos = i + term.length;
    }
    return count;
  }
  const re = new RegExp(`\\b${term}\\b`, "g");
  return haystack.match(re)?.length ?? 0;
}

function countTerms(query: string, terms: readonly string[]): number {
  let total = 0;
  for (const term of terms) {
    total += countTerm(query, term);
  }
  return total;
}

export function detectIntent(query: string): RecallIntent {
  const q = query.toLowerCase();
  const why = countTerms(q, WHY_TERMS);
  const when = countTerms(q, WHEN_TERMS);
  const entity = countTerms(q, ENTITY_TERMS);
  if (why > when && why > entity && why > 0) {
    return "WHY";
  }
  if (when > why && when > entity && when > 0) {
    return "WHEN";
  }
  if (entity > 0) {
    return "ENTITY";
  }
  return "GENERAL";
}

export function intentWeights(intent: RecallIntent): Record<EdgeType, number> {
  return INTENT_WEIGHTS[intent];
}

export function traversalLimits(intent: RecallIntent): { beam: number; depth: number; visited: number } {
  return TRAVERSAL_LIMITS[intent];
}
