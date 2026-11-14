import {
  CAUSAL_MIN_OVERLAP,
  CAUSAL_PHRASES,
  ENABLES_PHRASES,
  PREVENTS_PHRASES,
  SEMANTIC_EDGE_MIN_COSINE,
} from "./constants.js";
import { tokenize } from "./tokenize.js";
import type { EdgeType } from "../types.js";

export interface NewEdge {
  sourceId: string;
  targetId: string;
  edgeType: EdgeType;
  weight: number;
  metadata: Record<string, string>;
}

function containsPhrase(text: string, phrases: readonly string[]): boolean {
  const lower = text.toLowerCase();
  return phrases.some((p) => lower.includes(p.toLowerCase()));
}

function hasCausalPhrase(text: string): boolean {
  return containsPhrase(text, CAUSAL_PHRASES);
}

function classifyCausalSubtype(text: string): "prevents" | "enables" | "causes" {
  if (containsPhrase(text, PREVENTS_PHRASES)) {
    return "prevents";
  }
  if (containsPhrase(text, ENABLES_PHRASES)) {
    return "enables";
  }
  return "causes";
}

function causalOverlap(a: Set<string>, b: Set<string>): number {
  if (a.size === 0 || b.size === 0) {
    return 0;
  }
  let inter = 0;
  const smaller = a.size <= b.size ? a : b;
  const larger = a.size <= b.size ? b : a;
  for (const t of smaller) {
    if (larger.has(t)) {
      inter++;
    }
  }
  return inter / Math.max(a.size, b.size);
}

function temporalProximityWeight(hoursDiff: number): number {
  return 1 / (1 + Math.abs(hoursDiff));
}

function hoursDifference(a: Date, b: Date): number {
  return Math.abs(a.getTime() - b.getTime()) / 3_600_000;
}

function bidirectional(
  a: string,
  b: string,
  edgeType: EdgeType,
  weight: number,
  metadata: Record<string, string>,
  reverseMetadata: Record<string, string> = metadata,
): NewEdge[] {
  return [
    { sourceId: a, targetId: b, edgeType, weight, metadata },
    { sourceId: b, targetId: a, edgeType, weight, metadata: reverseMetadata },
  ];
}

export function buildTemporalEdges(input: {
  newId: string;
  newCreatedAt: Date;
  latestSameSource?: { id: string };
  recentWithin24h: readonly { id: string; createdAt: Date }[];
}): NewEdge[] {
  const edges: NewEdge[] = [];
  if (input.latestSameSource) {
    edges.push(
      ...bidirectional(
        input.latestSameSource.id,
        input.newId,
        "temporal",
        1,
        { sub_type: "backbone", direction: "precedes" },
        { sub_type: "backbone", direction: "succeeds" },
      ),
    );
  }
  const backboneId = input.latestSameSource?.id;
  for (const near of input.recentWithin24h) {
    if (near.id === backboneId || near.id === input.newId) {
      continue;
    }
    const hours = hoursDifference(input.newCreatedAt, near.createdAt);
    const weight = temporalProximityWeight(hours);
    const hoursDiff = hours.toFixed(2);
    edges.push(
      ...bidirectional(input.newId, near.id, "temporal", weight, { sub_type: "proximity", hours_diff: hoursDiff }),
    );
  }
  return edges;
}

export function buildEntityEdges(input: {
  newId: string;
  pairs: readonly { entity: string; targetId: string }[];
}): NewEdge[] {
  const edges: NewEdge[] = [];
  for (const pair of input.pairs) {
    if (pair.targetId === input.newId) {
      continue;
    }
    edges.push(...bidirectional(input.newId, pair.targetId, "entity", 1, { entity: pair.entity }));
  }
  return edges;
}

export function buildCausalEdges(input: {
  newId: string;
  newContent: string;
  previous: readonly { id: string; content: string }[];
}): NewEdge[] {
  const newTokens = tokenize(input.newContent);
  if (newTokens.size === 0) {
    return [];
  }
  const newHas = hasCausalPhrase(input.newContent);
  const edges: NewEdge[] = [];
  for (const prev of input.previous) {
    const prevHas = hasCausalPhrase(prev.content);
    if (!newHas && !prevHas) {
      continue;
    }
    const overlap = causalOverlap(newTokens, tokenize(prev.content));
    if (overlap < CAUSAL_MIN_OVERLAP) {
      continue;
    }
    let sourceId = prev.id;
    let targetId = input.newId;
    if (!newHas && prevHas) {
      sourceId = input.newId;
      targetId = prev.id;
    }
    edges.push({
      sourceId,
      targetId,
      edgeType: "causal",
      weight: overlap,
      metadata: {
        overlap: overlap.toFixed(4),
        sub_type: classifyCausalSubtype(`${input.newContent} ${prev.content}`),
      },
    });
  }
  return edges;
}

export function buildSemanticEdges(input: {
  newId: string;
  neighbors: readonly { id: string; cosine: number }[];
}): NewEdge[] {
  const edges: NewEdge[] = [];
  for (const n of input.neighbors) {
    if (n.cosine < SEMANTIC_EDGE_MIN_COSINE || n.id === input.newId) {
      continue;
    }
    const cosine = n.cosine.toFixed(4);
    edges.push(
      ...bidirectional(input.newId, n.id, "semantic", n.cosine, { created_by: "auto", cosine }),
    );
  }
  return edges;
}

export function emptyEdgeCounts(): Record<EdgeType, number> {
  return { temporal: 0, semantic: 0, causal: 0, entity: 0 };
}

export function countEdgesByType(edges: readonly NewEdge[]): Record<EdgeType, number> {
  const counts = emptyEdgeCounts();
  for (const edge of edges) {
    counts[edge.edgeType]++;
  }
  return counts;
}
