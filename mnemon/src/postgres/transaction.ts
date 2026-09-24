import type { Pool, PoolClient } from "pg";

import { MnemonDatabaseError, MnemonError } from "../errors.js";

export function wrapDatabaseError(error: unknown): MnemonDatabaseError {
  if (error instanceof MnemonDatabaseError) {
    return error;
  }
  const err = error as { message?: string; code?: string };
  return new MnemonDatabaseError("database operation failed", {
    cause: error,
    code: typeof err.code === "string" ? err.code : undefined,
  });
}

export async function withTransaction<T>(pool: Pool, fn: (client: PoolClient) => Promise<T>): Promise<T> {
  const client = await pool.connect();
  let destroyClient = false;
  try {
    await client.query("BEGIN");
    const result = await fn(client);
    await client.query("COMMIT");
    return result;
  } catch (error) {
    try {
      await client.query("ROLLBACK");
    } catch (rollbackError) {
      destroyClient = true;
      const wrapped = wrapDatabaseError(error);
      (wrapped as MnemonDatabaseError & { rollbackError?: unknown }).rollbackError = rollbackError;
      throw wrapped;
    }
    if (error instanceof MnemonError && !(error instanceof MnemonDatabaseError)) {
      throw error;
    }
    throw wrapDatabaseError(error);
  } finally {
    client.release(destroyClient);
  }
}
