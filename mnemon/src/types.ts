export type InsightCategory =
  | "preference"
  | "decision"
  | "fact"
  | "insight"
  | "context"
  | "general";

export type EdgeType = "temporal" | "semantic" | "causal" | "entity";
export type RecallIntent = "WHY" | "WHEN" | "ENTITY" | "GENERAL";
export type RememberAction = "added" | "skipped";
export type DiffSuggestion = "ADD" | "DUPLICATE" | "CONFLICT" | "UPDATE";
export type AlgorithmVersion = "mnemon-ts-v1";

export const INSIGHT_CATEGORIES: readonly InsightCategory[] = [
  "preference",
  "decision",
  "fact",
  "insight",
  "context",
  "general",
];

export const EDGE_TYPES: readonly EdgeType[] = ["temporal", "semantic", "causal", "entity"];

export interface Insight {
  id: string;
  content: string;
  category: InsightCategory;
  importance: 1 | 2 | 3 | 4 | 5;
  tags: string[];
  entities: string[];
  source: string;
  accessCount: number;
  storedAt: string;
  createdAt: string;
  updatedAt: string;
  deletedAt?: string;
}

export interface Edge {
  sourceId: string;
  targetId: string;
  edgeType: EdgeType;
  weight: number;
  metadata: Record<string, string>;
  createdAt: string;
}

export interface RememberInput {
  content: string;
  category?: InsightCategory;
  importance?: 1 | 2 | 3 | 4 | 5;
  tags?: string[];
  entities?: string[];
  source?: string;
  createdAt?: string;
  deduplicate?: boolean;
}

export interface SimilarMemory {
  id: string;
  content: string;
  category: InsightCategory;
  tokenSimilarity: number;
  cosineSimilarity: number;
}

export interface DiffMatch {
  id: string;
  content: string;
  tokenSimilarity: number;
  cosineSimilarity: number;
  similarity: number;
  suggestion: DiffSuggestion;
}

export interface RememberResult {
  action: RememberAction;
  insight: Insight;
  duplicateOf?: string;
  /** Go Diff class. Informational only — this library never auto-replaces. */
  suggestion: DiffSuggestion;
  diff: DiffMatch[];
  semanticCandidates: SimilarMemory[];
  edgeCounts: Record<EdgeType, number>;
}

export interface RecallInput {
  query: string;
  limit?: number;
  intent?: RecallIntent;
  source?: string;
  /** Compact discovery projection: flatten whitespace and truncate content. */
  brief?: boolean;
  /** Maximum Unicode code points per brief excerpt. Default 240. Must be > 0. */
  excerptChars?: number;
}

export interface RecallSignals {
  keyword: number;
  entity: number;
  similarity: number;
  graph: number;
}

export interface RecallHit {
  insight: Insight;
  score: number;
  intent: RecallIntent;
  matchedVia: "keyword" | "vector" | "time" | "fts" | "hybrid" | EdgeType;
  signals: RecallSignals;
  /** Present when `RecallInput.brief` is true. Full text is available via `get(id)`. */
  excerpt?: string;
}

export interface RecallResult {
  results: RecallHit[];
  meta: {
    intent: RecallIntent;
    intentSource: "auto" | "override";
    anchorCount: number;
    traversed: number;
    hint?: "sparse_results";
    algorithmVersion: AlgorithmVersion;
  };
}

export interface LinkInput {
  sourceId: string;
  targetId: string;
  edgeType: EdgeType;
  weight?: number;
  metadata?: Record<string, string>;
}

export interface ForgetResult {
  forgotten: boolean;
  id: string;
}

export interface SearchInput {
  query: string;
  limit?: number;
  source?: string;
}

export interface ListInput {
  limit?: number;
  source?: string;
  category?: InsightCategory;
  since?: string;
  until?: string;
}

export interface SearchHit {
  insight: Insight;
  score: number;
  matchedVia: "keyword" | "fts" | "hybrid";
  signals: { keyword: number; fts: number };
}

export interface SearchResult {
  results: SearchHit[];
}

export interface LogInput {
  limit?: number;
  operation?: string;
}

export interface OpLogEntry {
  id: string;
  operation: string;
  insightId?: string;
  detail: Record<string, unknown>;
  createdAt: string;
}

export interface MnemonStatus {
  schema: string;
  algorithmVersion: AlgorithmVersion;
  insights: number;
  embeddings: number;
  edges: number;
  embeddingModel?: string;
  embeddingDimensions?: number;
}

export interface RelatedInsight extends Insight {
  depth: number;
  viaEdgeType?: EdgeType;
}

export interface Mnemon {
  initialize(): Promise<void>;
  remember(input: RememberInput): Promise<RememberResult>;
  recall(input: RecallInput): Promise<RecallResult>;
  link(input: LinkInput): Promise<Edge>;
  related(
    id: string,
    options?: { maxDepth?: number; limit?: number; edgeType?: EdgeType },
  ): Promise<RelatedInsight[]>;
  forget(id: string): Promise<ForgetResult>;
  get(id: string): Promise<Insight | null>;
  search(input: SearchInput): Promise<SearchResult>;
  list(input?: ListInput): Promise<Insight[]>;
  log(input?: LogInput): Promise<OpLogEntry[]>;
  status(): Promise<MnemonStatus>;
  close(): Promise<void>;
}
