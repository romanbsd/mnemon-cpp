import { createHash, randomUUID } from "node:crypto";

import { Pool } from "pg";
import pgvector from "pgvector/pg";

import { resolveConfig, type MnemonConfig, type ResolvedConfig } from "./config.js";
import {
  ALGORITHM_VERSION,
  ANCHOR_TOP_K,
  DEDUP_CANDIDATE_LIMIT,
  DEFAULT_RELATED_DEPTH,
  DEFAULT_RELATED_LIMIT,
  MAX_RELATED_DEPTH,
  MAX_RELATED_LIMIT,
  MAX_SEMANTIC_EDGES,
  SEMANTIC_CANDIDATE_MIN_COSINE,
  TEMPORAL_WINDOW_HOURS,
} from "./engine/constants.js";
import {
  buildCausalEdges,
  buildEntityEdges,
  buildSemanticEdges,
  buildTemporalEdges,
  countEdgesByType,
} from "./engine/edges.js";
import { makeBriefExcerpt } from "./engine/brief.js";
import { extractEntitiesIndexed, mergeEntities } from "./engine/entities.js";
import { detectIntent } from "./engine/intent.js";
import { contentHash, normalizeContent } from "./engine/normalize.js";
import {
  causalTopologicalOrder,
  compareRecallHits,
  composeFinalScore,
  normalizeEliteGraph,
} from "./engine/recall.js";
import { effectiveImportance } from "./engine/retention.js";
import { classifyDiff, classifySafeDuplicate, scoreDuplicateCandidate } from "./engine/diff.js";
import { sortedSearchTokens, tokenize } from "./engine/tokenize.js";
import {
  validateEmbedding,
  validateLinkInput,
  validateListInput,
  validateLogInput,
  validateRecallInput,
  validateRememberInput,
  validateSearchInput,
  validateUuid,
} from "./engine/validate.js";
import { MnemonConfigurationError, MnemonEmbeddingError, MnemonNotFoundError } from "./errors.js";
import { runMigrations } from "./postgres/migrations.js";
import { toPublicInsight } from "./postgres/row-mappers.js";
import { PostgresMnemonStore, isUniqueViolation, type MnemonStore } from "./postgres/store.js";
import type { InsightRecord } from "./postgres/schema.js";
import {
  EDGE_TYPES,
  type Edge,
  type EdgeType,
  type ForgetResult,
  type Insight,
  type LinkInput,
  type ListInput,
  type LogInput,
  type Mnemon,
  type MnemonStatus,
  type OpLogEntry,
  type RecallHit,
  type RecallInput,
  type RecallResult,
  type RelatedInsight,
  type RememberInput,
  type RememberResult,
  type SearchInput,
  type SearchResult,
  type SimilarMemory,
} from "./types.js";

export function createMnemon(config: MnemonConfig): Mnemon {
  return new MnemonService(resolveConfig(config));
}

class MnemonService implements Mnemon {
  private readonly ownsPool: boolean;
  private readonly pool: Pool;
  private readonly store: MnemonStore;
  private initPromise: Promise<void> | undefined;
  private closed = false;

  constructor(private readonly config: ResolvedConfig) {
    this.ownsPool = config.pool === undefined;
    this.pool =
      config.pool ??
      new Pool({
        connectionString: config.databaseUrl,
        onConnect: async (client) => {
          try {
            await pgvector.registerTypes(client);
          } catch {
            // vector type is registered again after CREATE EXTENSION
          }
        },
      });
    this.store = new PostgresMnemonStore(this.pool, config.schema);
  }

  async initialize(): Promise<void> {
    if (!this.initPromise) {
      this.initPromise = this.doInitialize();
    }
    return this.initPromise;
  }

  private async doInitialize(): Promise<void> {
    await runMigrations(this.pool, this.config.schema);
    await this.registerVectorTypes();
    if (this.config.embeddingProvider) {
      const stored = await this.store.getSetting("embedding_dimensions");
      if (stored !== undefined && Number(stored) !== this.config.embeddingProvider.dimensions) {
        throw new MnemonConfigurationError(
          `embedding provider dimension ${this.config.embeddingProvider.dimensions} does not match store ${String(stored)}`,
        );
      }
    }
  }

  private async registerVectorTypes(): Promise<void> {
    const client = await this.pool.connect();
    try {
      await pgvector.registerTypes(client);
    } finally {
      client.release();
    }
  }

  private async ready(): Promise<void> {
    if (this.closed) {
      throw new MnemonConfigurationError("mnemon is closed");
    }
    await this.initialize();
  }

  async remember(input: RememberInput): Promise<RememberResult> {
    await this.ready();
    const validated = validateRememberInput(input, this.config.defaults);
    const now = this.config.clock.now();
    const createdAt = validated.createdAt ?? now;
    const normalized = normalizeContent(validated.content);
    const hash = contentHash(normalized);

    let embedding: number[] | undefined;
    if (this.config.embeddingProvider) {
      embedding = await this.embed(validated.content, "document");
    }

    const exact = await this.store.findExactDuplicate(hash);
    if (exact) {
      return this.skipDuplicate(exact, now);
    }

    const near = await this.findNearDuplicates(validated.content, embedding);
    const diff = classifyDiff(validated.content, near);
    if (validated.deduplicate) {
      const classified = classifySafeDuplicate(validated.content, near);
      if (classified) {
        const existing = await this.store.getActiveInsight(classified.id);
        if (existing) {
          return this.skipDuplicate(existing, now, classified.id, diff);
        }
      }
    }

    const known = new Set(await this.store.listKnownEntities());
    const extracted = extractEntitiesIndexed(validated.content, known);
    const entities = mergeEntities(validated.entities, extracted);
    const searchTokens = sortedSearchTokens(validated.content, validated.tags, entities);
    const id = randomUUID();
    const generated = await this.generateEdges(
      { id, content: validated.content, source: validated.source, createdAt, entities, embedding },
      now,
    );

    try {
      const { insight, edges } = await this.store.withTransaction(async (tx) => {
        if (embedding && this.config.embeddingProvider) {
          try {
            await tx.establishEmbeddingSettings(
              this.config.embeddingProvider.dimensions,
              this.config.embeddingProvider.model,
              now,
            );
          } catch (error) {
            if (error instanceof Error && error.message.startsWith("embedding dimension")) {
              throw new MnemonConfigurationError(error.message, { cause: error });
            }
            throw error;
          }
        }

        const inserted = await tx.insertInsight({
          id,
          content: validated.content,
          normalizedContent: normalized,
          contentHash: hash,
          searchTokens,
          category: validated.category,
          importance: validated.importance,
          tags: validated.tags,
          entities,
          source: validated.source,
          createdAt,
          updatedAt: now,
          embedding: embedding ?? null,
          effectiveImportance: 0.5,
        });

        const persisted = await tx.upsertEdges(generated.map((e) => ({ ...e, createdAt: now })));
        const ei = effectiveImportance({
          importance: inserted.importance,
          accessCount: 0,
          daysSinceAccess: 0,
          edgeCount: persisted.length,
        });
        await tx.setEffectiveImportance(inserted.id, ei);
        inserted.effectiveImportance = ei;

        await tx.appendOp(
          "remember",
          inserted.id,
          {
            edge_counts: countEdgesByType(generated),
            embedding_model: this.config.embeddingProvider?.model ?? null,
          },
          now,
        );
        return { insight: inserted, edges: persisted };
      });

      const semanticCandidates = await this.semanticCandidates(insight);
      return {
        action: "added",
        insight: toPublicInsight(insight),
        suggestion: diff.suggestion,
        diff: diff.matches,
        semanticCandidates,
        edgeCounts: countEdgesByType(edges),
      };
    } catch (error) {
      if (isUniqueViolation(error)) {
        const winner = await this.store.findExactDuplicate(hash);
        if (winner) {
          return this.skipDuplicate(winner, now);
        }
      }
      throw error;
    }
  }

  async recall(input: RecallInput): Promise<RecallResult> {
    await this.ready();
    const validated = validateRecallInput(input, this.config.defaults.recallLimit);
    const now = this.config.clock.now();

    let queryVector: number[] | undefined;
    if (this.config.embeddingProvider) {
      queryVector = await this.embed(validated.query, "query");
    }

    const intent = validated.intent ?? detectIntent(validated.query);
    const intentSource = validated.intent ? "override" : "auto";
    const queryTokens = [...tokenize(validated.query)].sort();
    const known = new Set(await this.store.listKnownEntities());
    const queryEntities = extractEntitiesIndexed(validated.query, known);

    const anchors = await this.store.selectRecallAnchors({
      queryTokens,
      queryVector,
      limitPerSignal: ANCHOR_TOP_K,
      source: validated.source,
    });

    const walked = await this.store.walkRecallGraph({
      anchors,
      intent,
      queryVector,
      maxCandidates: this.config.limits.maxRecallCandidates,
    });
    const graphRaw = new Map(walked.map((row) => [row.id, row.score]));
    const viaById = new Map(walked.map((row) => [row.id, row.via]));
    const candidateIds = walked.map((row) => row.id);
    const scored = await this.store.loadScoredInsights({
      ids: candidateIds,
      queryTokens,
      queryEntities,
      queryVector,
    });
    const scoredById = new Map(scored.map((row) => [row.insight.id, row]));
    const graphById = normalizeEliteGraph(
      scored.map((row) => ({
        id: row.insight.id,
        keyword: row.signals.keyword,
        similarity: row.signals.similarity,
        graphRaw: graphRaw.get(row.insight.id) ?? 0,
      })),
      queryVector !== undefined,
    );

    let hits: RecallHit[] = [];
    for (const id of candidateIds) {
      const row = scoredById.get(id);
      if (!row) {
        continue;
      }
      const insight = row.insight;
      const signals = row.signals;
      const graph = graphById.get(id) ?? 0;
      const score = composeFinalScore({
        keyword: signals.keyword,
        entity: signals.entity,
        similarity: signals.similarity,
        graph,
        hasQueryEmbedding: queryVector !== undefined,
      });
      const via = viaById.get(id) ?? "keyword";
      hits.push({
        insight: toPublicInsight(insight),
        score,
        intent,
        matchedVia: via as RecallHit["matchedVia"],
        signals: {
          keyword: signals.keyword,
          entity: signals.entity,
          similarity: signals.similarity,
          graph,
        },
      });
    }

    hits.sort((a, b) =>
      compareRecallHits(
        { score: a.score, importance: a.insight.importance },
        { score: b.score, importance: b.insight.importance },
      ),
    );

    if (intent === "WHY") {
      const causal = (await this.store.getEdgesForNodeIds(candidateIds)).filter((e) => e.edgeType === "causal");
      const ranked = hits.slice(0, validated.limit);
      hits = causalTopologicalOrder(
        ranked.map((h) => ({ ...h, id: h.insight.id })),
        causal,
      );
    } else {
      hits = hits.slice(0, validated.limit);
    }

    if (validated.brief) {
      hits = hits.map((hit) => {
        const excerpt = makeBriefExcerpt(hit.insight.content, validated.excerptChars);
        return { ...hit, excerpt, insight: { ...hit.insight, content: excerpt } };
      });
    }

    const returnedIds = hits.map((h) => h.insight.id);
    await this.store.withTransaction(async (tx) => {
      await tx.incrementAccess(returnedIds, now);
      await tx.appendOp(
        "recall",
        null,
        {
          query_hash: createHash("sha256").update(validated.query, "utf8").digest("hex"),
          hit_count: returnedIds.length,
          intent,
          algorithm_version: "mnemon-ts-v1",
        },
        now,
      );
    });

    const result: RecallResult = {
      results: hits,
      meta: {
        intent,
        intentSource,
        anchorCount: anchors.length,
        traversed: walked.length,
        algorithmVersion: ALGORITHM_VERSION,
      },
    };
    if (hits.length === 0 || hits.length < validated.limit / 2) {
      result.meta.hint = "sparse_results";
    }
    return result;
  }

  async link(input: LinkInput): Promise<Edge> {
    await this.ready();
    const validated = validateLinkInput(input);
    const now = this.config.clock.now();
    const edge = await this.store.withTransaction((tx) =>
      tx.linkAndLog(
        {
          sourceId: validated.sourceId,
          targetId: validated.targetId,
          edgeType: validated.edgeType,
          weight: validated.weight,
          metadata: validated.metadata,
          createdAt: now,
        },
        now,
      ),
    );
    return {
      sourceId: edge.sourceId,
      targetId: edge.targetId,
      edgeType: edge.edgeType,
      weight: edge.weight,
      metadata: edge.metadata,
      createdAt: edge.createdAt.toISOString(),
    };
  }

  async related(
    id: string,
    options?: { maxDepth?: number; limit?: number; edgeType?: EdgeType },
  ): Promise<RelatedInsight[]> {
    await this.ready();
    validateUuid(id, "id");
    const start = await this.store.getActiveInsight(id);
    if (!start) {
      throw new MnemonNotFoundError(`insight ${id} not found`, id);
    }
    const maxDepth = Math.min(options?.maxDepth ?? DEFAULT_RELATED_DEPTH, MAX_RELATED_DEPTH);
    const limit = Math.min(options?.limit ?? DEFAULT_RELATED_LIMIT, MAX_RELATED_LIMIT);
    const walked = await this.store.walkRelated({
      startId: id,
      maxDepth,
      limit,
      edgeType: options?.edgeType,
    });
    const byId = indexById(await this.store.loadInsightsByIds(walked.map((o) => o.id)));
    return walked.flatMap((o) => {
      const insight = byId.get(o.id);
      if (!insight) {
        return [];
      }
      const via = o.viaEdgeType;
      const row: RelatedInsight = { ...toPublicInsight(insight), depth: o.depth };
      if (via && (EDGE_TYPES as readonly string[]).includes(via)) {
        row.viaEdgeType = via as RelatedInsight["viaEdgeType"];
      }
      return [row];
    });
  }

  async forget(id: string): Promise<ForgetResult> {
    await this.ready();
    validateUuid(id, "id");
    const now = this.config.clock.now();
    const forgotten = await this.store.withTransaction((tx) => tx.forgetAndLog(id, now));
    return { forgotten, id };
  }

  async get(id: string): Promise<Insight | null> {
    await this.ready();
    validateUuid(id, "id");
    const record = await this.store.getActiveInsight(id);
    return record ? toPublicInsight(record) : null;
  }

  async list(input?: ListInput): Promise<Insight[]> {
    await this.ready();
    const validated = validateListInput(input);
    const rows = await this.store.listInsights(validated);
    return rows.map(toPublicInsight);
  }

  async search(input: SearchInput): Promise<SearchResult> {
    await this.ready();
    const validated = validateSearchInput(input);
    const queryTokens = [...tokenize(validated.query)].sort();
    const hits = await this.store.searchInsights({
      query: validated.query,
      queryTokens,
      limit: validated.limit,
      source: validated.source,
    });
    const byId = indexById(await this.store.loadInsightsByIds(hits.map((h) => h.id)));
    return {
      results: hits.flatMap((hit) => {
        const insight = byId.get(hit.id);
        if (!insight) {
          return [];
        }
        const via =
          hit.keyword > 0 && hit.fts > 0 ? "hybrid" : hit.fts > hit.keyword ? "fts" : "keyword";
        return [
          {
            insight: toPublicInsight(insight),
            score: 0.45 * hit.keyword + 0.55 * hit.fts,
            matchedVia: via,
            signals: { keyword: hit.keyword, fts: hit.fts },
          },
        ];
      }),
    };
  }

  async log(input?: LogInput): Promise<OpLogEntry[]> {
    await this.ready();
    const validated = validateLogInput(input);
    const rows = await this.store.listOps(validated);
    return rows.map((row) => {
      const entry: OpLogEntry = {
        id: row.id,
        operation: row.operation,
        detail: row.detail,
        createdAt: row.createdAt.toISOString(),
      };
      if (row.insightId) {
        entry.insightId = row.insightId;
      }
      return entry;
    });
  }

  async status(): Promise<MnemonStatus> {
    await this.ready();
    const counts = await this.store.counts();
    const model = await this.store.getSetting("embedding_model");
    const dimensions = await this.store.getSetting("embedding_dimensions");
    const status: MnemonStatus = {
      schema: this.config.schema,
      algorithmVersion: ALGORITHM_VERSION,
      insights: counts.insights,
      embeddings: counts.embeddings,
      edges: counts.edges,
    };
    if (typeof model === "string" && model.length > 0) {
      status.embeddingModel = model;
    }
    if (typeof dimensions === "number" && Number.isFinite(dimensions)) {
      status.embeddingDimensions = dimensions;
    }
    return status;
  }

  async close(): Promise<void> {
    this.closed = true;
    if (this.ownsPool) {
      await this.pool.end();
    }
  }

  private async embed(text: string, purpose: "document" | "query"): Promise<number[]> {
    const provider = this.config.embeddingProvider!;
    try {
      const vector = await provider.embed(text, purpose);
      return validateEmbedding(vector, provider.dimensions);
    } catch (error) {
      if (error instanceof MnemonEmbeddingError) {
        throw error;
      }
      throw new MnemonEmbeddingError("embedding provider failed", { cause: error });
    }
  }

  private async skipDuplicate(
    existing: InsightRecord,
    now: Date,
    duplicateOf?: string,
    diff = classifyDiff(existing.content, [
      { id: existing.id, content: existing.content, cosineSimilarity: 1 },
    ]),
  ): Promise<RememberResult> {
    await this.store.withTransaction((tx) =>
      tx.appendOp("remember_skipped", existing.id, { duplicate_of: existing.id }, now),
    );
    return {
      action: "skipped",
      insight: toPublicInsight(existing),
      duplicateOf: duplicateOf ?? existing.id,
      suggestion: "DUPLICATE",
      diff: diff.matches,
      semanticCandidates: [],
      edgeCounts: { temporal: 0, semantic: 0, causal: 0, entity: 0 },
    };
  }

  private async findNearDuplicates(
    content: string,
    embedding: number[] | undefined,
  ): Promise<Array<{ id: string; content: string; tokenSimilarity: number; cosineSimilarity: number }>> {
    const tokens = [...tokenize(content)].sort();
    const keywordHits = await this.store.findKeywordCandidates(tokens, DEDUP_CANDIDATE_LIMIT);
    const vectorHits = embedding
      ? await this.store.nearestEmbeddings(embedding, { limit: DEDUP_CANDIDATE_LIMIT })
      : [];
    const ids = [...new Set([...keywordHits.map((h) => h.id), ...vectorHits.map((h) => h.id)])];
    return (await this.store.loadInsightsByIds(ids, { embedding: true })).map((ins) => ({
      id: ins.id,
      content: ins.content,
      ...scoreDuplicateCandidate(content, ins.content, embedding, ins.embedding ?? undefined),
    }));
  }

  private async generateEdges(
    insight: {
      id: string;
      content: string;
      source: string;
      createdAt: Date;
      entities: readonly string[];
      embedding?: number[] | null;
    },
    now: Date,
  ) {
    const since = new Date(now.getTime() - TEMPORAL_WINDOW_HOURS * 3_600_000);
    const context = await this.store.loadEdgeContext({
      excludeId: insight.id,
      source: insight.source,
      since,
      entities: insight.entities,
    });
    const temporal = buildTemporalEdges({
      newId: insight.id,
      newCreatedAt: insight.createdAt,
      now,
      latestSameSource: context.latestSameSource,
      recentWithin24h: context.recentWithin24h,
    });
    const entity = buildEntityEdges({ newId: insight.id, pairs: context.entityPairs });
    const causal = buildCausalEdges({
      newId: insight.id,
      newContent: insight.content,
      previous: context.causalPrevious,
    });

    let semantic: ReturnType<typeof buildSemanticEdges> = [];
    if (insight.embedding) {
      const neighbors = await this.store.nearestEmbeddings(insight.embedding, {
        excludeId: insight.id,
        limit: MAX_SEMANTIC_EDGES,
      });
      semantic = buildSemanticEdges({
        newId: insight.id,
        neighbors: neighbors.map((n) => ({ id: n.id, cosine: n.cosineSimilarity })),
      });
    }

    return [...temporal, ...entity, ...causal, ...semantic];
  }

  private async semanticCandidates(insight: InsightRecord): Promise<SimilarMemory[]> {
    if (!insight.embedding) {
      return [];
    }
    const hits = await this.store.nearestEmbeddings(insight.embedding, {
      excludeId: insight.id,
      limit: 5,
      minCosine: SEMANTIC_CANDIDATE_MIN_COSINE,
    });
    const byId = indexById(await this.store.loadInsightsByIds(hits.map((h) => h.id), { embedding: true }));
    return hits.flatMap((h) => {
      const ins = byId.get(h.id);
      if (!ins) {
        return [];
      }
      const scored = scoreDuplicateCandidate(
        insight.content,
        ins.content,
        insight.embedding ?? undefined,
        ins.embedding ?? undefined,
      );
      return [
        {
          id: ins.id,
          content: ins.content,
          category: ins.category,
          tokenSimilarity: scored.tokenSimilarity,
          cosineSimilarity: h.cosineSimilarity,
        } satisfies SimilarMemory,
      ];
    });
  }
}

function indexById<T extends { id: string }>(rows: readonly T[]): Map<string, T> {
  return new Map(rows.map((row) => [row.id, row]));
}
