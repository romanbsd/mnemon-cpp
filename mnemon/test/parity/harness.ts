import { execFileSync } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import type { Pool } from "pg";

import { quoteIdent } from "../../src/config.js";
import type { InsightCategory, Mnemon, RecallIntent } from "../../src/types.js";

export type Side = "typescript" | "go" | "cpp";
export type Judgment = "harmful" | "neutral" | "intentionally_safer" | "architectural";
export type RememberAction = "added" | "skipped" | "updated";

export interface CorpusMemory {
  key: string;
  content: string;
  category?: InsightCategory;
  source?: string;
  createdAt: string;
}

export interface CorpusQuery {
  id: string;
  query: string;
  intent?: RecallIntent;
  expected: Array<{ key: string; maxRank: number }>;
  forbidden?: string[];
  mandatory?: boolean;
}

export interface Fixtures {
  memories: CorpusMemory[];
  queries: CorpusQuery[];
}

export interface RecallHitView {
  key?: string;
  score: number;
  intent: string;
  signals: { keyword: number; entity: number; similarity: number; graph: number };
}

export interface SideSnapshot {
  side: Side;
  remembers: Array<{
    key: string;
    action: RememberAction;
    diffSuggestion?: string;
    entities: string[];
    id: string;
    content: string;
  }>;
  recalls: Array<{
    queryId: string;
    intent: string;
    keys: Array<string | undefined>;
    hits: RecallHitView[];
  }>;
  edges: string[];
  related: Record<string, string[]>;
  activeKeys: string[];
}

export interface Mismatch {
  id: string;
  judgment: Judgment;
  kind: string;
  message: string;
  sides?: Partial<Record<Side, unknown>>;
}

export interface PairMetrics {
  reference: Side;
  decisionAgreement: number;
  edgeAgreement: number;
  top5OverlapMean: number;
  expectedHitRate: { typescript: number; reference: number };
}

export type EmbeddingMode = "disabled" | "ollama";

export interface CliRunOptions {
  embeddings: EmbeddingMode;
  endpoint?: string;
  model?: string;
}

export interface ParityReport {
  generatedAt: string;
  embeddings: EmbeddingMode;
  timestamps: "wall-clock";
  sides: Side[];
  metrics: PairMetrics[];
  mismatches: Mismatch[];
  notes: string[];
}

const here = dirname(fileURLToPath(import.meta.url));
const pkgRoot = join(here, "../..");
const fixtureDir = join(here, "../fixtures");
const outDir = join(here, "out");

const RELATED_KEYS = ["dark-mode", "chose-pg", "deploy-start", "cycle-a", "ts-services"] as const;
const DEAD_EMBED = "http://127.0.0.1:1";
export const OLLAMA_ENDPOINT = process.env.MNEMON_EMBED_ENDPOINT || "http://127.0.0.1:11434";
export const OLLAMA_MODEL = process.env.MNEMON_EMBED_MODEL || "nomic-embed-text";

export function loadFixtures(): Fixtures {
  const corpus = JSON.parse(readFileSync(join(fixtureDir, "corpus.json"), "utf8")) as {
    memories: CorpusMemory[];
  };
  const queries = JSON.parse(readFileSync(join(fixtureDir, "queries.json"), "utf8")) as {
    queries: CorpusQuery[];
  };
  return { memories: corpus.memories, queries: queries.queries };
}

export function resolveGoBinary(): string | undefined {
  const env = process.env.MNEMON_GO_BIN;
  if (env && existsSync(env)) {
    return env;
  }
  const cached = join(pkgRoot, ".cache/mnemon-go");
  if (existsSync(cached)) {
    return cached;
  }
  return undefined;
}

export function resolveCppBinary(): string | undefined {
  const env = process.env.MNEMON_CPP_BIN;
  if (env && existsSync(env)) {
    return env;
  }
  const candidates = [
    join(pkgRoot, "../../../build/mnemon"),
    join(pkgRoot, "../../build/mnemon"),
    join(pkgRoot, "../build/mnemon"),
  ];
  return candidates.find((p) => existsSync(p));
}

function parseJson(stdout: string): unknown {
  const text = stdout.trim();
  try {
    return JSON.parse(text);
  } catch {
    const object = text.indexOf("{");
    const array = text.indexOf("[");
    const start = [object, array].filter((i) => i >= 0).sort((a, b) => a - b)[0];
    if (start === undefined) {
      throw new Error(`CLI produced no JSON: ${text.slice(0, 200)}`);
    }
    return JSON.parse(text.slice(start));
  }
}

function cliEnv(options: CliRunOptions): NodeJS.ProcessEnv {
  if (options.embeddings === "ollama") {
    return {
      ...process.env,
      MNEMON_EMBED_ENDPOINT: options.endpoint ?? OLLAMA_ENDPOINT,
      MNEMON_EMBED_MODEL: options.model ?? OLLAMA_MODEL,
      MNEMON_EMBED_PROTOCOL: "ollama",
    };
  }
  return {
    ...process.env,
    MNEMON_EMBED_ENDPOINT: DEAD_EMBED,
    MNEMON_EMBED_MODEL: "",
    MNEMON_EMBED_API_KEY: "",
    MNEMON_EMBED_PROTOCOL: "ollama",
  };
}

function runCli(bin: string, dataDir: string, args: string[], options: CliRunOptions): unknown {
  const stdout = execFileSync(bin, ["--data-dir", dataDir, ...args], {
    encoding: "utf8",
    env: cliEnv(options),
    timeout: options.embeddings === "ollama" ? 60_000 : 15_000,
    maxBuffer: 4 * 1024 * 1024,
  });
  return parseJson(stdout);
}

export async function ollamaAvailable(
  endpoint = OLLAMA_ENDPOINT,
  model = OLLAMA_MODEL,
): Promise<boolean> {
  try {
    const tags = await fetch(`${endpoint.replace(/\/$/u, "")}/api/tags`, {
      signal: AbortSignal.timeout(2000),
    });
    if (!tags.ok) {
      return false;
    }
    const body = (await tags.json()) as { models?: Array<{ name?: string }> };
    const names = (body.models ?? []).map((m) => String(m.name ?? ""));
    const stem = model.replace(/:latest$/u, "");
    return names.some((name) => name === model || name === `${stem}:latest` || name === stem);
  } catch {
    return false;
  }
}

function sqliteJson(dbPath: string, sql: string): unknown {
  const stdout = execFileSync("sqlite3", ["-json", dbPath, sql], {
    encoding: "utf8",
    timeout: 5_000,
  });
  const text = stdout.trim();
  return text ? JSON.parse(text) : [];
}

function dbPath(dataDir: string): string {
  return join(dataDir, "data/default/mnemon.db");
}

function normalizeEntities(values: unknown): string[] {
  if (!Array.isArray(values)) {
    return [];
  }
  return [...new Set(values.map((v) => String(v).toLowerCase()))].sort();
}

function edgeKey(source: string, target: string, type: string): string {
  return `${source}|${target}|${type}`;
}

function countEdgeTypes(edges: readonly string[]): Record<string, number> {
  const counts: Record<string, number> = {};
  for (const edge of edges) {
    const type = edge.split("|")[2] ?? "unknown";
    counts[type] = (counts[type] ?? 0) + 1;
  }
  return counts;
}

function jaccard(a: readonly string[], b: readonly string[]): number {
  const left = new Set(a);
  const right = new Set(b);
  let inter = 0;
  for (const x of left) {
    if (right.has(x)) {
      inter++;
    }
  }
  const union = left.size + right.size - inter;
  return union === 0 ? 1 : inter / union;
}

function contentIndex(memories: readonly CorpusMemory[]): Map<string, string> {
  return new Map(memories.map((m) => [m.content, m.key]));
}

function keyOf(content: string, byContent: Map<string, string>): string | undefined {
  return byContent.get(content);
}

export async function runTypescript(
  mnemon: Mnemon,
  pool: Pool,
  schema: string,
  fixtures: Fixtures,
): Promise<SideSnapshot> {
  const byContent = contentIndex(fixtures.memories);
  const idToKey = new Map<string, string>();
  const remembers: SideSnapshot["remembers"] = [];

  for (const memory of fixtures.memories) {
    const result = await mnemon.remember({
      content: memory.content,
      category: memory.category,
      source: memory.source,
    });
    if (result.action === "added") {
      idToKey.set(result.insight.id, memory.key);
    } else if (result.duplicateOf) {
      const existing = idToKey.get(result.duplicateOf);
      if (existing) {
        idToKey.set(result.insight.id, existing);
      }
    }
    remembers.push({
      key: memory.key,
      action: result.action,
      entities: normalizeEntities(result.insight.entities),
      id: result.insight.id,
      content: result.insight.content,
    });
  }

  const forgotten = remembers.find((r) => r.key === "soft-delete-me" && r.action === "added");
  if (forgotten) {
    await mnemon.forget(forgotten.id);
  }

  const recalls = await collectRecalls(fixtures, async (query, intent) => {
    const recalled = await mnemon.recall({ query, intent, limit: 10 });
    return {
      intent: recalled.meta.intent,
      hits: recalled.results.map((hit) => ({
        key: idToKey.get(hit.insight.id) ?? keyOf(hit.insight.content, byContent),
        score: hit.score,
        intent: hit.intent,
        signals: hit.signals,
      })),
    };
  });

  const quoted = quoteIdent(schema);
  const edgeRows = await pool.query<{ source_id: string; target_id: string; edge_type: string; source_content: string; target_content: string }>(
    `
    SELECT e.source_id, e.target_id, e.edge_type, s.content AS source_content, t.content AS target_content
    FROM ${quoted}.edges e
    JOIN ${quoted}.insights s ON s.id = e.source_id AND s.deleted_at IS NULL
    JOIN ${quoted}.insights t ON t.id = e.target_id AND t.deleted_at IS NULL
    `,
  );
  const edges = edgeRows.rows
    .map((row) => {
      const source = idToKey.get(row.source_id) ?? keyOf(row.source_content, byContent);
      const target = idToKey.get(row.target_id) ?? keyOf(row.target_content, byContent);
      return source && target ? edgeKey(source, target, row.edge_type) : undefined;
    })
    .filter((k): k is string => k !== undefined)
    .sort();

  const related: Record<string, string[]> = {};
  for (const key of RELATED_KEYS) {
    const row = remembers.find((r) => r.key === key && r.action === "added");
    if (!row) {
      related[key] = [];
      continue;
    }
    const neighbors = await mnemon.related(row.id, { maxDepth: 2, limit: 100 });
    related[key] = neighbors
      .map((insight) => idToKey.get(insight.id) ?? keyOf(insight.content, byContent))
      .filter((k): k is string => k !== undefined && k !== key)
      .sort();
  }

  const active = await pool.query<{ content: string }>(
    `SELECT content FROM ${quoted}.insights WHERE deleted_at IS NULL`,
  );

  return {
    side: "typescript",
    remembers,
    recalls,
    edges,
    related,
    activeKeys: active.rows
      .map((row) => keyOf(row.content, byContent))
      .filter((k): k is string => k !== undefined)
      .sort(),
  };
}

export function runReferenceCli(
  side: "go" | "cpp",
  bin: string,
  fixtures: Fixtures,
  options: CliRunOptions = { embeddings: "disabled" },
): SideSnapshot {
  const dataDir = mkdtempSync(join(tmpdir(), `mnemon-${side}-`));
  const byContent = contentIndex(fixtures.memories);
  const idToKey = new Map<string, string>();
  const remembers: SideSnapshot["remembers"] = [];

  try {
    for (const memory of fixtures.memories) {
      const raw = runCli(
        bin,
        dataDir,
        [
          "remember",
          "--cat",
          memory.category ?? "general",
          "--source",
          memory.source ?? "agent",
          memory.content,
        ],
        options,
      ) as {
        action?: string;
        id?: string;
        content?: string;
        entities?: unknown;
        diff_suggestion?: string;
        replaced_id?: string;
        embedded?: boolean;
      };
      if (options.embeddings === "ollama" && raw.action === "added" && raw.embedded !== true) {
        throw new Error(`${side} remember did not embed; is Ollama reachable at ${options.endpoint ?? OLLAMA_ENDPOINT}?`);
      }
      const action = (raw.action ?? "added") as RememberAction;
      const id = String(raw.id ?? "");
      if (action === "added" || action === "updated") {
        idToKey.set(id, memory.key);
      }
      if (action === "updated" && raw.replaced_id) {
        idToKey.delete(String(raw.replaced_id));
      }
      remembers.push({
        key: memory.key,
        action,
        diffSuggestion: raw.diff_suggestion,
        entities: normalizeEntities(raw.entities),
        id,
        content: String(raw.content ?? memory.content),
      });
    }

    const forgotten = remembers.find((r) => r.key === "soft-delete-me" && (r.action === "added" || r.action === "updated"));
    if (forgotten) {
      runCli(bin, dataDir, ["forget", forgotten.id], options);
    }

    const recalls = collectRecallsSync(fixtures, (query, intent) => {
      const args = ["recall", "--verbose", "--limit", "10"];
      if (intent) {
        args.push("--intent", intent);
      }
      args.push(query);
      const raw = runCli(bin, dataDir, args, options) as {
        meta?: { intent?: string };
        results?: Array<{
          insight?: { id?: string; content?: string };
          score?: number;
          intent?: string;
          signals?: { keyword?: number; entity?: number; similarity?: number; graph?: number };
        }>;
      };
      return {
        intent: raw.meta?.intent ?? "GENERAL",
        hits: (raw.results ?? []).map((hit) => ({
          key: idToKey.get(String(hit.insight?.id ?? "")) ?? keyOf(String(hit.insight?.content ?? ""), byContent),
          score: Number(hit.score ?? 0),
          intent: String(hit.intent ?? raw.meta?.intent ?? "GENERAL"),
          signals: {
            keyword: Number(hit.signals?.keyword ?? 0),
            entity: Number(hit.signals?.entity ?? 0),
            similarity: Number(hit.signals?.similarity ?? 0),
            graph: Number(hit.signals?.graph ?? 0),
          },
        })),
      };
    });

    const rows = sqliteJson(
      dbPath(dataDir),
      `
      SELECT e.source_id, e.target_id, e.edge_type, s.content AS source_content, t.content AS target_content
      FROM edges e
      JOIN insights s ON s.id = e.source_id AND s.deleted_at IS NULL
      JOIN insights t ON t.id = e.target_id AND t.deleted_at IS NULL
      `,
    ) as Array<{ source_id: string; target_id: string; edge_type: string; source_content: string; target_content: string }>;

    const edges = rows
      .map((row) => {
        const source = idToKey.get(row.source_id) ?? keyOf(row.source_content, byContent);
        const target = idToKey.get(row.target_id) ?? keyOf(row.target_content, byContent);
        return source && target ? edgeKey(source, target, row.edge_type) : undefined;
      })
      .filter((k): k is string => k !== undefined)
      .sort();

    const related: Record<string, string[]> = {};
    for (const key of RELATED_KEYS) {
      const row = remembers.find((r) => r.key === key && (r.action === "added" || r.action === "updated"));
      if (!row || !idToKey.has(row.id)) {
        related[key] = [];
        continue;
      }
      const raw = runCli(bin, dataDir, ["related", "--depth", "2", row.id], options) as Array<{
        id?: string;
        content?: string;
      }>;
      related[key] = (Array.isArray(raw) ? raw : [])
        .map((node) => idToKey.get(String(node.id ?? "")) ?? keyOf(String(node.content ?? ""), byContent))
        .filter((k): k is string => k !== undefined && k !== key)
        .sort();
    }

    const activeRows = sqliteJson(
      dbPath(dataDir),
      `SELECT content FROM insights WHERE deleted_at IS NULL`,
    ) as Array<{ content: string }>;

    return {
      side,
      remembers,
      recalls,
      edges,
      related,
      activeKeys: activeRows
        .map((row) => keyOf(row.content, byContent))
        .filter((k): k is string => k !== undefined)
        .sort(),
    };
  } finally {
    rmSync(dataDir, { recursive: true, force: true });
  }
}

async function collectRecalls(
  fixtures: Fixtures,
  fn: (query: string, intent?: RecallIntent) => Promise<{ intent: string; hits: RecallHitView[] }>,
): Promise<SideSnapshot["recalls"]> {
  const out: SideSnapshot["recalls"] = [];
  for (const q of fixtures.queries) {
    const result = await fn(q.query, q.intent);
    out.push({
      queryId: q.id,
      intent: result.intent,
      keys: result.hits.map((h) => h.key),
      hits: result.hits,
    });
  }
  return out;
}

function collectRecallsSync(
  fixtures: Fixtures,
  fn: (query: string, intent?: RecallIntent) => { intent: string; hits: RecallHitView[] },
): SideSnapshot["recalls"] {
  return fixtures.queries.map((q) => {
    const result = fn(q.query, q.intent);
    return {
      queryId: q.id,
      intent: result.intent,
      keys: result.hits.map((h) => h.key),
      hits: result.hits,
    };
  });
}

function expectedHitRate(snapshot: SideSnapshot, fixtures: Fixtures): number {
  let expected = 0;
  let hits = 0;
  for (const q of fixtures.queries) {
    const recalled = snapshot.recalls.find((r) => r.queryId === q.id);
    const keys = recalled?.keys ?? [];
    for (const exp of q.expected) {
      expected++;
      const rank = keys.indexOf(exp.key) + 1;
      if (rank > 0 && rank <= exp.maxRank) {
        hits++;
      }
    }
  }
  return expected === 0 ? 1 : hits / expected;
}

function top5Overlap(a: SideSnapshot, b: SideSnapshot, queryId: string): number {
  const left = new Set((a.recalls.find((r) => r.queryId === queryId)?.keys ?? []).slice(0, 5).filter(Boolean));
  const right = new Set((b.recalls.find((r) => r.queryId === queryId)?.keys ?? []).slice(0, 5).filter(Boolean));
  let inter = 0;
  for (const k of left) {
    if (right.has(k)) {
      inter++;
    }
  }
  return inter / 5;
}

function replaceCascade(reference: SideSnapshot): Set<string> {
  const active = new Set(reference.activeKeys);
  const gone = new Set<string>();
  for (const row of reference.remembers) {
    if ((row.action === "added" || row.action === "updated") && !active.has(row.key) && row.key !== "soft-delete-me") {
      gone.add(row.key);
    }
  }
  return gone;
}

export function compareSnapshots(
  typescript: SideSnapshot,
  references: SideSnapshot[],
  fixtures: Fixtures,
  options: CliRunOptions = { embeddings: "disabled" },
): ParityReport {
  const mismatches: Mismatch[] = [];
  const notes =
    options.embeddings === "ollama"
      ? [
          `Both sides used Ollama ${options.model ?? OLLAMA_MODEL} at ${options.endpoint ?? OLLAMA_ENDPOINT} via /api/embed (no nomic prefixes, matching Go).`,
          "createdAt is still wall-clock because the CLIs cannot ingest fixture timestamps.",
          "TypeScript quality gates with fixture vectors and dates live in test/integration/corpus.test.ts.",
        ]
      : [
          "Reference CLIs cannot accept fixture embeddings or createdAt; this run disables embeddings and uses wall-clock timestamps on every side.",
          "TypeScript quality gates with fixture vectors and dates live in test/integration/corpus.test.ts.",
        ];

  const tsByKey = new Map(typescript.remembers.map((r) => [r.key, r]));

  for (const memory of fixtures.memories) {
    const ts = tsByKey.get(memory.key);
    if (memory.key === "dark-mode-dup" && ts?.action !== "skipped") {
      mismatches.push({
        id: `ts-dup-${memory.key}`,
        judgment: "harmful",
        kind: "remember_action",
        message: `TypeScript must skip exact duplicate ${memory.key}`,
        sides: { typescript: ts?.action },
      });
    }
    if ((memory.key === "dark-mode-ext" || memory.key === "dark-mode-neg") && ts?.action !== "added") {
      mismatches.push({
        id: `ts-keep-${memory.key}`,
        judgment: "harmful",
        kind: "remember_action",
        message: `TypeScript discarded ${memory.key}; extensions and negated corrections must be kept`,
        sides: { typescript: ts?.action },
      });
    }
  }

  for (const q of fixtures.queries) {
    const recalled = typescript.recalls.find((r) => r.queryId === q.id);
    const keys = recalled?.keys ?? [];
    if (keys.includes("soft-delete-me") || (q.forbidden ?? []).some((k) => keys.includes(k))) {
      mismatches.push({
        id: `ts-leak-${q.id}`,
        judgment: "harmful",
        kind: "forgotten_leak",
        message: `TypeScript recall ${q.id} leaked a forgotten or forbidden key`,
        sides: { typescript: keys },
      });
    }
  }

  const metrics: PairMetrics[] = [];
  for (const reference of references) {
    const gone = replaceCascade(reference);
    const refByKey = new Map(reference.remembers.map((r) => [r.key, r]));
    let decisions = 0;
    let decisionHits = 0;
    for (const memory of fixtures.memories) {
      const ts = tsByKey.get(memory.key);
      const ref = refByKey.get(memory.key);
      if (!ts || !ref) {
        continue;
      }
      decisions++;
      const tsNorm = ts.action;
      const refNorm = ref.action === "updated" ? "updated" : ref.action;
      if (tsNorm === refNorm) {
        decisionHits++;
        continue;
      }
      const judgment: Judgment =
        tsNorm === "skipped" && refNorm === "added"
          ? "harmful"
          : tsNorm === "added" && (refNorm === "updated" || refNorm === "skipped")
            ? "intentionally_safer"
            : "architectural";
      mismatches.push({
        id: `decision-${reference.side}-${memory.key}`,
        judgment,
        kind: "remember_action",
        message: `remember ${memory.key}: TypeScript ${tsNorm} vs ${reference.side} ${refNorm}${ref.diffSuggestion ? ` (${ref.diffSuggestion})` : ""}`,
        sides: { typescript: tsNorm, [reference.side]: refNorm },
      });
    }

    for (const q of fixtures.queries) {
      const tsRecall = typescript.recalls.find((r) => r.queryId === q.id);
      const refRecall = reference.recalls.find((r) => r.queryId === q.id);
      const tsKeys = tsRecall?.keys ?? [];
      const refKeys = refRecall?.keys ?? [];
      if (refKeys.includes("soft-delete-me")) {
        mismatches.push({
          id: `ref-leak-${reference.side}-${q.id}`,
          judgment: "harmful",
          kind: "forgotten_leak",
          message: `${reference.side} recall ${q.id} leaked forgotten soft-delete-me`,
          sides: { [reference.side]: refKeys },
        });
      }
      for (const exp of q.expected) {
        const tsRank = tsKeys.indexOf(exp.key) + 1;
        const refRank = refKeys.indexOf(exp.key) + 1;
        if (tsRank === refRank) {
          continue;
        }
        if (refRank === 0 && gone.has(exp.key)) {
          mismatches.push({
            id: `rank-${reference.side}-${q.id}-${exp.key}`,
            judgment: "architectural",
            kind: "recall_rank",
            message: `${q.id}: ${exp.key} missing on ${reference.side} because auto-replace removed it`,
            sides: { typescript: tsRank, [reference.side]: refRank },
          });
          continue;
        }
        if (tsRank > 0 && refRank > 0 && tsRank <= 5 && refRank <= 5) {
          mismatches.push({
            id: `rank-${reference.side}-${q.id}-${exp.key}`,
            judgment: "neutral",
            kind: "recall_rank",
            message: `${q.id}: ${exp.key} rank TypeScript ${tsRank} vs ${reference.side} ${refRank}`,
            sides: { typescript: tsRank, [reference.side]: refRank },
          });
          continue;
        }
        mismatches.push({
          id: `rank-${reference.side}-${q.id}-${exp.key}`,
          judgment: "architectural",
          kind: "recall_rank",
          message: `${q.id}: ${exp.key} rank TypeScript ${tsRank || "absent"} vs ${reference.side} ${refRank || "absent"}`,
          sides: {
            typescript: { rank: tsRank, top5: tsKeys.slice(0, 5), signals: tsRecall?.hits[tsRank - 1]?.signals },
            [reference.side]: { rank: refRank, top5: refKeys.slice(0, 5), signals: refRecall?.hits[refRank - 1]?.signals },
          },
        });
      }

      if (tsRecall && refRecall && tsRecall.intent !== refRecall.intent) {
        mismatches.push({
          id: `intent-${reference.side}-${q.id}`,
          judgment: "neutral",
          kind: "intent",
          message: `${q.id}: intent TypeScript ${tsRecall.intent} vs ${reference.side} ${refRecall.intent}`,
          sides: { typescript: tsRecall.intent, [reference.side]: refRecall.intent },
        });
      }
    }

    if (jaccard(typescript.edges, reference.edges) < 1) {
      const onlyTs = typescript.edges.filter((e) => !reference.edges.includes(e));
      const onlyRef = reference.edges.filter((e) => !typescript.edges.includes(e));
      mismatches.push({
        id: `edges-${reference.side}`,
        judgment: "architectural",
        kind: "edges",
        message: `edge Jaccard ${jaccard(typescript.edges, reference.edges).toFixed(3)} vs ${reference.side} (ts-only ${onlyTs.length}, ref-only ${onlyRef.length})`,
        sides: {
          typescript: { extra: onlyTs.slice(0, 20), byType: countEdgeTypes(onlyTs) },
          [reference.side]: { extra: onlyRef.slice(0, 20), byType: countEdgeTypes(onlyRef) },
        },
      });
    }

    for (const key of RELATED_KEYS) {
      const tsRel = typescript.related[key] ?? [];
      const refRel = reference.related[key] ?? [];
      if (jaccard(tsRel, refRel) < 1) {
        mismatches.push({
          id: `related-${reference.side}-${key}`,
          judgment: "architectural",
          kind: "related",
          message: `related(${key}) set differs from ${reference.side}`,
          sides: { typescript: tsRel, [reference.side]: refRel },
        });
      }
    }

    for (const memory of fixtures.memories) {
      const ts = tsByKey.get(memory.key);
      const ref = refByKey.get(memory.key);
      if (!ts || !ref || ts.action === "skipped" || ref.action === "skipped") {
        continue;
      }
      if (ts.entities.join("\0") !== ref.entities.join("\0")) {
        mismatches.push({
          id: `entities-${reference.side}-${memory.key}`,
          judgment: "architectural",
          kind: "entities",
          message: `${memory.key} entities differ`,
          sides: { typescript: ts.entities, [reference.side]: ref.entities },
        });
      }
    }

    const overlaps = fixtures.queries.map((q) => top5Overlap(typescript, reference, q.id));
    metrics.push({
      reference: reference.side,
      decisionAgreement: decisions === 0 ? 1 : decisionHits / decisions,
      edgeAgreement: jaccard(typescript.edges, reference.edges),
      top5OverlapMean: overlaps.reduce((s, n) => s + n, 0) / Math.max(overlaps.length, 1),
      expectedHitRate: {
        typescript: expectedHitRate(typescript, fixtures),
        reference: expectedHitRate(reference, fixtures),
      },
    });
  }

  return {
    generatedAt: new Date().toISOString(),
    embeddings: options.embeddings,
    timestamps: "wall-clock",
    sides: ["typescript", ...references.map((r) => r.side)],
    metrics,
    mismatches,
    notes,
  };
}

export function writeReports(
  report: ParityReport,
  basename = report.embeddings === "ollama" ? "report-embed" : "report",
): { jsonPath: string; markdownPath: string } {
  mkdirSync(outDir, { recursive: true });
  const jsonPath = join(outDir, `${basename}.json`);
  const markdownPath = join(outDir, `${basename}.md`);
  writeFileSync(jsonPath, `${JSON.stringify(report, null, 2)}\n`);
  writeFileSync(markdownPath, renderMarkdown(report));
  return { jsonPath, markdownPath };
}

function renderMarkdown(report: ParityReport): string {
  const lines: string[] = [
    "# Mnemon parity report",
    "",
    `Generated: ${report.generatedAt}`,
    `Sides: ${report.sides.join(", ")}`,
    `Embeddings: ${report.embeddings}; timestamps: ${report.timestamps}`,
    "",
    "## Metrics",
    "",
  ];
  for (const m of report.metrics) {
    lines.push(
      `- vs ${m.reference}: decision agreement ${pct(m.decisionAgreement)}, edge Jaccard ${pct(m.edgeAgreement)}, mean top-5 overlap ${pct(m.top5OverlapMean)}, expected-hit TS ${pct(m.expectedHitRate.typescript)} / ref ${pct(m.expectedHitRate.reference)}`,
    );
  }
  lines.push("", "## Mismatches", "");
  if (report.mismatches.length === 0) {
    lines.push("None.");
  } else {
    for (const m of report.mismatches) {
      lines.push(`- **${m.judgment}** \`${m.kind}\` ${m.message}`);
    }
  }
  lines.push("", "## Notes", "");
  for (const note of report.notes) {
    lines.push(`- ${note}`);
  }
  lines.push("");
  return lines.join("\n");
}

function pct(n: number): string {
  return `${(n * 100).toFixed(1)}%`;
}

export function harmfulMismatches(report: ParityReport): Mismatch[] {
  return report.mismatches.filter((m) => m.judgment === "harmful");
}
