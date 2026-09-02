#include <catch2/catch_test_macros.hpp>

#include "../src/db.hpp"
#include "../src/model.hpp"
#include "../src/time_util.hpp"

#include <sqlite3.h>

#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>

using namespace mnemon;
namespace fs = std::filesystem;

namespace {

// Regression coverage for upstream commits a4faa7249af7 (auto-prune must log
// an oplog entry) and b1ca65942390 (narrative-edge migration masked by FK
// enforcement, aborted by orphaned edges).

// Self-cleaning temp directory. std::filesystem (not POSIX mkdtemp) keeps
// this test target portable to MSVC, where cpp_tests also builds.
struct TempDir {
  fs::path path;

  TempDir() {
    auto base = fs::temp_directory_path();
    std::random_device rd;
    std::mt19937_64 gen(rd());
    for (int attempt = 0; attempt < 100; ++attempt) {
      auto candidate = base / ("mnemon_db_test_" + std::to_string(gen()));
      std::error_code ec;
      if (fs::create_directory(candidate, ec)) {
        path = candidate;
        return;
      }
    }
    FAIL("could not create a unique temp directory under " << base);
  }

  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  std::string string() const { return path.string(); }
};

Insight make_insight(const std::string& id) {
  Insight ins;
  ins.id = id;
  ins.content = "content";
  ins.created_at = time_util::now_utc();
  ins.updated_at = ins.created_at;
  return ins;
}

void exec_or_fail(sqlite3* db, const char* sql) {
  char* err = nullptr;
  int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
  std::string msg = err ? err : "";
  sqlite3_free(err);
  REQUIRE(rc == SQLITE_OK);
  (void)msg;
}

int count_rows(sqlite3* db, const char* sql) {
  sqlite3_stmt* st = nullptr;
  REQUIRE(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(st) == SQLITE_ROW);
  int n = sqlite3_column_int(st, 0);
  sqlite3_finalize(st);
  return n;
}

} // namespace

TEST_CASE("auto_prune records an oplog entry for every pruned insight") {
  TempDir dir;
  auto db = Database::open_readwrite(dir.string());

  for (int i = 0; i < 5; ++i) {
    db->insert_insight(make_insight("audit-" + std::to_string(i)));
  }

  // Disable the newborn grace period so the just-inserted rows are eligible;
  // this test exercises the audit-logging path, not the age protection.
  setenv("MNEMON_AUTO_PRUNE_MIN_AGE", "0", 1);
  int pruned = db->auto_prune(3, {});
  unsetenv("MNEMON_AUTO_PRUNE_MIN_AGE");
  REQUIRE(pruned == 2);

  auto entries = db->get_oplog(50);
  int logged = 0;
  for (const auto& e : entries) {
    if (e.operation == "prune") {
      ++logged;
      REQUIRE_FALSE(e.detail.empty());
    }
  }
  REQUIRE(logged == pruned);
}

TEST_CASE("auto_prune_min_age_seconds parses durations, day suffixes, and rejects junk") {
  setenv("MNEMON_AUTO_PRUNE_MIN_AGE", "0", 1);
  CHECK(auto_prune_min_age_seconds() == 0);
  setenv("MNEMON_AUTO_PRUNE_MIN_AGE", "24h", 1);
  CHECK(auto_prune_min_age_seconds() == 24L * 3600);
  setenv("MNEMON_AUTO_PRUNE_MIN_AGE", "7d", 1);
  CHECK(auto_prune_min_age_seconds() == 7L * 24 * 3600);
  setenv("MNEMON_AUTO_PRUNE_MIN_AGE", "30m", 1);
  CHECK(auto_prune_min_age_seconds() == 1800);
  setenv("MNEMON_AUTO_PRUNE_MIN_AGE", "1h30m", 1);
  CHECK(auto_prune_min_age_seconds() == 5400);
  // invalid and negative fall back to the 24h default
  setenv("MNEMON_AUTO_PRUNE_MIN_AGE", "bogus", 1);
  CHECK(auto_prune_min_age_seconds() == kDefaultAutoPruneMinAgeSeconds);
  setenv("MNEMON_AUTO_PRUNE_MIN_AGE", "-5h", 1);
  CHECK(auto_prune_min_age_seconds() == kDefaultAutoPruneMinAgeSeconds);
  unsetenv("MNEMON_AUTO_PRUNE_MIN_AGE");
  CHECK(auto_prune_min_age_seconds() == kDefaultAutoPruneMinAgeSeconds);
}

TEST_CASE("narrative-edge migration survives orphaned edges and actually runs under FK enforcement") {
  TempDir dir;
  std::string dbpath = dir.string() + "/mnemon.db";

  // Seed a legacy database on disk: edges CHECK constraint still admits the
  // removed 'narrative' type, plus a legacy narrative edge itself, plus an
  // edge whose target row does not exist (the kind of damage a `.recover` or
  // a write made with FK enforcement off leaves behind).
  sqlite3* raw = nullptr;
  REQUIRE(sqlite3_open(dbpath.c_str(), &raw) == SQLITE_OK);
  exec_or_fail(raw, "PRAGMA foreign_keys=OFF");
  exec_or_fail(raw, R"SQL(
CREATE TABLE insights (
    id          TEXT PRIMARY KEY,
    content     TEXT NOT NULL,
    category    TEXT DEFAULT 'general',
    importance  INTEGER DEFAULT 3,
    tags        TEXT DEFAULT '[]',
    entities    TEXT DEFAULT '[]',
    source      TEXT DEFAULT 'user',
    access_count INTEGER DEFAULT 0,
    created_at  TEXT NOT NULL,
    updated_at  TEXT NOT NULL,
    deleted_at  TEXT
)
)SQL");
  exec_or_fail(raw, R"SQL(
CREATE TABLE edges (
    source_id   TEXT NOT NULL,
    target_id   TEXT NOT NULL,
    edge_type   TEXT NOT NULL CHECK(edge_type IN ('temporal','semantic','causal','entity','narrative')),
    weight      REAL DEFAULT 1.0,
    metadata    TEXT DEFAULT '{}',
    created_at  TEXT NOT NULL,
    PRIMARY KEY (source_id, target_id, edge_type),
    FOREIGN KEY (source_id) REFERENCES insights(id) ON DELETE CASCADE,
    FOREIGN KEY (target_id) REFERENCES insights(id) ON DELETE CASCADE
)
)SQL");
  exec_or_fail(raw, R"SQL(
CREATE TABLE oplog (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    operation   TEXT NOT NULL,
    insight_id  TEXT,
    detail      TEXT DEFAULT '',
    created_at  TEXT NOT NULL
)
)SQL");
  exec_or_fail(raw,
               "INSERT INTO insights VALUES ('narr-src','content','general',3,'[]','[]','user',0,"
               "datetime('now'),datetime('now'),NULL)");
  exec_or_fail(raw,
               "INSERT INTO edges VALUES ('narr-src','narr-src','narrative',1,'{}','2026-01-01T00:00:00Z')");
  exec_or_fail(raw,
               "INSERT INTO edges VALUES ('narr-src','vanished','semantic',0.5,'{}','2026-01-01T00:00:00Z')");
  sqlite3_close(raw);

  // Reopening through Database runs migrate(); it must not be blocked by the
  // orphaned edge, and the FK-masked probe must no longer skip the rebuild.
  auto db = Database::open_readwrite(dir.string());
  db.reset(); // release the connection before inspecting the file directly

  sqlite3* verify = nullptr;
  REQUIRE(sqlite3_open(dbpath.c_str(), &verify) == SQLITE_OK);

  // The legacy narrative edge must have been dropped by the migration.
  REQUIRE(count_rows(verify, "SELECT COUNT(*) FROM edges WHERE edge_type='narrative'") == 0);

  // And the CHECK constraint must now reject the type outright.
  int rc = sqlite3_exec(
      verify, "INSERT INTO edges VALUES ('narr-src','narr-src','narrative',1,'{}','2026-01-01T00:00:00Z')", nullptr,
      nullptr, nullptr);
  REQUIRE(rc != SQLITE_OK); // narrative edge type must be rejected after migration

  // The orphaned edge survived the rebuild verbatim.
  REQUIRE(count_rows(verify, "SELECT COUNT(*) FROM edges WHERE target_id='vanished'") == 1);

  sqlite3_close(verify);
}
