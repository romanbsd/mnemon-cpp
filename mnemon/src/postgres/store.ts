import type { Pool, PoolClient } from "pg";
import pgvector from "pgvector";

import { quoteIdent } from "../config.js";
import {
  ANCHOR_TOP_K,
  CAUSAL_LOOKBACK,
  GRAPH_LAMBDA1,
  GRAPH_LAMBDA2,
  MAX_ENTITY_LINKS,
  MAX_TEMPORAL_PROXIMITY,
  MAX_TOTAL_ENTITY_EDGES,
  VECTOR_ANCHOR_MIN_COSINE,
} from "../engine/constants.js";
import { intentWeights, traversalLimits } from "../engine/intent.js";
import type { RecallIntent } from "../types.js";
import { uniquePreserveOrder } from "../engine/normalize.js";
import { MnemonDatabaseError, MnemonNotFoundError } from "../errors.js";
import { wrapDatabaseError, withTransaction } from "./transaction.js";
import { mapEdgeRow, mapInsightRow } from "./row-mappers.js";
import type {
  AnchorHit,
  EdgeContext,
  EdgeRecord,
  GraphWalkHit,
  InsightRecord,
  KeywordHit,
  NewEdgeRecord,
  NewInsightRecord,
  OpLogRecord,
  RelatedWalkHit,
  ScoredInsight,
  SearchStoreHit,
  StoreCounts,
  VectorHit,
} from "./schema.js";

const UNIQUE_VIOLATION = "23505";

const TOKEN_OVERLAP = `
      CROSS JOIN LATERAL (
          SELECT count(*) AS count
          FROM unnest(i.search_tokens) AS token
          WHERE token = ANY(q.tokens)
      ) AS matched`;

const INSIGHT_COLS = `
          id, content, normalized_content, content_hash, search_tokens, category, importance,
          tags, entities, source, access_count, stored_at, created_at, updated_at, deleted_at,
          last_accessed_at, effective_importance`;

function insightSelect(embedding: boolean): string {
  return embedding ? `${INSIGHT_COLS}, embedding` : INSIGHT_COLS;
}

export class UniqueViolationError extends Error {
  readonly code = UNIQUE_VIOLATION;
}

export function isUniqueViolation(error: unknown): boolean {
  if (error instanceof UniqueViolationError) {
    return true;
  }
  if (error instanceof MnemonDatabaseError && error.code === UNIQUE_VIOLATION) {
    return true;
  }
  return error instanceof Error && error.cause instanceof UniqueViolationError;
}

export interface MnemonStore {
  withTransaction<T>(fn: (tx: MnemonStoreTx) => Promise<T>): Promise<T>;
  getActiveInsight(id: string): Promise<InsightRecord | null>;
  findExactDuplicate(contentHash: string): Promise<InsightRecord | null>;
  findKeywordCandidates(queryTokens: readonly string[], limit: number): Promise<KeywordHit[]>;
  nearestEmbeddings(
    vector: readonly number[],
    options: { excludeId?: string; limit: number; minCosine?: number },
  ): Promise<VectorHit[]>;
  selectRecallAnchors(input: {
    queryTokens: readonly string[];
    queryVector?: readonly number[];
    limitPerSignal: number;
    source?: string;
  }): Promise<AnchorHit[]>;
  searchInsights(input: {
    query: string;
    queryTokens: readonly string[];
    limit: number;
    source?: string;
  }): Promise<SearchStoreHit[]>;
  listOps(input: { limit: number; operation?: string }): Promise<OpLogRecord[]>;
  listInsights(input: {
    limit: number;
    source?: string;
    category?: string;
    since?: Date;
    until?: Date;
  }): Promise<InsightRecord[]>;
  counts(): Promise<StoreCounts>;
  loadScoredInsights(input: {
    ids: readonly string[];
    queryTokens: readonly string[];
    queryEntities: readonly string[];
    queryVector?: readonly number[];
  }): Promise<ScoredInsight[]>;
  getEdgesForNodeIds(ids: readonly string[]): Promise<EdgeRecord[]>;
  listKnownEntities(): Promise<string[]>;
  walkRecallGraph(input: {
    anchors: readonly AnchorHit[];
    intent: RecallIntent;
    queryVector?: readonly number[];
    maxCandidates: number;
  }): Promise<GraphWalkHit[]>;
  walkRelated(input: {
    startId: string;
    maxDepth: number;
    limit: number;
    edgeType?: string;
  }): Promise<RelatedWalkHit[]>;
  loadEdgeContext(input: {
    excludeId: string;
    source: string;
    since: Date;
    entities: readonly string[];
  }): Promise<EdgeContext>;
  getSetting(key: string): Promise<unknown | undefined>;
  loadInsightsByIds(ids: readonly string[], options?: { embedding?: boolean }): Promise<InsightRecord[]>;
}

export interface MnemonStoreTx {
  insertInsight(record: NewInsightRecord): Promise<InsightRecord>;
  upsertEdges(edges: readonly NewEdgeRecord[]): Promise<EdgeRecord[]>;
  appendOp(operation: string, insightId: string | null, detail: Record<string, unknown>, at: Date): Promise<void>;
  setEffectiveImportance(id: string, value: number): Promise<void>;
  establishEmbeddingSettings(dimensions: number, model: string, at: Date): Promise<void>;
  linkAndLog(edge: NewEdgeRecord, at: Date): Promise<EdgeRecord>;
  forgetAndLog(id: string, at: Date): Promise<boolean>;
  incrementAccess(ids: readonly string[], at: Date): Promise<void>;
}

function vec(values: readonly number[]): string {
  const sql = pgvector.toSql([...values]);
  if (sql == null) {
    throw new Error("invalid embedding vector");
  }
  return sql;
}

export class PostgresMnemonStore implements MnemonStore {
  private readonly s: string;

  constructor(
    private readonly pool: Pool,
    schema: string,
  ) {
    this.s = quoteIdent(schema);
  }

  async withTransaction<T>(fn: (tx: MnemonStoreTx) => Promise<T>): Promise<T> {
    return withTransaction(this.pool, async (client) => fn(new PostgresMnemonStoreTx(client, this.s)));
  }

  async getActiveInsight(id: string): Promise<InsightRecord | null> {
    const result = await this.pool.query(
      `SELECT ${insightSelect(false)} FROM ${this.s}.insights WHERE id = $1::uuid AND deleted_at IS NULL`,
      [id],
    );
    const row = result.rows[0] as Record<string, unknown> | undefined;
    return row ? mapInsightRow(row) : null;
  }

  async loadInsightsByIds(ids: readonly string[], options?: { embedding?: boolean }): Promise<InsightRecord[]> {
    if (ids.length === 0) {
      return [];
    }
    const result = await this.pool.query(
      `SELECT ${insightSelect(options?.embedding === true)} FROM ${this.s}.insights WHERE id = ANY($1::uuid[]) AND deleted_at IS NULL`,
      [ids],
    );
    return result.rows.map((row) => mapInsightRow(row as Record<string, unknown>));
  }

  async findExactDuplicate(contentHash: string): Promise<InsightRecord | null> {
    const result = await this.pool.query(
      `SELECT ${insightSelect(false)} FROM ${this.s}.insights WHERE content_hash = $1 AND deleted_at IS NULL`,
      [contentHash],
    );
    const row = result.rows[0] as Record<string, unknown> | undefined;
    return row ? mapInsightRow(row) : null;
  }

  async findKeywordCandidates(queryTokens: readonly string[], limit: number): Promise<KeywordHit[]> {
    if (queryTokens.length === 0) {
      return [];
    }
    const result = await this.pool.query(
      `
      WITH q AS (
          SELECT $1::text[] AS tokens, cardinality($1::text[]) AS token_count
      )
      SELECT i.id,
             matched.count::double precision / NULLIF(q.token_count, 0) AS keyword_score
      FROM ${this.s}.insights AS i
      CROSS JOIN q
      ${TOKEN_OVERLAP}
      WHERE i.deleted_at IS NULL
        AND q.token_count > 0
        AND i.search_tokens && q.tokens
      ORDER BY keyword_score DESC, i.importance DESC, i.created_at DESC, i.id ASC
      LIMIT $2
      `,
      [queryTokens, limit],
    );
    return result.rows.map((row) => ({
      id: String(row.id),
      keywordScore: Number(row.keyword_score),
    }));
  }

  async nearestEmbeddings(
    vector: readonly number[],
    options: { excludeId?: string; limit: number; minCosine?: number },
  ): Promise<VectorHit[]> {
    const result = await this.pool.query(
      `
      SELECT id, 1 - (embedding <=> $1::vector) AS cosine_similarity
      FROM ${this.s}.insights
      WHERE deleted_at IS NULL
        AND embedding IS NOT NULL
        AND ($2::uuid IS NULL OR id <> $2::uuid)
        AND ($4::float8 IS NULL OR 1 - (embedding <=> $1::vector) >= $4)
      ORDER BY embedding <=> $1::vector, id ASC
      LIMIT $3
      `,
      [vec(vector), options.excludeId ?? null, options.limit, options.minCosine ?? null],
    );
    return result.rows.map((row) => ({
      id: String(row.id),
      cosineSimilarity: Number(row.cosine_similarity),
    }));
  }

  async selectRecallAnchors(input: {
    queryTokens: readonly string[];
    queryVector?: readonly number[];
    limitPerSignal: number;
    source?: string;
  }): Promise<AnchorHit[]> {
    const limit = input.limitPerSignal || ANCHOR_TOP_K;
    const result = await this.pool.query(
      `
      WITH
      q AS (
          SELECT $1::text[] AS tokens,
                 cardinality($1::text[]) AS token_count,
                 $2::vector AS embedding
      ),
      keyword_scored AS (
          SELECT i.id,
                 matched.count::double precision / NULLIF(q.token_count, 0) AS score
          FROM ${this.s}.insights AS i
          CROSS JOIN q
          ${TOKEN_OVERLAP}
          WHERE i.deleted_at IS NULL
            AND q.token_count > 0
            AND i.search_tokens && q.tokens
            AND ($4::text IS NULL OR i.source = $4)
      ),
      keyword_ranked AS (
          SELECT id, row_number() OVER (ORDER BY score DESC, id ASC) AS rank
          FROM keyword_scored
          ORDER BY score DESC, id ASC
          LIMIT $3
      ),
      vector_ranked AS (
          SELECT i.id,
                 row_number() OVER (ORDER BY i.embedding <=> q.embedding, i.id ASC) AS rank
          FROM ${this.s}.insights AS i
          CROSS JOIN q
          WHERE q.embedding IS NOT NULL
            AND i.deleted_at IS NULL
            AND i.embedding IS NOT NULL
            AND 1 - (i.embedding <=> q.embedding) > ${VECTOR_ANCHOR_MIN_COSINE}
            AND ($4::text IS NULL OR i.source = $4)
          ORDER BY i.embedding <=> q.embedding, i.id ASC
          LIMIT $3
      ),
      time_ranked AS (
          SELECT id, row_number() OVER (ORDER BY created_at DESC, id ASC) AS rank
          FROM ${this.s}.insights
          WHERE deleted_at IS NULL
            AND ($4::text IS NULL OR source = $4)
          ORDER BY created_at DESC, id ASC
          LIMIT $3
      ),
      signals AS (
          SELECT id, rank, 'keyword'::text AS signal FROM keyword_ranked
          UNION ALL
          SELECT id, rank, 'vector'::text AS signal FROM vector_ranked
          UNION ALL
          SELECT id, rank, 'time'::text AS signal FROM time_ranked
      ),
      fused AS (
          SELECT id,
                 sum(1.0 / (60.0 + rank)) AS raw_score,
                 array_agg(DISTINCT signal ORDER BY signal) AS signals
          FROM signals
          GROUP BY id
      ),
      normalized AS (
          SELECT id, raw_score / max(raw_score) OVER () AS score, signals
          FROM fused
      )
      SELECT n.id, n.score,
             CASE WHEN cardinality(n.signals) > 1 THEN 'hybrid' ELSE n.signals[1] END AS matched_via,
             n.signals
      FROM normalized AS n
      ORDER BY n.score DESC, n.id ASC
      `,
      [input.queryTokens, input.queryVector ? vec(input.queryVector) : null, limit, input.source ?? null],
    );
    return result.rows.map(mapAnchor);
  }

  async searchInsights(input: {
    query: string;
    queryTokens: readonly string[];
    limit: number;
    source?: string;
  }): Promise<SearchStoreHit[]> {
    const result = await this.pool.query(
      `
      WITH q AS (
          SELECT $1::text[] AS tokens,
                 cardinality($1::text[]) AS token_count,
                 plainto_tsquery('english', $2) AS fts
      )
      SELECT i.id,
             COALESCE(matched.count, 0)::double precision / NULLIF(q.token_count, 0) AS keyword,
             CASE WHEN q.fts <> ''::tsquery THEN ts_rank_cd(i.search_tsv, q.fts) ELSE 0 END AS fts
      FROM ${this.s}.insights AS i
      CROSS JOIN q
      LEFT JOIN LATERAL (
          SELECT count(*) AS count
          FROM unnest(i.search_tokens) AS token
          WHERE token = ANY(q.tokens)
      ) AS matched ON true
      WHERE i.deleted_at IS NULL
        AND ($4::text IS NULL OR i.source = $4)
        AND (
          (q.token_count > 0 AND i.search_tokens && q.tokens)
          OR (q.fts <> ''::tsquery AND i.search_tsv @@ q.fts)
        )
      ORDER BY
        (COALESCE(matched.count, 0)::double precision / NULLIF(q.token_count, 0) * 0.45
         + CASE WHEN q.fts <> ''::tsquery THEN ts_rank_cd(i.search_tsv, q.fts) ELSE 0 END * 0.55) DESC NULLS LAST,
        i.id ASC
      LIMIT $3
      `,
      [input.queryTokens, input.query, input.limit, input.source ?? null],
    );
    return result.rows.map((row) => ({
      id: String(row.id),
      keyword: Number(row.keyword ?? 0) || 0,
      fts: Number(row.fts ?? 0) || 0,
    }));
  }

  async listInsights(input: {
    limit: number;
    source?: string;
    category?: string;
    since?: Date;
    until?: Date;
  }): Promise<InsightRecord[]> {
    const result = await this.pool.query(
      `
      SELECT ${insightSelect(false)}
      FROM ${this.s}.insights
      WHERE deleted_at IS NULL
        AND ($2::text IS NULL OR source = $2)
        AND ($3::text IS NULL OR category = $3)
        AND ($4::timestamptz IS NULL OR created_at >= $4)
        AND ($5::timestamptz IS NULL OR created_at <= $5)
      ORDER BY created_at DESC, id ASC
      LIMIT $1
      `,
      [input.limit, input.source ?? null, input.category ?? null, input.since ?? null, input.until ?? null],
    );
    return result.rows.map((row) => mapInsightRow(row as Record<string, unknown>));
  }

  async listOps(input: { limit: number; operation?: string }): Promise<OpLogRecord[]> {
    const result = await this.pool.query(
      `
      SELECT id, operation, insight_id, detail, created_at
      FROM ${this.s}.oplog
      WHERE ($2::text IS NULL OR operation = $2)
      ORDER BY created_at DESC, id DESC
      LIMIT $1
      `,
      [input.limit, input.operation ?? null],
    );
    return result.rows.map((row) => ({
      id: String(row.id),
      operation: String(row.operation),
      insightId: row.insight_id == null ? null : String(row.insight_id),
      detail: (row.detail ?? {}) as Record<string, unknown>,
      createdAt: new Date(String(row.created_at)),
    }));
  }

  async counts(): Promise<StoreCounts> {
    const result = await this.pool.query(
      `
      SELECT
        (SELECT count(*)::int FROM ${this.s}.insights WHERE deleted_at IS NULL) AS insights,
        (SELECT count(*)::int FROM ${this.s}.insights WHERE deleted_at IS NULL AND embedding IS NOT NULL) AS embeddings,
        (SELECT count(*)::int FROM ${this.s}.edges) AS edges
      `,
    );
    const row = result.rows[0] ?? {};
    return {
      insights: Number(row.insights ?? 0),
      embeddings: Number(row.embeddings ?? 0),
      edges: Number(row.edges ?? 0),
    };
  }

  async loadScoredInsights(input: {
    ids: readonly string[];
    queryTokens: readonly string[];
    queryEntities: readonly string[];
    queryVector?: readonly number[];
  }): Promise<ScoredInsight[]> {
    if (input.ids.length === 0) {
      return [];
    }
    const queryEntities = uniquePreserveOrder(input.queryEntities.map((e) => e.toLowerCase()).filter((e) => e.length > 0));
    const result = await this.pool.query(
      `
      SELECT ${insightSelect(false)},
             COALESCE(tok.count, 0)::double precision / NULLIF(cardinality($2::text[]), 0) AS keyword,
             COALESCE(ent.count, 0)::double precision / GREATEST(1, cardinality($3::text[])) AS entity,
             CASE
               WHEN $4::vector IS NULL OR i.embedding IS NULL THEN 0
               ELSE GREATEST(0, 1 - (i.embedding <=> $4::vector))
             END AS similarity
      FROM ${this.s}.insights AS i
      LEFT JOIN LATERAL (
          SELECT count(*) AS count
          FROM unnest(i.search_tokens) AS token
          WHERE token = ANY($2::text[])
      ) AS tok ON true
      LEFT JOIN LATERAL (
          SELECT count(*) AS count
          FROM jsonb_array_elements_text(i.entities) AS e
          WHERE lower(e) = ANY($3::text[])
      ) AS ent ON true
      WHERE i.deleted_at IS NULL
        AND i.id = ANY($1::uuid[])
      `,
      [input.ids, input.queryTokens, queryEntities, input.queryVector ? vec(input.queryVector) : null],
    );
    return result.rows.map((row) => {
      const rec = row as Record<string, unknown>;
      return {
        insight: mapInsightRow(rec),
        signals: {
          id: String(rec.id),
          keyword: Number(rec.keyword ?? 0) || 0,
          entity: Number(rec.entity ?? 0) || 0,
          similarity: Number(rec.similarity ?? 0) || 0,
        },
      };
    });
  }

  async walkRelated(input: {
    startId: string;
    maxDepth: number;
    limit: number;
    edgeType?: string;
  }): Promise<RelatedWalkHit[]> {
    const seen = new Set<string>([input.startId]);
    let frontier = [input.startId];
    const hits: RelatedWalkHit[] = [];
    for (let depth = 1; depth <= input.maxDepth && frontier.length > 0; depth++) {
      const hop = await this.pool.query(
        `
        SELECT DISTINCT ON (neigh.id) neigh.id, e.weight, e.edge_type AS via
        FROM unnest($1::uuid[]) AS f(id)
        JOIN ${this.s}.edges AS e ON e.source_id = f.id OR e.target_id = f.id
        JOIN ${this.s}.insights AS neigh
          ON neigh.id = CASE WHEN e.source_id = f.id THEN e.target_id ELSE e.source_id END
         AND neigh.deleted_at IS NULL
        WHERE ($2::text IS NULL OR e.edge_type = $2)
          AND NOT neigh.id = ANY ($3::uuid[])
        ORDER BY neigh.id, e.created_at ASC, e.ctid ASC
        `,
        [frontier, input.edgeType ?? null, [...seen]],
      );
      frontier = [];
      for (const row of hop.rows) {
        const id = String(row.id);
        if (seen.has(id)) {
          continue;
        }
        seen.add(id);
        frontier.push(id);
        hits.push({
          id,
          depth,
          weight: Number(row.weight),
          viaEdgeType: row.via == null ? undefined : String(row.via),
        });
      }
    }
    return hits
      .sort((a, b) => a.depth - b.depth || b.weight - a.weight || a.id.localeCompare(b.id))
      .slice(0, input.limit);
  }

  async loadEdgeContext(input: {
    excludeId: string;
    source: string;
    since: Date;
    entities: readonly string[];
  }): Promise<EdgeContext> {
    const result = await this.pool.query(
      `
      WITH
      latest AS (
          SELECT id, content, created_at
          FROM ${this.s}.insights
          WHERE deleted_at IS NULL AND id <> $1::uuid AND source = $2
          ORDER BY created_at DESC, id ASC
          LIMIT 1
      ),
      windowed AS (
          SELECT id, content, created_at
          FROM ${this.s}.insights
          WHERE deleted_at IS NULL AND id <> $1::uuid AND created_at >= $3
          ORDER BY created_at DESC, id ASC
          LIMIT $4
      ),
      causal AS (
          SELECT id, content, created_at
          FROM ${this.s}.insights
          WHERE deleted_at IS NULL AND id <> $1::uuid AND source = $2
          ORDER BY created_at DESC, id ASC
          LIMIT $5
      ),
      entity_ranked AS (
          SELECT e.entity, e.ord, i.id AS target_id,
                 row_number() OVER (PARTITION BY e.entity ORDER BY i.created_at DESC, i.id ASC) AS rn
          FROM unnest($6::text[]) WITH ORDINALITY AS e(entity, ord)
          JOIN ${this.s}.insights AS i
            ON i.deleted_at IS NULL
           AND i.id <> $1::uuid
           AND EXISTS (
                 SELECT 1
                 FROM jsonb_array_elements_text(i.entities) AS stored
                 WHERE lower(stored) = lower(e.entity)
               )
      )
      SELECT 'latest' AS bucket, id, content, created_at, NULL::text AS entity, NULL::uuid AS target_id, NULL::int AS ord, NULL::int AS rn
      FROM latest
      UNION ALL
      SELECT 'window', id, content, created_at, NULL::text, NULL::uuid, NULL::int, NULL::int FROM windowed
      UNION ALL
      SELECT 'causal', id, content, created_at, NULL::text, NULL::uuid, NULL::int, NULL::int FROM causal
      UNION ALL
      SELECT 'entity', NULL::uuid, NULL::text, NULL::timestamptz, entity, target_id, ord::int, rn::int
      FROM entity_ranked
      WHERE rn <= $7
      ORDER BY bucket, created_at DESC NULLS LAST, ord ASC NULLS LAST, rn ASC NULLS LAST, id ASC NULLS LAST
      `,
      [
        input.excludeId,
        input.source,
        input.since,
        MAX_TEMPORAL_PROXIMITY,
        CAUSAL_LOOKBACK,
        input.entities,
        MAX_ENTITY_LINKS,
      ],
    );

    const context: EdgeContext = { recentWithin24h: [], causalPrevious: [], entityPairs: [] };
    const maxPairs = MAX_TOTAL_ENTITY_EDGES / 2;
    for (const row of result.rows) {
      const bucket = String(row.bucket);
      if (bucket === "latest" && row.id) {
        context.latestSameSource = {
          id: String(row.id),
          content: String(row.content),
          createdAt: row.created_at instanceof Date ? row.created_at : new Date(String(row.created_at)),
        };
      } else if (bucket === "window" && row.id) {
        context.recentWithin24h.push({
          id: String(row.id),
          content: String(row.content),
          createdAt: row.created_at instanceof Date ? row.created_at : new Date(String(row.created_at)),
        });
      } else if (bucket === "causal" && row.id) {
        context.causalPrevious.push({ id: String(row.id), content: String(row.content) });
      } else if (bucket === "entity" && row.entity && row.target_id && context.entityPairs.length < maxPairs) {
        context.entityPairs.push({ entity: String(row.entity), targetId: String(row.target_id) });
      }
    }
    return context;
  }

  async getEdgesForNodeIds(ids: readonly string[]): Promise<EdgeRecord[]> {
    if (ids.length === 0) {
      return [];
    }
    const result = await this.pool.query(
      `
      SELECT * FROM ${this.s}.edges
      WHERE source_id = ANY($1::uuid[]) OR target_id = ANY($1::uuid[])
      `,
      [ids],
    );
    return result.rows.map((row) => mapEdgeRow(row as Record<string, unknown>));
  }

  async listKnownEntities(): Promise<string[]> {
    const result = await this.pool.query(
      `
      SELECT DISTINCT e AS entity
      FROM ${this.s}.insights AS i,
           jsonb_array_elements_text(i.entities) AS e
      WHERE i.deleted_at IS NULL
      `,
    );
    return result.rows.map((row) => String(row.entity));
  }

  async walkRecallGraph(input: {
    anchors: readonly AnchorHit[];
    intent: RecallIntent;
    queryVector?: readonly number[];
    maxCandidates: number;
  }): Promise<GraphWalkHit[]> {
    if (input.anchors.length === 0) {
      return [];
    }
    const limits = traversalLimits(input.intent);
    const weights = intentWeights(input.intent);
    const best = new Map<string, { score: number; via: string }>();
    for (const anchor of input.anchors) {
      best.set(anchor.id, { score: anchor.score, via: anchor.matchedVia });
    }
    const seen = new Map<string, Set<string>>();
    let frontier = input.anchors.map((anchor) => ({
      id: anchor.id,
      anchorId: anchor.id,
      score: anchor.score,
    }));
    for (const anchor of input.anchors) {
      seen.set(anchor.id, new Set([anchor.id]));
    }

    for (let depth = 0; depth < limits.depth && frontier.length > 0; depth++) {
      const open = frontier.filter((row) => (seen.get(row.anchorId)?.size ?? 0) < limits.visited);
      if (open.length === 0) {
        break;
      }
      const hop = await this.pool.query(
        `
        SELECT s.anchor_id, neigh.id,
               s.score + $1::float8 * COALESCE(($2::jsonb ->> e.edge_type)::float8, 0) * e.weight
                        + $3::float8 * CASE
                          WHEN $4::vector IS NULL OR neigh.embedding IS NULL THEN 0::float8
                          ELSE GREATEST(0::float8, 1 - (neigh.embedding <=> $4::vector))
                        END AS score,
               e.edge_type AS via
        FROM unnest($5::uuid[], $6::uuid[], $7::float8[]) AS s(id, anchor_id, score)
        JOIN ${this.s}.edges AS e ON e.source_id = s.id OR e.target_id = s.id
        JOIN ${this.s}.insights AS neigh
          ON neigh.id = CASE WHEN e.source_id = s.id THEN e.target_id ELSE e.source_id END
         AND neigh.deleted_at IS NULL
        ORDER BY 3 DESC, neigh.id ASC, s.anchor_id ASC, e.edge_type ASC
        `,
        [
          GRAPH_LAMBDA1,
          JSON.stringify(weights),
          GRAPH_LAMBDA2,
          input.queryVector ? vec(input.queryVector) : null,
          open.map((row) => row.id),
          open.map((row) => row.anchorId),
          open.map((row) => row.score),
        ],
      );

      const nextByAnchor = new Map<string, Array<{ id: string; score: number }>>();
      for (const row of hop.rows) {
        const id = String(row.id);
        const anchorId = String(row.anchor_id);
        const score = Number(row.score);
        const via = String(row.via);
        const prev = best.get(id);
        if (!prev || score > prev.score) {
          best.set(id, { score, via });
        }
        const local = seen.get(anchorId) ?? new Set<string>();
        if (local.has(id) || local.size >= limits.visited) {
          continue;
        }
        local.add(id);
        seen.set(anchorId, local);
        const bucket = nextByAnchor.get(anchorId) ?? [];
        bucket.push({ id, score });
        nextByAnchor.set(anchorId, bucket);
      }

      frontier = [];
      for (const [anchorId, bucket] of nextByAnchor) {
        bucket.sort((a, b) => b.score - a.score || a.id.localeCompare(b.id));
        for (const row of bucket.slice(0, limits.beam)) {
          frontier.push({ id: row.id, anchorId, score: row.score });
        }
      }
    }

    return [...best.entries()]
      .sort((a, b) => b[1].score - a[1].score || a[0].localeCompare(b[0]))
      .slice(0, input.maxCandidates)
      .map(([id, row]) => ({ id, score: row.score, via: row.via }));
  }

  async getSetting(key: string): Promise<unknown | undefined> {
    const result = await this.pool.query(`SELECT value FROM ${this.s}.settings WHERE key = $1`, [key]);
    return result.rows[0]?.value;
  }
}

class PostgresMnemonStoreTx implements MnemonStoreTx {
  constructor(
    private readonly client: PoolClient,
    private readonly s: string,
  ) {}

  async insertInsight(record: NewInsightRecord): Promise<InsightRecord> {
    try {
      const result = await this.client.query(
        `
        INSERT INTO ${this.s}.insights (
          id, content, normalized_content, content_hash, search_tokens, category, importance,
          tags, entities, source, created_at, updated_at, embedding, effective_importance
        ) VALUES (
          $1::uuid, $2, $3, $4, $5::text[], $6, $7, $8::jsonb, $9::jsonb, $10, $11, $12, $13::vector, $14
        )
        RETURNING ${insightSelect(false)}
        `,
        [
          record.id,
          record.content,
          record.normalizedContent,
          record.contentHash,
          record.searchTokens,
          record.category,
          record.importance,
          JSON.stringify(record.tags),
          JSON.stringify(record.entities),
          record.source,
          record.createdAt,
          record.updatedAt,
          record.embedding ? vec(record.embedding) : null,
          record.effectiveImportance,
        ],
      );
      const inserted = mapInsightRow(result.rows[0] as Record<string, unknown>);
      inserted.embedding = record.embedding;
      return inserted;
    } catch (error) {
      const code = (error as { code?: string }).code;
      if (code === UNIQUE_VIOLATION) {
        throw new UniqueViolationError("active content hash already exists");
      }
      throw wrapDatabaseError(error);
    }
  }

  async upsertEdges(edges: readonly NewEdgeRecord[]): Promise<EdgeRecord[]> {
    if (edges.length === 0) {
      return [];
    }
    const unique = new Map<string, NewEdgeRecord>();
    for (const edge of edges) {
      unique.set(`${edge.sourceId}\0${edge.targetId}\0${edge.edgeType}`, edge);
    }
    const deduped = [...unique.values()];
    const sourceIds = deduped.map((e) => e.sourceId);
    const targetIds = deduped.map((e) => e.targetId);
    const types = deduped.map((e) => e.edgeType);
    const weights = deduped.map((e) => e.weight);
    const metas = deduped.map((e) => JSON.stringify(e.metadata));
    const created = deduped.map((e) => e.createdAt);
    const result = await this.client.query(
      `
      INSERT INTO ${this.s}.edges (source_id, target_id, edge_type, weight, metadata, created_at)
      SELECT *
      FROM unnest($1::uuid[], $2::uuid[], $3::text[], $4::float8[], $5::jsonb[], $6::timestamptz[])
      ON CONFLICT (source_id, target_id, edge_type)
      DO UPDATE SET
          weight = EXCLUDED.weight,
          metadata = EXCLUDED.metadata,
          created_at = EXCLUDED.created_at
      RETURNING *
      `,
      [sourceIds, targetIds, types, weights, metas, created],
    );
    return result.rows.map((row) => mapEdgeRow(row as Record<string, unknown>));
  }

  async appendOp(
    operation: string,
    insightId: string | null,
    detail: Record<string, unknown>,
    at: Date,
  ): Promise<void> {
    await this.client.query(
      `INSERT INTO ${this.s}.oplog (operation, insight_id, detail, created_at) VALUES ($1, $2, $3::jsonb, $4)`,
      [operation, insightId, JSON.stringify(detail), at],
    );
  }

  async setEffectiveImportance(id: string, value: number): Promise<void> {
    await this.client.query(`UPDATE ${this.s}.insights SET effective_importance = $2 WHERE id = $1::uuid`, [
      id,
      value,
    ]);
  }

  async establishEmbeddingSettings(dimensions: number, model: string, at: Date): Promise<void> {
    const dim = await this.client.query(
      `
      WITH ins_dim AS (
        INSERT INTO ${this.s}.settings (key, value, updated_at)
        VALUES ('embedding_dimensions', to_jsonb($1::int), $3)
        ON CONFLICT (key) DO UPDATE SET key = ${this.s}.settings.key
        RETURNING value
      ),
      ins_model AS (
        INSERT INTO ${this.s}.settings (key, value, updated_at)
        VALUES ('embedding_model', to_jsonb($2::text), $3)
        ON CONFLICT (key) DO UPDATE SET key = ${this.s}.settings.key
        RETURNING value
      )
      SELECT value FROM ins_dim
      `,
      [dimensions, model, at],
    );
    const stored = dim.rows[0]?.value;
    if (Number(stored) !== dimensions) {
      throw new Error(`embedding dimension mismatch: store has ${String(stored)}, got ${dimensions}`);
    }
  }

  async linkAndLog(edge: NewEdgeRecord, at: Date): Promise<EdgeRecord> {
    const result = await this.client.query(
      `
      WITH
      src AS (
        SELECT EXISTS (
          SELECT 1 FROM ${this.s}.insights WHERE id = $1::uuid AND deleted_at IS NULL
        ) AS ok
      ),
      tgt AS (
        SELECT EXISTS (
          SELECT 1 FROM ${this.s}.insights WHERE id = $2::uuid AND deleted_at IS NULL
        ) AS ok
      ),
      upserted AS (
        INSERT INTO ${this.s}.edges (source_id, target_id, edge_type, weight, metadata, created_at)
        SELECT $1::uuid, $2::uuid, $3, $4, $5::jsonb, $6
        WHERE (SELECT ok FROM src) AND (SELECT ok FROM tgt)
        ON CONFLICT (source_id, target_id, edge_type)
        DO UPDATE SET
            weight = EXCLUDED.weight,
            metadata = EXCLUDED.metadata,
            created_at = EXCLUDED.created_at
        RETURNING *
      ),
      logged AS (
        INSERT INTO ${this.s}.oplog (operation, insight_id, detail, created_at)
        SELECT 'link', $1::uuid, $7::jsonb, $6
        FROM upserted
      )
      SELECT src.ok AS src_ok, tgt.ok AS tgt_ok,
             u.source_id, u.target_id, u.edge_type, u.weight, u.metadata, u.created_at
      FROM src, tgt
      LEFT JOIN upserted AS u ON true
      `,
      [
        edge.sourceId,
        edge.targetId,
        edge.edgeType,
        edge.weight,
        JSON.stringify(edge.metadata),
        at,
        JSON.stringify({
          source_id: edge.sourceId,
          target_id: edge.targetId,
          edge_type: edge.edgeType,
        }),
      ],
    );
    const row = result.rows[0] as Record<string, unknown> | undefined;
    if (!row?.src_ok) {
      throw new MnemonNotFoundError(`insight ${edge.sourceId} not found`, edge.sourceId);
    }
    if (!row.tgt_ok) {
      throw new MnemonNotFoundError(`insight ${edge.targetId} not found`, edge.targetId);
    }
    return mapEdgeRow(row);
  }

  async forgetAndLog(id: string, at: Date): Promise<boolean> {
    const locked = await this.client.query(
      `SELECT id FROM ${this.s}.insights WHERE id = $1::uuid AND deleted_at IS NULL FOR UPDATE`,
      [id],
    );
    if ((locked.rowCount ?? 0) === 0) {
      return false;
    }
    await this.client.query(
      `
      WITH tombstone AS (
        UPDATE ${this.s}.insights
        SET deleted_at = $2, updated_at = $2
        WHERE id = $1::uuid AND deleted_at IS NULL
        RETURNING id
      ),
      removed AS (
        DELETE FROM ${this.s}.edges
        WHERE source_id = $1::uuid OR target_id = $1::uuid
      )
      INSERT INTO ${this.s}.oplog (operation, insight_id, detail, created_at)
      SELECT 'forget', id, '{}'::jsonb, $2 FROM tombstone
      `,
      [id, at],
    );
    return true;
  }

  async incrementAccess(ids: readonly string[], at: Date): Promise<void> {
    if (ids.length === 0) {
      return;
    }
    await this.client.query(
      `
      UPDATE ${this.s}.insights
      SET access_count = access_count + 1,
          last_accessed_at = $2,
          updated_at = GREATEST(updated_at, $2)
      WHERE id = ANY($1::uuid[])
        AND deleted_at IS NULL
      `,
      [ids, at],
    );
  }
}

function mapAnchor(row: Record<string, unknown>): AnchorHit {
  return {
    id: String(row.id),
    score: Number(row.score),
    matchedVia: row.matched_via as AnchorHit["matchedVia"],
    signals: Array.isArray(row.signals) ? row.signals.map(String) : [],
  };
}
