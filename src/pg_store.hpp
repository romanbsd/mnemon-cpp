#pragma once

// PostgreSQL implementation of the Store interface (docs/postgres-pgvector.md,
// Phase 2). Compiled only when MNEMON_WITH_POSTGRES is defined.
//
// Deviations from the design proposal, deliberate and parity-first (see PR):
//   - id and the timestamp columns are TEXT, not UUID/TIMESTAMPTZ. TEXT stores
//     the exact RFC 3339 UTC string the model already carries and reproduces
//     SQLite's lexicographic created_at comparisons byte-for-byte, with zero
//     marshalling. Native types buy nothing at Phase 2 scale.
//   - embedding is BYTEA holding the same little-endian float32 blob SQLite
//     writes (serialize_vector), so Phase 2 needs no pgvector extension and is
//     dimension-safe. The pgvector `vector` column + HNSW index arrive in
//     Phase 3, which also does the one-time bytea -> vector copy.
// tags/entities/metadata ARE JSONB, per the proposal, to get the entity GIN
// index and server-side jsonb_array_elements.
#include "store.hpp"

#include <memory>
#include <string>
#include <vector>

namespace mnemon::pg {

// `store` subcommand backed by Postgres schemas (docs §4.1). A store maps to a
// schema named "mnemon_<store>"; these operate on the DSN with the default
// search_path.
std::vector<std::string> list_stores(const std::string& dsn);
bool store_exists(const std::string& dsn, const std::string& store);
void create_store(const std::string& dsn, const std::string& store);
void remove_store(const std::string& dsn, const std::string& store);

// Backend factory used by commands.cpp when a --database-url / MNEMON_DATABASE_URL
// is active. `store` selects the schema.
std::unique_ptr<Store> open_readwrite(const std::string& dsn, const std::string& store);
std::unique_ptr<Store> open_readonly(const std::string& dsn, const std::string& store);

} // namespace mnemon::pg
