import { MnemonValidationError } from "../errors.js";
import {
  EDGE_TYPES,
  INSIGHT_CATEGORIES,
  type EdgeType,
  type InsightCategory,
  type RecallIntent,
} from "../types.js";
import {
  DEFAULT_BRIEF_EXCERPT_CHARS,
  DEFAULT_LIST_LIMIT,
  DEFAULT_LOG_LIMIT,
  DEFAULT_SEARCH_LIMIT,
  MAX_CONTENT_CODE_POINTS,
  MAX_ENTITIES,
  MAX_ENTITY_CODE_POINTS,
  MAX_LIST_LIMIT,
  MAX_LOG_LIMIT,
  MAX_RECALL_LIMIT,
  MAX_SEARCH_LIMIT,
  MAX_SOURCE_CODE_POINTS,
  MAX_TAGS,
  MAX_TAG_CODE_POINTS,
} from "./constants.js";
import { codePointLength, uniquePreserveOrder } from "./normalize.js";

export interface ValidatedRemember {
  content: string;
  category: InsightCategory;
  importance: 1 | 2 | 3 | 4 | 5;
  tags: string[];
  entities: string[];
  source: string;
  createdAt?: Date;
  deduplicate: boolean;
}

export interface ValidatedRecall {
  query: string;
  limit: number;
  intent?: RecallIntent;
  source?: string;
  brief: boolean;
  excerptChars: number;
}

export interface ValidatedLink {
  sourceId: string;
  targetId: string;
  edgeType: EdgeType;
  weight: number;
  metadata: Record<string, string>;
}

function fail(field: string, code: string, message: string): never {
  throw new MnemonValidationError(message, field, code);
}

function requireNonEmptyTrimmed(value: string, field: string, max: number): string {
  const trimmed = value.trim();
  const len = codePointLength(trimmed);
  if (len < 1) {
    fail(field, "empty", `${field} must be non-empty`);
  }
  if (len > max) {
    fail(field, "too_long", `${field} exceeds ${max} code points`);
  }
  return trimmed;
}

function requireStringList(values: readonly string[] | undefined, field: string, maxItems: number, maxLen: number): string[] {
  if (!values) {
    return [];
  }
  const trimmed = values.map((v) => {
    if (typeof v !== "string") {
      fail(field, "invalid", `${field} must be strings`);
    }
    return requireNonEmptyTrimmed(v, field, maxLen);
  });
  const unique = uniquePreserveOrder(trimmed);
  if (unique.length > maxItems) {
    fail(field, "too_many", `${field} exceeds ${maxItems} unique values`);
  }
  return unique;
}

export function parseTimestamp(value: string, field: string): Date {
  if (!/([zZ]|[+-]\d{2}:\d{2})$/.test(value)) {
    fail(field, "invalid_timestamp", `${field} must include an explicit timezone offset`);
  }
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) {
    fail(field, "invalid_timestamp", `${field} is not a valid timestamp`);
  }
  return date;
}

export function validateEmbedding(vector: readonly number[], dimensions: number, field = "embedding"): number[] {
  if (vector.length !== dimensions) {
    fail(field, "dimension_mismatch", `${field} dimension ${vector.length} does not match ${dimensions}`);
  }
  const copy: number[] = [];
  for (const n of vector) {
    if (!Number.isFinite(n)) {
      fail(field, "not_finite", `${field} must contain only finite numbers`);
    }
    copy.push(n);
  }
  return copy;
}

export function validateWeight(weight: number, field = "weight"): number {
  if (!Number.isFinite(weight) || weight < 0 || weight > 1) {
    fail(field, "out_of_range", `${field} must be a finite number from 0 through 1`);
  }
  return weight;
}

export function validateUuid(id: string, field: string): string {
  if (!/^[0-9a-f]{8}-[0-9a-f]{4}-[1-8][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i.test(id)) {
    fail(field, "invalid", `${field} must be a UUID`);
  }
  return id;
}

export function validateRememberInput(
  input: {
    content: string;
    category?: InsightCategory;
    importance?: 1 | 2 | 3 | 4 | 5;
    tags?: string[];
    entities?: string[];
    source?: string;
    createdAt?: string;
    deduplicate?: boolean;
  },
  defaults: { category: InsightCategory; importance: 1 | 2 | 3 | 4 | 5; source: string },
): ValidatedRemember {
  const content = requireNonEmptyTrimmed(input.content, "content", MAX_CONTENT_CODE_POINTS);
  const category = input.category ?? defaults.category;
  if (!INSIGHT_CATEGORIES.includes(category)) {
    fail(
      "category",
      "invalid_enum",
      `invalid category "${String(category)}"; valid: ${INSIGHT_CATEGORIES.join(", ")}`,
    );
  }
  const importance = input.importance ?? defaults.importance;
  if (!Number.isInteger(importance) || importance < 1 || importance > 5) {
    fail("importance", "out_of_range", "importance must be an integer from 1 through 5");
  }
  return {
    content,
    category,
    importance: importance as 1 | 2 | 3 | 4 | 5,
    tags: requireStringList(input.tags, "tags", MAX_TAGS, MAX_TAG_CODE_POINTS),
    entities: requireStringList(input.entities, "entities", MAX_ENTITIES, MAX_ENTITY_CODE_POINTS),
    source: requireNonEmptyTrimmed(input.source ?? defaults.source, "source", MAX_SOURCE_CODE_POINTS),
    createdAt: input.createdAt ? parseTimestamp(input.createdAt, "createdAt") : undefined,
    deduplicate: input.deduplicate !== false,
  };
}

export function validateRecallInput(
  input: {
    query: string;
    limit?: number;
    intent?: RecallIntent;
    source?: string;
    brief?: boolean;
    excerptChars?: number;
  },
  defaultLimit: number,
): ValidatedRecall {
  const query = requireNonEmptyTrimmed(input.query, "query", MAX_CONTENT_CODE_POINTS);
  const limit = input.limit ?? defaultLimit;
  if (!Number.isInteger(limit) || limit < 1 || limit > MAX_RECALL_LIMIT) {
    fail("limit", "out_of_range", `limit must be an integer from 1 through ${MAX_RECALL_LIMIT}`);
  }
  if (input.intent && !["WHY", "WHEN", "ENTITY", "GENERAL"].includes(input.intent)) {
    fail("intent", "invalid_enum", `invalid intent "${String(input.intent)}"`);
  }
  const brief = input.brief === true;
  const excerptChars = input.excerptChars ?? DEFAULT_BRIEF_EXCERPT_CHARS;
  if (brief && (!Number.isInteger(excerptChars) || excerptChars <= 0)) {
    fail("excerptChars", "out_of_range", "excerptChars must be greater than 0");
  }
  return {
    query,
    limit,
    intent: input.intent,
    source: input.source ? requireNonEmptyTrimmed(input.source, "source", MAX_SOURCE_CODE_POINTS) : undefined,
    brief,
    excerptChars,
  };
}

export function validateSearchInput(input: { query: string; limit?: number; source?: string }): {
  query: string;
  limit: number;
  source?: string;
} {
  const query = requireNonEmptyTrimmed(input.query, "query", MAX_CONTENT_CODE_POINTS);
  const limit = input.limit ?? DEFAULT_SEARCH_LIMIT;
  if (!Number.isInteger(limit) || limit < 1 || limit > MAX_SEARCH_LIMIT) {
    fail("limit", "out_of_range", `limit must be an integer from 1 through ${MAX_SEARCH_LIMIT}`);
  }
  return {
    query,
    limit,
    source: input.source ? requireNonEmptyTrimmed(input.source, "source", MAX_SOURCE_CODE_POINTS) : undefined,
  };
}

export function validateListInput(input?: {
  limit?: number;
  source?: string;
  category?: InsightCategory;
  since?: string;
  until?: string;
}): {
  limit: number;
  source?: string;
  category?: InsightCategory;
  since?: Date;
  until?: Date;
} {
  const limit = input?.limit ?? DEFAULT_LIST_LIMIT;
  if (!Number.isInteger(limit) || limit < 1 || limit > MAX_LIST_LIMIT) {
    fail("limit", "out_of_range", `limit must be an integer from 1 through ${MAX_LIST_LIMIT}`);
  }
  if (input?.category && !INSIGHT_CATEGORIES.includes(input.category)) {
    fail(
      "category",
      "invalid_enum",
      `invalid category "${String(input.category)}"; valid: ${INSIGHT_CATEGORIES.join(", ")}`,
    );
  }
  const since = input?.since ? parseTimestamp(input.since, "since") : undefined;
  const until = input?.until ? parseTimestamp(input.until, "until") : undefined;
  if (since && until && since.getTime() > until.getTime()) {
    fail("until", "invalid_range", "until must be at or after since");
  }
  return {
    limit,
    source: input?.source ? requireNonEmptyTrimmed(input.source, "source", MAX_SOURCE_CODE_POINTS) : undefined,
    category: input?.category,
    since,
    until,
  };
}

export function validateLogInput(input?: { limit?: number; operation?: string }): {
  limit: number;
  operation?: string;
} {
  const limit = input?.limit ?? DEFAULT_LOG_LIMIT;
  if (!Number.isInteger(limit) || limit < 1 || limit > MAX_LOG_LIMIT) {
    fail("limit", "out_of_range", `limit must be an integer from 1 through ${MAX_LOG_LIMIT}`);
  }
  return {
    limit,
    operation: input?.operation
      ? requireNonEmptyTrimmed(input.operation, "operation", MAX_SOURCE_CODE_POINTS)
      : undefined,
  };
}

export function validateLinkInput(input: {
  sourceId: string;
  targetId: string;
  edgeType: EdgeType;
  weight?: number;
  metadata?: Record<string, string>;
}): ValidatedLink {
  const sourceId = validateUuid(input.sourceId, "sourceId");
  const targetId = validateUuid(input.targetId, "targetId");
  if (sourceId === targetId) {
    fail("targetId", "self_link", "cannot link an insight to itself");
  }
  if (!EDGE_TYPES.includes(input.edgeType)) {
    fail("edgeType", "invalid_enum", `invalid edgeType "${String(input.edgeType)}"`);
  }
  const metadata = input.metadata ?? {};
  for (const v of Object.values(metadata)) {
    if (typeof v !== "string") {
      fail("metadata", "invalid", "metadata values must be strings");
    }
  }
  return {
    sourceId,
    targetId,
    edgeType: input.edgeType,
    weight: validateWeight(input.weight ?? 1),
    metadata,
  };
}
