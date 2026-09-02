import { randomUUID } from "node:crypto";

import { Pool } from "pg";

import { createMnemon } from "../../src/mnemon.js";
import type { Mnemon } from "../../src/types.js";
import type { Clock } from "../../src/clock.js";
import type { EmbeddingProvider } from "../../src/embedding-provider.js";
import { quoteIdent } from "../../src/config.js";

export const TEST_DATABASE_URL =
  process.env.MNEMON_DATABASE_URL ??
  process.env.DATABASE_URL ??
  "postgresql://mnemon:mnemon@127.0.0.1:55432/mnemon";

export async function postgresAvailable(): Promise<boolean> {
  const pool = new Pool({
    connectionString: TEST_DATABASE_URL,
    connectionTimeoutMillis: 1500,
  });
  try {
    await pool.query("SELECT 1");
    return true;
  } catch {
    return false;
  } finally {
    await pool.end();
  }
}

export function uniqueSchema(): string {
  return `mnemon_t_${randomUUID().replaceAll("-", "").slice(0, 12)}`;
}

export async function withMnemon(
  options: {
    clock?: Clock;
    embeddingProvider?: EmbeddingProvider;
    pool?: Pool;
    databaseUrl?: string;
  },
  fn: (mnemon: Mnemon, ctx: { schema: string; pool: Pool }) => Promise<void>,
): Promise<void> {
  const schema = uniqueSchema();
  const ownsPool = !options.pool;
  const pool =
    options.pool ??
    new Pool({
      connectionString: options.databaseUrl ?? TEST_DATABASE_URL,
    });
  const mnemon = createMnemon({
    pool,
    schema,
    clock: options.clock,
    embeddingProvider: options.embeddingProvider,
  });
  try {
    await mnemon.initialize();
    await fn(mnemon, { schema, pool });
  } finally {
    await mnemon.close();
    await pool.query(`DROP SCHEMA IF EXISTS ${quoteIdent(schema)} CASCADE`);
    if (ownsPool) {
      await pool.end();
    }
  }
}
