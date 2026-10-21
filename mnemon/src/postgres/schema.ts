import type { EdgeType, InsightCategory } from "../types.js";

export interface InsightRecord {
  id: string;
  content: string;
  normalizedContent: string;
  contentHash: string;
  searchTokens: string[];
  category: InsightCategory;
  importance: 1 | 2 | 3 | 4 | 5;
  tags: string[];
  entities: string[];
  source: string;
  accessCount: number;
  storedAt: Date;
  createdAt: Date;
  updatedAt: Date;
  deletedAt: Date | null;
  lastAccessedAt: Date | null;
  embedding: number[] | null;
  effectiveImportance: number;
}

export interface NewInsightRecord {
  id: string;
  content: string;
  normalizedContent: string;
  contentHash: string;
  searchTokens: string[];
  category: InsightCategory;
  importance: 1 | 2 | 3 | 4 | 5;
  tags: string[];
  entities: string[];
  source: string;
  createdAt: Date;
  updatedAt: Date;
  embedding: number[] | null;
  effectiveImportance: number;
}

export interface EdgeRecord {
  sourceId: string;
  targetId: string;
  edgeType: EdgeType;
  weight: number;
  metadata: Record<string, string>;
  createdAt: Date;
}

export type NewEdgeRecord = EdgeRecord;

export interface KeywordHit {
  id: string;
  keywordScore: number;
}

export interface VectorHit {
  id: string;
  cosineSimilarity: number;
}

export interface AnchorHit {
  id: string;
  score: number;
  matchedVia: "keyword" | "vector" | "time" | "fts" | "hybrid";
  signals: string[];
}

export interface StaticSignalHit {
  id: string;
  keyword: number;
  entity: number;
  similarity: number;
}

export interface ScoredInsight {
  insight: InsightRecord;
  signals: StaticSignalHit;
}

export interface EdgeContext {
  latestSameSource?: { id: string; content: string; createdAt: Date };
  recentWithin24h: Array<{ id: string; content: string; createdAt: Date }>;
  causalPrevious: Array<{ id: string; content: string }>;
  entityPairs: Array<{ entity: string; targetId: string }>;
}

export interface RelatedWalkHit {
  id: string;
  depth: number;
  weight: number;
  viaEdgeType?: string;
}

export interface GraphWalkHit {
  id: string;
  score: number;
  via: string;
}

export interface SearchStoreHit {
  id: string;
  keyword: number;
  fts: number;
}

export interface OpLogRecord {
  id: string;
  operation: string;
  insightId: string | null;
  detail: Record<string, unknown>;
  createdAt: Date;
}

export interface StoreCounts {
  insights: number;
  embeddings: number;
  edges: number;
}
