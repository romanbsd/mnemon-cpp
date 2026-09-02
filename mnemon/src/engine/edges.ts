import {
  CAUSAL_MIN_OVERLAP,
  CAUSES_PHRASES,
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

export function hasCausalPhrase(text: string): boolean {
  return containsPhrase(text, CAUSAL_PHRASES);
}

export function classifyCausalSubtype(text: string): "prevents" | "enables" | "causes" {
  if (containsPhrase(text, PREVENTS_PHRASES)) {
    return "prevents";
  }
  if (containsPhrase(text, ENABLES_PHRASES)) {
    return "enables";
  }
  if (containsPhrase(text, CAUSES_PHRASES)) {
    return "causes";
  }
  return "causes";
}

export function causalOverlap(a: Set<string>, b: Set<string>): number {
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

export function temporalProximityWeight(hoursDiff: number): number {
  return 1 / (1 + Math.abs(hoursDiff));
}

export function hoursDifference(a: Date, b: Date): number {
  return Math.abs(a.getTime() - b.getTime()) / 3_600_000;
}

export function buildTemporalEdges(input: {
  newId: string;
  newCreatedAt: Date;
  now: Date;
  latestSameSource?: { id: string };
  recentWithin24h: readonly { id: string; createdAt: Date }[];
}): NewEdge[] {
  const edges: NewEdge[] = [];
  const createdAt = input.now.toISOString();
  if (input.latestSameSource) {
    edges.push(
      {
        sourceId: input.latestSameSource.id,
        targetId: input.newId,
        edgeType: "temporal",
        weight: 1,
        metadata: { sub_type: "backbone", direction: "precedes" },
      },
      {
        sourceId: input.newId,
        targetId: input.latestSameSource.id,
        edgeType: "temporal",
        weight: 1,
        metadata: { sub_type: "backbone", direction: "succeeds" },
      },
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
      {
        sourceId: input.newId,
        targetId: near.id,
        edgeType: "temporal",
        weight,
        metadata: { sub_type: "proximity", hours_diff: hoursDiff },
      },
      {
        sourceId: near.id,
        targetId: input.newId,
        edgeType: "temporal",
        weight,
        metadata: { sub_type: "proximity", hours_diff: hoursDiff },
      },
    );
  }
  void createdAt;
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
    edges.push(
      {
        sourceId: input.newId,
        targetId: pair.targetId,
        edgeType: "entity",
        weight: 1,
        metadata: { entity: pair.entity },
      },
      {
        sourceId: pair.targetId,
        targetId: input.newId,
        edgeType: "entity",
        weight: 1,
        metadata: { entity: pair.entity },
      },
    );
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
      {
        sourceId: input.newId,
        targetId: n.id,
        edgeType: "semantic",
        weight: n.cosine,
        metadata: { created_by: "auto", cosine },
      },
      {
        sourceId: n.id,
        targetId: input.newId,
        edgeType: "semantic",
        weight: n.cosine,
        metadata: { created_by: "auto", cosine },
      },
    );
  }
  return edges;
}

export function countEdgesByType(edges: readonly NewEdge[]): Record<EdgeType, number> {
  const counts: Record<EdgeType, number> = { temporal: 0, semantic: 0, causal: 0, entity: 0 };
  for (const edge of edges) {
    counts[edge.edgeType]++;
  }
  return counts;
}
