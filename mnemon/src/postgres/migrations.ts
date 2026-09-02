import type { Pool } from "pg";

import { quoteIdent } from "../config.js";
import { MnemonConfigurationError } from "../errors.js";
import { wrapDatabaseError, withTransaction } from "./transaction.js";

export const MIGRATION_VERSION = 1;

export async function runMigrations(pool: Pool, schema: string): Promise<number> {
  const s = quoteIdent(schema);
  try {
    await pool.query("CREATE EXTENSION IF NOT EXISTS vector");
  } catch (error) {
    const code = (error as { code?: string }).code;
    if (code !== "23505") {
      throw new MnemonConfigurationError(
        "pgvector extension is unavailable; CREATE EXTENSION vector failed",
        { cause: error },
      );
    }
  }

  return withTransaction(pool, async (client) => {
    await client.query(`CREATE SCHEMA IF NOT EXISTS ${s}`);
    await client.query(`
      CREATE TABLE IF NOT EXISTS ${s}.schema_migrations (
          version      integer PRIMARY KEY,
          applied_at   timestamptz NOT NULL DEFAULT clock_timestamp()
      )
    `);

    const existing = await client.query<{ version: number }>(
      `SELECT version FROM ${s}.schema_migrations WHERE version = $1`,
      [MIGRATION_VERSION],
    );
    if ((existing.rowCount ?? 0) > 0) {
      return MIGRATION_VERSION;
    }

    await client.query(`
      CREATE TABLE IF NOT EXISTS ${s}.settings (
          key          text PRIMARY KEY,
          value        jsonb NOT NULL,
          updated_at   timestamptz NOT NULL DEFAULT clock_timestamp()
      )
    `);

    await client.query(`
      CREATE TABLE IF NOT EXISTS ${s}.insights (
          id                    uuid PRIMARY KEY,
          content               text NOT NULL,
          normalized_content    text NOT NULL,
          content_hash          text NOT NULL,
          search_tokens         text[] NOT NULL DEFAULT '{}'::text[],
          category              text NOT NULL
                                CHECK (category IN ('preference','decision','fact','insight','context','general')),
          importance            smallint NOT NULL CHECK (importance BETWEEN 1 AND 5),
          tags                  jsonb NOT NULL DEFAULT '[]'::jsonb
                                CHECK (jsonb_typeof(tags) = 'array'),
          entities              jsonb NOT NULL DEFAULT '[]'::jsonb
                                CHECK (jsonb_typeof(entities) = 'array'),
          source                text NOT NULL,
          access_count          integer NOT NULL DEFAULT 0 CHECK (access_count >= 0),
          stored_at             timestamptz NOT NULL DEFAULT clock_timestamp(),
          created_at            timestamptz NOT NULL,
          updated_at            timestamptz NOT NULL,
          deleted_at            timestamptz,
          last_accessed_at      timestamptz,
          embedding             vector,
          search_tsv            tsvector GENERATED ALWAYS AS (
                                  setweight(to_tsvector('english'::regconfig, coalesce(content, '')), 'A')
                                ) STORED,
          effective_importance  double precision NOT NULL DEFAULT 0.5
      )
    `);

    await client.query(`
      CREATE UNIQUE INDEX IF NOT EXISTS insights_active_content_hash_uq
          ON ${s}.insights (content_hash)
          WHERE deleted_at IS NULL
    `);
    await client.query(`
      CREATE INDEX IF NOT EXISTS insights_active_created_idx
          ON ${s}.insights (created_at DESC, id)
          WHERE deleted_at IS NULL
    `);
    await client.query(`
      CREATE INDEX IF NOT EXISTS insights_active_source_created_idx
          ON ${s}.insights (source, created_at DESC, id)
          WHERE deleted_at IS NULL
    `);
    await client.query(`
      CREATE INDEX IF NOT EXISTS insights_search_tokens_gin_idx
          ON ${s}.insights USING gin (search_tokens)
          WHERE deleted_at IS NULL
    `);
    await client.query(`
      CREATE INDEX IF NOT EXISTS insights_search_tsv_gin_idx
          ON ${s}.insights USING gin (search_tsv)
          WHERE deleted_at IS NULL
    `);
    await client.query(`
      CREATE INDEX IF NOT EXISTS insights_entities_gin_idx
          ON ${s}.insights USING gin (entities jsonb_path_ops)
          WHERE deleted_at IS NULL
    `);
    await client.query(`
      CREATE INDEX IF NOT EXISTS insights_active_embedding_present_idx
          ON ${s}.insights (id)
          WHERE deleted_at IS NULL AND embedding IS NOT NULL
    `);

    await client.query(`
      CREATE TABLE IF NOT EXISTS ${s}.edges (
          source_id   uuid NOT NULL REFERENCES ${s}.insights(id) ON DELETE CASCADE,
          target_id   uuid NOT NULL REFERENCES ${s}.insights(id) ON DELETE CASCADE,
          edge_type   text NOT NULL
                      CHECK (edge_type IN ('temporal','semantic','causal','entity')),
          weight      double precision NOT NULL DEFAULT 1.0
                      CHECK (weight >= 0.0 AND weight <= 1.0),
          metadata    jsonb NOT NULL DEFAULT '{}'::jsonb
                      CHECK (jsonb_typeof(metadata) = 'object'),
          created_at  timestamptz NOT NULL,
          PRIMARY KEY (source_id, target_id, edge_type),
          CHECK (source_id <> target_id)
      )
    `);
    await client.query(`
      CREATE INDEX IF NOT EXISTS edges_target_type_idx
          ON ${s}.edges (target_id, edge_type)
    `);
    await client.query(`
      CREATE INDEX IF NOT EXISTS edges_source_type_idx
          ON ${s}.edges (source_id, edge_type)
    `);

    await client.query(`
      CREATE TABLE IF NOT EXISTS ${s}.oplog (
          id          bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
          operation   text NOT NULL,
          insight_id  uuid,
          detail      jsonb NOT NULL DEFAULT '{}'::jsonb,
          created_at  timestamptz NOT NULL
      )
    `);
    await client.query(`
      CREATE INDEX IF NOT EXISTS oplog_created_idx
          ON ${s}.oplog (created_at DESC, id DESC)
    `);

    await client.query(`INSERT INTO ${s}.schema_migrations (version) VALUES ($1)`, [MIGRATION_VERSION]);
    return MIGRATION_VERSION;
  }).catch((error: unknown) => {
    if (error instanceof MnemonConfigurationError) {
      throw error;
    }
    throw wrapDatabaseError(error);
  });
}
