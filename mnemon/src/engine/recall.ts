import { GRAPH_ELITE_MIN_COSINE, GRAPH_LAMBDA1, GRAPH_LAMBDA2, RRF_K } from "./constants.js";
import { intentWeights, traversalLimits } from "./intent.js";
import { cosineSimilarity } from "./similarity.js";
import type { EdgeType, RecallIntent } from "../types.js";

export interface RrfSignal {
  id: string;
  signal: "keyword" | "vector" | "time" | "fts";
  rank: number;
}

export interface FusedAnchor {
  id: string;
  score: number;
  matchedVia: "keyword" | "vector" | "time" | "fts" | "hybrid";
  signals: string[];
}

export interface TraversalEdge {
  sourceId: string;
  targetId: string;
  edgeType: EdgeType;
  weight: number;
}

export interface TraversalAnchor {
  id: string;
  score: number;
  matchedVia: string;
}

export interface TraversalState {
  scores: Map<string, number>;
  via: Map<string, string>;
  traversed: number;
  capped: boolean;
}

export function fuseRrf(signals: readonly RrfSignal[]): FusedAnchor[] {
  const byId = new Map<string, { raw: number; signals: Set<string> }>();
  for (const s of signals) {
    const raw = 1 / (RRF_K + s.rank);
    const existing = byId.get(s.id);
    if (existing) {
      existing.raw += raw;
      existing.signals.add(s.signal);
    } else {
      byId.set(s.id, { raw, signals: new Set([s.signal]) });
    }
  }
  let max = 0;
  for (const v of byId.values()) {
    if (v.raw > max) {
      max = v.raw;
    }
  }
  const fused: FusedAnchor[] = [];
  for (const [id, v] of byId) {
    const labels = [...v.signals].sort();
    fused.push({
      id,
      score: max > 0 ? v.raw / max : 0,
      matchedVia: labels.length > 1 ? "hybrid" : (labels[0] as FusedAnchor["matchedVia"]),
      signals: labels,
    });
  }
  fused.sort((a, b) => b.score - a.score || a.id.localeCompare(b.id));
  return fused;
}

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

export function composeFinalScore(input: {
  keyword: number;
  entity: number;
  similarity: number;
  graph: number;
  hasQueryEmbedding: boolean;
}): number {
  if (input.hasQueryEmbedding) {
    return 0.3 * input.keyword + 0.15 * input.entity + 0.35 * input.similarity + 0.2 * input.graph;
  }
  return 0.45 * input.keyword + 0.25 * input.entity + 0.3 * input.graph;
}

export function transitionScore(structural: number, cosine: number): number {
  return GRAPH_LAMBDA1 * structural + GRAPH_LAMBDA2 * Math.max(0, cosine);
}

export function traverseGraph(input: {
  anchors: readonly TraversalAnchor[];
  edges: readonly TraversalEdge[];
  intent: RecallIntent;
  maxCandidates: number;
  queryVector?: readonly number[];
  embeddings?: ReadonlyMap<string, readonly number[]>;
}): TraversalState {
  const limits = traversalLimits(input.intent);
  const weights = intentWeights(input.intent);
  const scores = new Map<string, number>();
  const via = new Map<string, string>();

  for (const a of input.anchors) {
    scores.set(a.id, a.score);
    via.set(a.id, a.matchedVia);
  }

  const adj = new Map<string, TraversalEdge[]>();
  for (const e of input.edges) {
    const a = adj.get(e.sourceId) ?? [];
    a.push(e);
    adj.set(e.sourceId, a);
    const b = adj.get(e.targetId) ?? [];
    b.push(e);
    adj.set(e.targetId, b);
  }

  let traversed = scores.size;
  let capped = traversed >= input.maxCandidates;

  for (const anchor of input.anchors) {
    if (capped) {
      break;
    }
    const local = new Set<string>([anchor.id]);
    let frontier = [{ id: anchor.id, score: scores.get(anchor.id) ?? 0 }];
    for (let depth = 0; depth < limits.depth; depth++) {
      if (frontier.length === 0 || local.size >= limits.visited || traversed >= input.maxCandidates) {
        if (traversed >= input.maxCandidates) {
          capped = true;
        }
        break;
      }
      const next = new Map<string, number>();
      for (const node of frontier) {
        for (const edge of adj.get(node.id) ?? []) {
          const neighbor = edge.sourceId === node.id ? edge.targetId : edge.sourceId;
          if (neighbor === node.id) {
            continue;
          }
          const structural = (weights[edge.edgeType] ?? 0) * edge.weight;
          const neighborVec = input.embeddings?.get(neighbor);
          const cosine =
            input.queryVector && neighborVec ? cosineSimilarity(input.queryVector, neighborVec) : 0;
          const nextScore = node.score + transitionScore(structural, cosine);
          const best = scores.get(neighbor) ?? Number.NEGATIVE_INFINITY;
          if (nextScore > best) {
            scores.set(neighbor, nextScore);
            via.set(neighbor, edge.edgeType);
          }
          if (!local.has(neighbor)) {
            local.add(neighbor);
            next.set(neighbor, nextScore);
          } else if (next.has(neighbor) && nextScore > (next.get(neighbor) ?? Number.NEGATIVE_INFINITY)) {
            next.set(neighbor, nextScore);
          }
        }
      }
      const ranked = [...next.entries()]
        .map(([id, score]) => ({ id, score }))
        .sort((a, b) => b.score - a.score || a.id.localeCompare(b.id));
      frontier = ranked.slice(0, limits.beam);
      traversed = scores.size;
    }
  }

  return { scores, via, traversed: scores.size, capped };
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
