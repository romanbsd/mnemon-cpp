#include <catch2/catch_test_macros.hpp>

#include "../src/db.hpp"
#include "../src/model.hpp"
#include "../src/time_util.hpp"

#include <sqlite3.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

using namespace mnemon;
namespace fs = std::filesystem;

namespace {

// Regression coverage for upstream commits a4faa7249af7 (auto-prune must log
// an oplog entry) and b1ca65942390 (narrative-edge migration masked by FK
// enforcement, aborted by orphaned edges).

std::string make_temp_dir() {
  std::string tmpl = (fs::temp_directory_path() / "mnemon_db_test_XXXXXX").string();
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  REQUIRE(mkdtemp(buf.data()) != nullptr);
  return std::string(buf.data());
}

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

} // namespace

TEST_CASE("auto_prune records an oplog entry for every pruned insight") {
  auto dir = make_temp_dir();
  auto db = Database::open_readwrite(dir);

  for (int i = 0; i < 5; ++i) {
    db->insert_insight(make_insight("audit-" + std::to_string(i)));
  }

  int pruned = db->auto_prune(3, {});
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

TEST_CASE("narrative-edge migration survives orphaned edges and actually runs under FK enforcement") {
  auto dir = make_temp_dir();
  std::string dbpath = dir + "/mnemon.db";

  // Seed a legacy database on disk: edges CHECK constraint still admits the
  // removed 'narrative' type, plus an edge whose target row does not exist
  // (the kind of damage a `.recover` or a write made with FK enforcement off
  // leaves behind).
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
               "INSERT INTO edges VALUES ('narr-src','vanished','semantic',0.5,'{}','2026-01-01T00:00:00Z')");
  sqlite3_close(raw);

  // Reopening through Database runs migrate(); it must not be blocked by the
  // orphaned edge, and the FK-masked probe must no longer skip the rebuild.
  auto db = Database::open_readwrite(dir);
  db.reset(); // release the connection before inspecting the file directly

  sqlite3* verify = nullptr;
  REQUIRE(sqlite3_open(dbpath.c_str(), &verify) == SQLITE_OK);

  int rc = sqlite3_exec(
      verify, "INSERT INTO edges VALUES ('narr-src','narr-src','narrative',1,'{}','2026-01-01T00:00:00Z')", nullptr,
      nullptr, nullptr);
  REQUIRE(rc != SQLITE_OK); // narrative edge type must be rejected after migration

  sqlite3_stmt* st = nullptr;
  REQUIRE(sqlite3_prepare_v2(verify, "SELECT COUNT(*) FROM edges WHERE target_id='vanished'", -1, &st, nullptr) ==
          SQLITE_OK);
  REQUIRE(sqlite3_step(st) == SQLITE_ROW);
  REQUIRE(sqlite3_column_int(st, 0) == 1); // orphaned edge survived the rebuild verbatim
  sqlite3_finalize(st);
  sqlite3_close(verify);
}
