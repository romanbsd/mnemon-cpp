import type { Edge, Insight } from "../types.js";
import type { EdgeRecord, InsightRecord } from "./schema.js";

export function asDate(value: unknown): Date {
  if (value instanceof Date) {
    return value;
  }
  return new Date(String(value));
}

function asStringArray(value: unknown): string[] {
  if (Array.isArray(value)) {
    return value.map(String);
  }
  return [];
}

function asStringMap(value: unknown): Record<string, string> {
  if (value && typeof value === "object" && !Array.isArray(value)) {
    const out: Record<string, string> = {};
    for (const [k, v] of Object.entries(value as Record<string, unknown>)) {
      out[k] = String(v);
    }
    return out;
  }
  return {};
}

function asEmbedding(value: unknown): number[] | null {
  if (value == null) {
    return null;
  }
  if (Array.isArray(value)) {
    return value.map(Number);
  }
  return null;
}

export function mapInsightRow(row: Record<string, unknown>): InsightRecord {
  return {
    id: String(row.id),
    content: String(row.content),
    normalizedContent: String(row.normalized_content),
    contentHash: String(row.content_hash),
    searchTokens: asStringArray(row.search_tokens),
    category: row.category as InsightRecord["category"],
    importance: Number(row.importance) as InsightRecord["importance"],
    tags: asStringArray(row.tags),
    entities: asStringArray(row.entities),
    source: String(row.source),
    accessCount: Number(row.access_count),
    storedAt: asDate(row.stored_at),
    createdAt: asDate(row.created_at),
    updatedAt: asDate(row.updated_at),
    deletedAt: row.deleted_at == null ? null : asDate(row.deleted_at),
    lastAccessedAt: row.last_accessed_at == null ? null : asDate(row.last_accessed_at),
    embedding: asEmbedding(row.embedding),
    effectiveImportance: Number(row.effective_importance),
  };
}

export function mapEdgeRow(row: Record<string, unknown>): EdgeRecord {
  return {
    sourceId: String(row.source_id),
    targetId: String(row.target_id),
    edgeType: row.edge_type as EdgeRecord["edgeType"],
    weight: Number(row.weight),
    metadata: asStringMap(row.metadata),
    createdAt: asDate(row.created_at),
  };
}

export function toPublicInsight(record: InsightRecord): Insight {
  const insight: Insight = {
    id: record.id,
    content: record.content,
    category: record.category,
    importance: record.importance,
    tags: record.tags,
    entities: record.entities,
    source: record.source,
    accessCount: record.accessCount,
    storedAt: record.storedAt.toISOString(),
    createdAt: record.createdAt.toISOString(),
    updatedAt: record.updatedAt.toISOString(),
  };
  if (record.deletedAt) {
    insight.deletedAt = record.deletedAt.toISOString();
  }
  return insight;
}

export function toPublicEdge(record: EdgeRecord): Edge {
  return {
    sourceId: record.sourceId,
    targetId: record.targetId,
    edgeType: record.edgeType,
    weight: record.weight,
    metadata: record.metadata,
    createdAt: record.createdAt.toISOString(),
  };
}
