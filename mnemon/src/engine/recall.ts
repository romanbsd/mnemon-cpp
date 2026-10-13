import { GRAPH_ELITE_MIN_COSINE } from "./constants.js";

export function minMaxNormalize(values: readonly number[]): number[] {
  if (values.length === 0) {
    return [];
  }
  let min = values[0]!;
  let max = values[0]!;
  for (const v of values) {
    if (v < min) min = v;
    if (v > max) max = v;
  }
  if (max === min) {
    // Match Go: a zero range is (raw-min)/1 = 0, not a flat 1.0.
    return values.map(() => 0);
  }
  return values.map((v) => (v - min) / (max - min));
}

export function isGraphElite(
  signals: { keyword: number; similarity: number },
  hasQueryEmbedding: boolean,
): boolean {
  if (signals.keyword > 0) {
    return true;
  }
  return hasQueryEmbedding && signals.similarity >= GRAPH_ELITE_MIN_COSINE;
}

/** Min-max graph only among keyword / strong-vector hits so a dense temporal clique cannot drown them. */
export function normalizeEliteGraph(
  rows: readonly { id: string; keyword: number; similarity: number; graphRaw: number }[],
  hasQueryEmbedding: boolean,
): Map<string, number> {
  const graphById = new Map<string, number>();
  const elite = rows.filter((row) => isGraphElite(row, hasQueryEmbedding));
  const norm = minMaxNormalize(elite.map((row) => row.graphRaw));
  for (const row of rows) {
    graphById.set(row.id, 0);
  }
  for (const [i, row] of elite.entries()) {
    graphById.set(row.id, norm[i] ?? 0);
  }
  return graphById;
}

const EMBEDDED_SCORE_WEIGHTS = { keyword: 0.3, entity: 0.15, similarity: 0.35, graph: 0.2 } as const;
const KEYWORD_SCORE_WEIGHTS = { keyword: 0.45, entity: 0.25, graph: 0.3 } as const;

export function composeFinalScore(input: {
  keyword: number;
  entity: number;
  similarity: number;
  graph: number;
  hasQueryEmbedding: boolean;
}): number {
  if (input.hasQueryEmbedding) {
    return (
      EMBEDDED_SCORE_WEIGHTS.keyword * input.keyword +
      EMBEDDED_SCORE_WEIGHTS.entity * input.entity +
      EMBEDDED_SCORE_WEIGHTS.similarity * input.similarity +
      EMBEDDED_SCORE_WEIGHTS.graph * input.graph
    );
  }
  return (
    KEYWORD_SCORE_WEIGHTS.keyword * input.keyword +
    KEYWORD_SCORE_WEIGHTS.entity * input.entity +
    KEYWORD_SCORE_WEIGHTS.graph * input.graph
  );
}

export function causalTopologicalOrder<T extends { id: string; score: number }>(
  items: readonly T[],
  causalEdges: readonly { sourceId: string; targetId: string }[],
): T[] {
  if (items.length <= 1) {
    return [...items];
  }
  const byId = new Map(items.map((i) => [i.id, i]));
  const ids = new Set(items.map((i) => i.id));
  const adj = new Map<string, string[]>();
  const indeg = new Map<string, number>();
  for (const item of items) {
    indeg.set(item.id, 0);
    adj.set(item.id, []);
  }
  for (const e of causalEdges) {
    if (ids.has(e.sourceId) && ids.has(e.targetId)) {
      adj.get(e.sourceId)!.push(e.targetId);
      indeg.set(e.targetId, (indeg.get(e.targetId) ?? 0) + 1);
    }
  }

  const ready = items.filter((i) => (indeg.get(i.id) ?? 0) === 0).sort((a, b) => b.score - a.score || a.id.localeCompare(b.id));
  const ordered: T[] = [];
  const remaining = new Map(indeg);

  while (ready.length > 0) {
    const next = ready.shift()!;
    ordered.push(next);
    for (const tgt of adj.get(next.id) ?? []) {
      const d = (remaining.get(tgt) ?? 0) - 1;
      remaining.set(tgt, d);
      if (d === 0) {
        const node = byId.get(tgt)!;
        ready.push(node);
        ready.sort((a, b) => b.score - a.score || a.id.localeCompare(b.id));
      }
    }
  }

  if (ordered.length < items.length) {
    const covered = new Set(ordered.map((i) => i.id));
    ordered.push(...items.filter((i) => !covered.has(i.id)));
  }
  return ordered;
}

export function compareRecallHits(
  a: { score: number; importance: number },
  b: { score: number; importance: number },
): number {
  return b.score - a.score || b.importance - a.importance;
}