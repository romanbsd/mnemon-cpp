// SQLite persistence: schema/migrations, MAGMA edges, oplog, embeddings, retention/GC.
// One connection per process; WAL for read-write. Read-only uses file URI immutable=1 (no -wal/-shm writes on RO mounts);
// application-level skips still apply for access log, EI refresh, oplog.
#include "db.hpp"

#include "model_json.hpp"
#include "time_util.hpp"
#include "vector_math.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <cctype>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace fs = std::filesystem;

namespace mnemon {

static constexpr int kTransactionBeginAttempts = 5;
static constexpr int kOplogTrimInterval = 100;

// Read-only mounts (e.g. chmod -R a-w): WAL mode would open/create -wal/-shm next to the DB; that requires
// write access. Open via URI with immutable=1 so SQLite uses only the main db file (parity with Go OpenReadOnly
// using journal_mode OFF). Recent data present only in an uncheckpointed WAL may not be visible.
static std::string pct_encode_uri_path(std::string_view s) {
  static const char hex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
        c == '_' || c == '~' || c == '/') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0xf];
    }
  }
  return out;
}

static std::string sqlite_readonly_uri(const fs::path& dbpath) {
  fs::path abs = fs::absolute(dbpath);
  std::string p = abs.generic_string();
#if defined(_WIN32)
  if (p.size() >= 2 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':') {
    p.insert(p.begin(), '/');
  }
#endif
  if (p.empty() || p[0] != '/') {
    throw std::runtime_error("sqlite readonly uri: path must be absolute: " + abs.string());
  }
  return std::string("file://") + pct_encode_uri_path(p) + "?mode=ro&immutable=1";
}

// SQLITE_DONE/ROW are success for step(); everything else becomes an exception.
static void check_sqlite(int rc, sqlite3* db, const char* ctx) {
  if (rc == SQLITE_OK || rc == SQLITE_DONE || rc == SQLITE_ROW) {
    return;
  }
  const char* msg = db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
  throw std::runtime_error(std::string(ctx) + ": " + (msg ? msg : "sqlite error"));
}

Statement::Statement(sqlite3* db, const char* sql) {
  const char* tail = nullptr;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt_, &tail);
  check_sqlite(rc, db, "prepare");
}

Statement::~Statement() {
  if (stmt_) {
    sqlite3_finalize(stmt_);
  }
}

void Statement::bind_int(int idx, int v) {
  check_sqlite(sqlite3_bind_int(stmt_, idx, v), sqlite3_db_handle(stmt_), "bind_int");
}

void Statement::bind_int64(int idx, int64_t v) {
  check_sqlite(sqlite3_bind_int64(stmt_, idx, v), sqlite3_db_handle(stmt_), "bind_int64");
}

void Statement::bind_double(int idx, double v) {
  check_sqlite(sqlite3_bind_double(stmt_, idx, v), sqlite3_db_handle(stmt_), "bind_double");
}

void Statement::bind_text(int idx, const std::string& s) {
  check_sqlite(sqlite3_bind_text(stmt_, idx, s.c_str(), static_cast<int>(s.size()), SQLITE_TRANSIENT),
               sqlite3_db_handle(stmt_), "bind_text");
}

void Statement::bind_blob(int idx, const void* data, size_t len) {
  check_sqlite(sqlite3_bind_blob(stmt_, idx, data, static_cast<int>(len), SQLITE_TRANSIENT),
               sqlite3_db_handle(stmt_), "bind_blob");
}

void Statement::bind_null(int idx) {
  check_sqlite(sqlite3_bind_null(stmt_, idx), sqlite3_db_handle(stmt_), "bind_null");
}

bool Statement::step() {
  int rc = sqlite3_step(stmt_);
  if (rc == SQLITE_ROW) {
    return true;
  }
  if (rc == SQLITE_DONE) {
    return false;
  }
  check_sqlite(rc, sqlite3_db_handle(stmt_), "step");
  return false;
}

std::string Statement::column_text(int idx) {
  const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, idx));
  if (!t) {
    return {};
  }
  return std::string(t, static_cast<size_t>(sqlite3_column_bytes(stmt_, idx)));
}

int Statement::column_int(int idx) {
  return sqlite3_column_int(stmt_, idx);
}

int64_t Statement::column_int64(int idx) {
  return sqlite3_column_int64(stmt_, idx);
}

double Statement::column_double(int idx) {
  return sqlite3_column_double(stmt_, idx);
}

const void* Statement::column_blob(int idx) {
  return sqlite3_column_blob(stmt_, idx);
}

int Statement::column_bytes(int idx) {
  return sqlite3_column_bytes(stmt_, idx);
}

bool Statement::column_null(int idx) {
  return sqlite3_column_type(stmt_, idx) == SQLITE_NULL;
}

void Statement::reset() {
  sqlite3_reset(stmt_);
}

// --- Database ---

Database::Database(sqlite3* h, std::string path, bool readonly) : db_(h), path_(std::move(path)), readonly_(readonly) {}

Database::~Database() {
  if (db_) {
    sqlite3_close(db_);
  }
}

void Database::exec_sql(const char* sql) {
  char* err = nullptr;
  int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    std::string m = err ? err : "exec";
    sqlite3_free(err);
    throw std::runtime_error(m);
  }
}

static void exec_begin_immediate_with_retry(sqlite3* db) {
  for (int attempt = 0; attempt < kTransactionBeginAttempts; ++attempt) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, &err);
    if (rc == SQLITE_OK) {
      return;
    }
    std::string msg = err ? err : sqlite3_errmsg(db);
    sqlite3_free(err);
    if (rc != SQLITE_BUSY && rc != SQLITE_LOCKED) {
      throw std::runtime_error(msg);
    }
    if (attempt + 1 == kTransactionBeginAttempts) {
      throw std::runtime_error("begin transaction: " + msg);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25 * (attempt + 1)));
  }
}

std::unique_ptr<Database> Database::open_readwrite(const std::string& data_dir) {
  fs::create_directories(data_dir);
  std::string dbpath = (fs::path(data_dir) / "mnemon.db").string();
  sqlite3* h = nullptr;
  int rc = sqlite3_open_v2(dbpath.c_str(), &h, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
  if (rc != SQLITE_OK) {
    std::string err = h ? sqlite3_errmsg(h) : "open";
    if (h) {
      sqlite3_close(h);
    }
    throw std::runtime_error("open database: " + err);
  }
  sqlite3_busy_timeout(h, 5000);
  auto db = std::unique_ptr<Database>(new Database(h, dbpath, false));
  db->exec_sql("PRAGMA journal_mode=WAL;");
  db->exec_sql("PRAGMA foreign_keys=ON;");
  db->migrate();
  return db;
}

std::unique_ptr<Database> Database::open_readonly(const std::string& data_dir) {
  fs::path dbpath = fs::path(data_dir) / "mnemon.db";
  if (!fs::exists(dbpath)) {
    throw std::runtime_error("database not found: " + dbpath.string());
  }
  std::string uri = sqlite_readonly_uri(dbpath);
  sqlite3* h = nullptr;
  int rc = sqlite3_open_v2(uri.c_str(), &h, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr);
  if (rc != SQLITE_OK) {
    std::string err = h ? sqlite3_errmsg(h) : "open";
    if (h) {
      sqlite3_close(h);
    }
    throw std::runtime_error("open readonly database: " + err);
  }
  sqlite3_busy_timeout(h, 5000);
  auto db = std::unique_ptr<Database>(new Database(h, dbpath.string(), true));
  db->exec_sql("PRAGMA foreign_keys=ON;");
  return db;
}

static void add_column_ignore_dup(sqlite3* db, const char* stmt) {
  char* err = nullptr;
  int rc = sqlite3_exec(db, stmt, nullptr, nullptr, &err);
  if (rc == SQLITE_OK) {
    return;
  }
  std::string msg = err ? err : "";
  sqlite3_free(err);
  if (msg.find("duplicate column") != std::string::npos) {
    return;
  }
  throw std::runtime_error(msg);
}

// Probe + migrate away from removed fifth edge type "narrative" (spec: four-graph MAGMA only).
void Database::migrate_remove_narrative_edges() {
  char* err = nullptr;
  int rc = sqlite3_exec(db_,
                        "INSERT INTO edges VALUES ('__test','__test','narrative',0,'{}',datetime('now'))", nullptr,
                        nullptr, &err);
  if (rc != SQLITE_OK) {
    sqlite3_free(err);
    return; // CHECK constraint already excludes narrative — nothing to do
  }
  // cleanup test row and migrate
  in_transaction([&] {
    exec_sql("DELETE FROM edges WHERE source_id = '__test'");
    exec_sql("DELETE FROM edges WHERE edge_type = 'narrative'");
    exec_sql("ALTER TABLE edges RENAME TO edges_old");
    exec_sql(
        "CREATE TABLE edges ("
        "source_id   TEXT NOT NULL,"
        "target_id   TEXT NOT NULL,"
        "edge_type   TEXT NOT NULL CHECK(edge_type IN ('temporal','semantic','causal','entity')),"
        "weight      REAL DEFAULT 1.0,"
        "metadata    TEXT DEFAULT '{}',"
        "created_at  TEXT NOT NULL,"
        "PRIMARY KEY (source_id, target_id, edge_type),"
        "FOREIGN KEY (source_id) REFERENCES insights(id) ON DELETE CASCADE,"
        "FOREIGN KEY (target_id) REFERENCES insights(id) ON DELETE CASCADE"
        ")");
    exec_sql("INSERT INTO edges SELECT * FROM edges_old");
    exec_sql("DROP TABLE edges_old");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_edges_source ON edges(source_id)");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_edges_target ON edges(target_id)");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_edges_type ON edges(edge_type)");
  });
}

// Idempotent DDL: base tables + incremental ALTERs; narrative edge/category cleanup matches Go migrations.
void Database::migrate() {
  const char* schema = R"SQL(
CREATE TABLE IF NOT EXISTS insights (
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
);

CREATE TABLE IF NOT EXISTS edges (
    source_id   TEXT NOT NULL,
    target_id   TEXT NOT NULL,
    edge_type   TEXT NOT NULL CHECK(edge_type IN ('temporal','semantic','causal','entity')),
    weight      REAL DEFAULT 1.0,
    metadata    TEXT DEFAULT '{}',
    created_at  TEXT NOT NULL,
    PRIMARY KEY (source_id, target_id, edge_type),
    FOREIGN KEY (source_id) REFERENCES insights(id) ON DELETE CASCADE,
    FOREIGN KEY (target_id) REFERENCES insights(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_insights_category    ON insights(category);
CREATE INDEX IF NOT EXISTS idx_insights_importance  ON insights(importance);
CREATE INDEX IF NOT EXISTS idx_insights_created     ON insights(created_at);
CREATE INDEX IF NOT EXISTS idx_insights_deleted     ON insights(deleted_at);
CREATE INDEX IF NOT EXISTS idx_insights_source      ON insights(source);
CREATE INDEX IF NOT EXISTS idx_edges_source         ON edges(source_id);
CREATE INDEX IF NOT EXISTS idx_edges_target         ON edges(target_id);
CREATE INDEX IF NOT EXISTS idx_edges_type           ON edges(edge_type);
CREATE INDEX IF NOT EXISTS idx_edges_source_type    ON edges(source_id, edge_type);
CREATE INDEX IF NOT EXISTS idx_edges_target_type    ON edges(target_id, edge_type);

CREATE TABLE IF NOT EXISTS oplog (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    operation   TEXT NOT NULL,
    insight_id  TEXT,
    detail      TEXT DEFAULT '',
    created_at  TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_oplog_created ON oplog(created_at);
)SQL";
  exec_sql(schema);

  add_column_ignore_dup(db_, "ALTER TABLE insights ADD COLUMN last_accessed_at      TEXT");
  add_column_ignore_dup(db_, "ALTER TABLE insights ADD COLUMN embedding             BLOB");
  add_column_ignore_dup(db_, "ALTER TABLE insights ADD COLUMN effective_importance  REAL DEFAULT 0.5");

  exec_sql("CREATE INDEX IF NOT EXISTS idx_insights_effective_imp ON insights(effective_importance)");
  exec_sql("CREATE INDEX IF NOT EXISTS idx_prune_candidates ON insights(deleted_at, importance, access_count, effective_importance)");

  migrate_remove_narrative_edges();

  // Legacy content used category "narrative"; soft-delete those rows (no longer a valid category).
  Statement st(db_, "SELECT COUNT(*) FROM insights WHERE category = 'narrative' AND deleted_at IS NULL");
  st.step();
  int n = st.column_int(0);
  if (n > 0) {
    exec_sql("UPDATE insights SET deleted_at = datetime('now') WHERE category = 'narrative' AND deleted_at IS NULL");
  }
}

// Re-entrant depth counter: nested callers share one BEGIN/COMMIT pair.
void Database::in_transaction(std::function<void()> fn) {
  const bool outermost = tx_depth_ == 0;
  if (outermost) {
    exec_begin_immediate_with_retry(db_);
  }
  ++tx_depth_;
  bool depth_decremented = false;
  try {
    fn();
    --tx_depth_;
    depth_decremented = true;
    if (outermost) {
      exec_sql("COMMIT;");
    }
  } catch (...) {
    if (!depth_decremented) {
      --tx_depth_;
    }
    if (outermost) {
      try {
        exec_sql("ROLLBACK;");
      } catch (...) {
      }
    }
    throw;
  }
}

Insight Database::scan_insight_row(Statement& st) {
  Insight i;
  i.id = st.column_text(0);
  i.content = st.column_text(1);
  i.category = st.column_text(2);
  i.importance = st.column_int(3);
  std::string tags = st.column_text(4);
  std::string ents = st.column_text(5);
  i.source = st.column_text(6);
  i.access_count = st.column_int(7);
  i.created_at = time_util::parse_rfc3339(st.column_text(8));
  i.updated_at = time_util::parse_rfc3339(st.column_text(9));
  if (!st.column_null(10) && !st.column_text(10).empty()) {
    i.deleted_at = time_util::parse_rfc3339(st.column_text(10));
  }
  auto tj = nlohmann::json::parse(tags, nullptr, false);
  if (tj.is_array()) {
    for (const auto& x : tj) {
      if (x.is_string()) {
        i.tags.push_back(x.get<std::string>());
      }
    }
  }
  auto ej = nlohmann::json::parse(ents, nullptr, false);
  if (ej.is_array()) {
    for (const auto& x : ej) {
      if (x.is_string()) {
        i.entities.push_back(x.get<std::string>());
      }
    }
  }
  return i;
}

Edge Database::scan_edge_row(Statement& st) {
  Edge e;
  e.source_id = st.column_text(0);
  e.target_id = st.column_text(1);
  auto et = parse_edge_type(st.column_text(2));
  if (!et) {
    throw std::runtime_error("bad edge type");
  }
  e.edge_type = *et;
  e.weight = st.column_double(3);
  parse_metadata(st.column_text(4), e.metadata);
  e.created_at = time_util::parse_rfc3339(st.column_text(5));
  return e;
}

void Database::insert_insight(const Insight& i) {
  nlohmann::json tj = i.tags;
  nlohmann::json ej = i.entities;
  Statement st(db_,
               "INSERT INTO insights (id, content, category, importance, tags, entities, source, access_count, "
               "created_at, updated_at) VALUES (?,?,?,?,?,?,?,?,?,?)");
  st.bind_text(1, i.id);
  st.bind_text(2, i.content);
  st.bind_text(3, i.category);
  st.bind_int(4, i.importance);
  st.bind_text(5, tj.dump());
  st.bind_text(6, ej.dump());
  st.bind_text(7, i.source);
  st.bind_int(8, i.access_count);
  st.bind_text(9, time_util::rfc3339_utc(i.created_at));
  st.bind_text(10, time_util::rfc3339_utc(i.updated_at));
  st.step();
}

std::optional<Insight> Database::get_insight_by_id(const std::string& id) {
  Statement st(db_,
               "SELECT id, content, category, importance, tags, entities, source, access_count, created_at, "
               "updated_at, deleted_at FROM insights WHERE id = ? AND deleted_at IS NULL");
  st.bind_text(1, id);
  if (!st.step()) {
    return std::nullopt;
  }
  return scan_insight_row(st);
}

std::optional<Insight> Database::get_insight_by_id_include_deleted(const std::string& id) {
  Statement st(db_,
               "SELECT id, content, category, importance, tags, entities, source, access_count, created_at, "
               "updated_at, deleted_at FROM insights WHERE id = ?");
  st.bind_text(1, id);
  if (!st.step()) {
    return std::nullopt;
  }
  return scan_insight_row(st);
}

std::vector<Insight> Database::query_insights(const QueryFilter& f) {
  std::string q = "SELECT id, content, category, importance, tags, entities, source, access_count, created_at, "
                  "updated_at, deleted_at FROM insights WHERE deleted_at IS NULL";
  std::vector<std::string> binds;
  if (!f.keyword.empty()) {
    q += " AND content LIKE ?";
    binds.push_back("%" + f.keyword + "%");
  }
  if (!f.category.empty()) {
    q += " AND category = ?";
    binds.push_back(f.category);
  }
  if (!f.source.empty()) {
    q += " AND source = ?";
    binds.push_back(f.source);
  }
  q += " ORDER BY importance DESC, created_at DESC LIMIT ?";
  Statement st(db_, q.c_str());
  int idx = 1;
  for (const auto& b : binds) {
    st.bind_text(idx++, b);
  }
  int lim = f.limit > 0 ? f.limit : 20;
  st.bind_int(idx, lim);
  std::vector<Insight> out;
  while (st.step()) {
    out.push_back(scan_insight_row(st));
  }
  return out;
}

void Database::soft_delete_insight(const std::string& id) {
  std::string now = time_util::rfc3339_utc(time_util::now_utc());
  Statement st(db_, "UPDATE insights SET deleted_at = ?, updated_at = ? WHERE id = ? AND deleted_at IS NULL");
  st.bind_text(1, now);
  st.bind_text(2, now);
  st.bind_text(3, id);
  st.step();
  if (sqlite3_changes(db_) == 0) {
    throw std::runtime_error("insight " + id + " not found or already deleted");
  }
  delete_edges_by_node(id);
}

void Database::update_entities(const std::string& id, const std::vector<std::string>& entities) {
  nlohmann::json ej = entities;
  Statement st(db_, "UPDATE insights SET entities = ?, updated_at = ? WHERE id = ?");
  st.bind_text(1, ej.dump());
  st.bind_text(2, time_util::rfc3339_utc(time_util::now_utc()));
  st.bind_text(3, id);
  st.step();
}

// Recall side-effect: bump access_count for ranking/EI. Read-only opens skip (no writes to the file).
void Database::increment_access_count(const std::string& id) {
  if (readonly_) {
    return;
  }
  Statement st(db_, "UPDATE insights SET access_count = access_count + 1, last_accessed_at = ? WHERE id = ?");
  st.bind_text(1, time_util::rfc3339_utc(time_util::now_utc()));
  st.bind_text(2, id);
  st.step();
}

// Spec parity: access_factor = max(1, log(1+access_count)); edge cap 5; half-life decay on kHalfLifeDays.
double Database::compute_effective_importance(int importance, int access_count, double days_since_access,
                                              int edge_count) {
  double base = 0.15;
  switch (importance) {
  case 5:
    base = 1.0;
    break;
  case 4:
    base = 0.8;
    break;
  case 3:
    base = 0.5;
    break;
  case 2:
    base = 0.3;
    break;
  default:
    base = 0.15;
    break;
  }
  double access_factor = std::log(1.0 + static_cast<double>(access_count));
  if (access_factor < 1.0) {
    access_factor = 1.0;
  }
  double decay = std::pow(0.5, days_since_access / kHalfLifeDays);
  int ec = edge_count;
  if (ec > 5) {
    ec = 5;
  }
  double edge_factor = 1.0 + 0.1 * static_cast<double>(ec);
  return base * access_factor * decay * edge_factor;
}

bool Database::is_immune(int importance, int access_count) {
  return importance >= 4 || access_count >= 3;
}

// Recompute EI from live importance, access, staleness, and undirected edge degree (source + target counts).
std::pair<double, bool> Database::refresh_effective_importance(const std::string& id) {
  Statement st(db_,
               "SELECT importance, access_count, created_at, last_accessed_at FROM insights WHERE id = ? AND "
               "deleted_at IS NULL");
  st.bind_text(1, id);
  if (!st.step()) {
    return {0.0, false};
  }
  int imp = st.column_int(0);
  int ac = st.column_int(1);
  auto created = time_util::parse_rfc3339(st.column_text(2));
  TimePoint last_access = created;
  if (!st.column_null(3)) {
    last_access = time_util::parse_rfc3339(st.column_text(3));
  }
  double days = std::chrono::duration<double>(time_util::now_utc() - last_access).count() / 86400.0;

  Statement ec(
      db_, "SELECT (SELECT COUNT(*) FROM edges WHERE source_id = ?) + (SELECT COUNT(*) FROM edges WHERE target_id = ?)");
  ec.bind_text(1, id);
  ec.bind_text(2, id);
  ec.step();
  int edge_c = ec.column_int(0);

  double ei = compute_effective_importance(imp, ac, days, edge_c);
  if (!readonly_) {
    Statement up(db_, "UPDATE insights SET effective_importance = ? WHERE id = ?");
    up.bind_double(1, ei);
    up.bind_text(2, id);
    up.step();
  }
  return {ei, true};
}

// GC preview: recompute EI for all live rows, persist updates in one transaction, return low-EI non-immune rows.
std::tuple<std::vector<RetentionCandidate>, int> Database::get_retention_candidates(double threshold, int limit) {
  std::map<std::string, int> edge_counts;
  {
    Statement st(db_, "SELECT id, SUM(cnt) FROM ("
                       "SELECT source_id AS id, COUNT(*) AS cnt FROM edges GROUP BY source_id "
                       "UNION ALL "
                       "SELECT target_id AS id, COUNT(*) AS cnt FROM edges GROUP BY target_id "
                       ") GROUP BY id");
    while (st.step()) {
      edge_counts[st.column_text(0)] = st.column_int(1);
    }
  }

  // Re-query with last_accessed — simpler second pass
  struct Row {
    Insight ins;
    TimePoint last_access;
  };
  std::vector<Row> rows;
  {
    Statement st(db_, "SELECT id, content, category, importance, tags, entities, source, access_count, created_at, "
                     "updated_at, deleted_at, last_accessed_at FROM insights WHERE deleted_at IS NULL");
    while (st.step()) {
      Row r;
      r.ins.id = st.column_text(0);
      r.ins.content = st.column_text(1);
      r.ins.category = st.column_text(2);
      r.ins.importance = st.column_int(3);
      std::string tags = st.column_text(4);
      std::string ents = st.column_text(5);
      r.ins.source = st.column_text(6);
      r.ins.access_count = st.column_int(7);
      r.ins.created_at = time_util::parse_rfc3339(st.column_text(8));
      r.ins.updated_at = time_util::parse_rfc3339(st.column_text(9));
      auto tj = nlohmann::json::parse(tags, nullptr, false);
      if (tj.is_array()) {
        for (const auto& x : tj) {
          if (x.is_string()) {
            r.ins.tags.push_back(x.get<std::string>());
          }
        }
      }
      auto ej = nlohmann::json::parse(ents, nullptr, false);
      if (ej.is_array()) {
        for (const auto& x : ej) {
          if (x.is_string()) {
            r.ins.entities.push_back(x.get<std::string>());
          }
        }
      }
      r.last_access = r.ins.created_at;
      if (!st.column_null(11)) {
        r.last_access = time_util::parse_rfc3339(st.column_text(11));
      }
      rows.push_back(std::move(r));
    }
  }

  auto now = time_util::now_utc();
  std::vector<RetentionCandidate> candidates;
  std::vector<std::pair<std::string, double>> ei_updates;
  for (const auto& r : rows) {
    double days = std::chrono::duration<double>(now - r.last_access).count() / 86400.0;
    int ec = edge_counts[r.ins.id];
    double ei = compute_effective_importance(r.ins.importance, r.ins.access_count, days, ec);
    bool immune = is_immune(r.ins.importance, r.ins.access_count);
    ei_updates.push_back({r.ins.id, ei});
    if (ei < threshold && !immune) {
      RetentionCandidate c;
      c.insight = r.ins;
      c.effective_importance = ei;
      c.days_since_access = days;
      c.edge_count = ec;
      c.immune = immune;
      candidates.push_back(std::move(c));
    }
  }
  // Persist refreshed EI for every scanned row in one transaction; rollback on any statement failure.
  if (!ei_updates.empty() && !readonly_) {
    try {
      in_transaction([&] {
        for (const auto& u : ei_updates) {
          Statement up(db_, "UPDATE insights SET effective_importance = ? WHERE id = ?");
          up.bind_double(1, u.second);
          up.bind_text(2, u.first);
          up.step();
        }
      });
    } catch (const std::exception& ex) {
      std::cerr << "warning: batch EI update failed, rolled back: " << ex.what() << "\n";
    }
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const RetentionCandidate& a, const RetentionCandidate& b) { return a.effective_importance < b.effective_importance; });
  int total = static_cast<int>(rows.size());
  if (limit > 0 && static_cast<int>(candidates.size()) > limit) {
    candidates.resize(static_cast<size_t>(limit));
  }
  return {candidates, total};
}

int Database::auto_prune(int max_insights, const std::vector<std::string>& exclude_ids) {
  int pruned = 0;
  auto run = [&] {
    Statement ct(db_, "SELECT COUNT(*) FROM insights WHERE deleted_at IS NULL");
    ct.step();
    int total = ct.column_int(0);
    if (total <= max_insights) {
      return;
    }
    int excess = total - max_insights;
    if (excess > kPruneBatchSize) {
      excess = kPruneBatchSize;
    }
    std::string q = "SELECT id FROM insights WHERE deleted_at IS NULL AND importance < 4 AND access_count < 3 ";
    for (const auto& e : exclude_ids) {
      (void)e;
    }
    if (!exclude_ids.empty()) {
      q += "AND id NOT IN (";
      for (size_t i = 0; i < exclude_ids.size(); ++i) {
        if (i) {
          q += ",";
        }
        q += "?";
      }
      q += ") ";
    }
    q += "ORDER BY effective_importance ASC LIMIT ?";
    Statement st(db_, q.c_str());
    int bi = 1;
    for (const auto& e : exclude_ids) {
      st.bind_text(bi++, e);
    }
    st.bind_int(bi, excess);
    std::vector<std::string> ids;
    while (st.step()) {
      ids.push_back(st.column_text(0));
    }
    std::string now = time_util::rfc3339_utc(time_util::now_utc());
    for (const auto& id : ids) {
      Statement u(db_, "UPDATE insights SET deleted_at = ?, updated_at = ? WHERE id = ? AND deleted_at IS NULL");
      u.bind_text(1, now);
      u.bind_text(2, now);
      u.bind_text(3, id);
      u.step();
      if (sqlite3_changes(db_) > 0) {
        delete_edges_by_node(id);
        pruned++;
      }
    }
  };
  if (tx_depth_ > 0) {
    run();
  } else {
    in_transaction(run);
  }
  return pruned;
}

void Database::boost_retention(const std::string& id) {
  std::string now = time_util::rfc3339_utc(time_util::now_utc());
  Statement st(db_,
               "UPDATE insights SET access_count = access_count + 3, last_accessed_at = ?, updated_at = ? WHERE id "
               "= ? AND deleted_at IS NULL");
  st.bind_text(1, now);
  st.bind_text(2, now);
  st.bind_text(3, id);
  st.step();
  if (sqlite3_changes(db_) == 0) {
    throw std::runtime_error("insight " + id + " not found or already deleted");
  }
}

std::vector<Insight> Database::get_recent_insights_in_window(const std::string& exclude_id, double window_hours,
                                                             int limit) {
  auto cutoff = time_util::now_utc() - std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                            std::chrono::duration<double>(window_hours * 3600.0));
  Statement st(db_,
               "SELECT id, content, category, importance, tags, entities, source, access_count, created_at, "
               "updated_at, deleted_at FROM insights WHERE id != ? AND deleted_at IS NULL AND created_at >= ? "
               "ORDER BY created_at DESC LIMIT ?");
  st.bind_text(1, exclude_id);
  st.bind_text(2, time_util::rfc3339_utc(cutoff));
  st.bind_int(3, limit);
  std::vector<Insight> out;
  while (st.step()) {
    out.push_back(scan_insight_row(st));
  }
  return out;
}

std::optional<Insight> Database::get_latest_insight_by_source(const std::string& source, const std::string& exclude_id) {
  Statement st(db_,
               "SELECT id, content, category, importance, tags, entities, source, access_count, created_at, updated_at, "
               "deleted_at FROM insights WHERE source = ? AND id != ? AND deleted_at IS NULL "
               "ORDER BY created_at DESC, rowid DESC LIMIT 1");
  st.bind_text(1, source);
  st.bind_text(2, exclude_id);
  if (!st.step()) {
    return std::nullopt;
  }
  return scan_insight_row(st);
}

std::vector<Insight> Database::get_recent_insights_by_source(const std::string& source, const std::string& exclude_id,
                                                             int limit) {
  Statement st(db_,
               "SELECT id, content, category, importance, tags, entities, source, access_count, created_at, updated_at, "
               "deleted_at FROM insights WHERE source = ? AND id != ? AND deleted_at IS NULL "
               "ORDER BY created_at DESC LIMIT ?");
  st.bind_text(1, source);
  st.bind_text(2, exclude_id);
  st.bind_int(3, limit);
  std::vector<Insight> out;
  while (st.step()) {
    out.push_back(scan_insight_row(st));
  }
  return out;
}

std::vector<Insight> Database::get_all_active_insights() {
  Statement st(db_,
               "SELECT id, content, category, importance, tags, entities, source, access_count, created_at, updated_at, "
               "deleted_at FROM insights WHERE deleted_at IS NULL ORDER BY created_at DESC");
  std::vector<Insight> out;
  while (st.step()) {
    out.push_back(scan_insight_row(st));
  }
  return out;
}

InsightStats Database::get_stats() {
  InsightStats s;
  Statement t(db_, "SELECT COUNT(*) FROM insights WHERE deleted_at IS NULL");
  t.step();
  s.total = t.column_int(0);
  Statement d(db_, "SELECT COUNT(*) FROM insights WHERE deleted_at IS NOT NULL");
  d.step();
  s.deleted_count = d.column_int(0);
  Statement c(db_, "SELECT category, COUNT(*) FROM insights WHERE deleted_at IS NULL GROUP BY category");
  while (c.step()) {
    s.by_category[c.column_text(0)] = c.column_int(1);
  }
  // Row count includes bidirectional pairs (e.g. entity/temporal often insert two directed edges per relation).
  Statement e(db_, "SELECT COUNT(*) FROM edges");
  e.step();
  s.edge_count = e.column_int(0);
  Statement o(db_, "SELECT COUNT(*) FROM oplog");
  o.step();
  s.oplog_count = o.column_int(0);
  Statement te(
      db_,
      "SELECT je.value, COUNT(DISTINCT i.id) as cnt FROM insights i, json_each(i.entities) je WHERE i.deleted_at IS "
      "NULL GROUP BY je.value ORDER BY cnt DESC LIMIT 20");
  while (te.step()) {
    EntityStat es;
    es.entity = te.column_text(0);
    es.count = te.column_int(1);
    s.top_entities.push_back(std::move(es));
  }
  return s;
}

void Database::update_embedding(const std::string& id, const std::vector<float>& v) {
  auto blob = mnemon::serialize_vector(v);
  Statement st(db_, "UPDATE insights SET embedding = ?, updated_at = ? WHERE id = ?");
  st.bind_blob(1, blob.data(), blob.size());
  st.bind_text(2, time_util::rfc3339_utc(time_util::now_utc()));
  st.bind_text(3, id);
  st.step();
}

std::vector<float> Database::get_embedding(const std::string& id) {
  Statement st(db_, "SELECT embedding FROM insights WHERE id = ? AND deleted_at IS NULL");
  st.bind_text(1, id);
  if (!st.step()) {
    throw std::runtime_error("no embedding");
  }
  if (st.column_null(0)) {
    return {};
  }
  const void* p = st.column_blob(0);
  int n = st.column_bytes(0);
  return mnemon::deserialize_vector(p, static_cast<size_t>(n));
}

std::vector<EmbeddedRow> Database::get_all_embeddings() {
  Statement st(db_, "SELECT id, embedding FROM insights WHERE deleted_at IS NULL AND embedding IS NOT NULL");
  std::vector<EmbeddedRow> out;
  while (st.step()) {
    EmbeddedRow r;
    r.id = st.column_text(0);
    if (!st.column_null(1)) {
      const void* p = st.column_blob(1);
      int n = st.column_bytes(1);
      r.embedding = mnemon::deserialize_vector(p, static_cast<size_t>(n));
    }
    if (!r.embedding.empty()) {
      out.push_back(std::move(r));
    }
  }
  return out;
}

std::tuple<int, int> Database::embedding_stats() {
  Statement t(db_, "SELECT COUNT(*) FROM insights WHERE deleted_at IS NULL");
  t.step();
  int total = t.column_int(0);
  Statement e(db_, "SELECT COUNT(*) FROM insights WHERE deleted_at IS NULL AND embedding IS NOT NULL");
  e.step();
  int emb = e.column_int(0);
  return {total, emb};
}

std::vector<Insight> Database::get_insights_without_embedding(int limit) {
  int lim = limit > 0 ? limit : 100;
  Statement st(db_,
               "SELECT id, content, category, importance, tags, entities, source, access_count, created_at, updated_at, "
               "deleted_at FROM insights WHERE deleted_at IS NULL AND embedding IS NULL "
               "ORDER BY importance DESC, created_at DESC LIMIT ?");
  st.bind_int(1, lim);
  std::vector<Insight> out;
  while (st.step()) {
    out.push_back(scan_insight_row(st));
  }
  return out;
}

void Database::insert_edge(const Edge& e) {
  nlohmann::json mj(e.metadata);
  Statement st(db_,
               "INSERT OR REPLACE INTO edges (source_id, target_id, edge_type, weight, metadata, created_at) "
               "VALUES (?,?,?,?,?,?)");
  st.bind_text(1, e.source_id);
  st.bind_text(2, e.target_id);
  st.bind_text(3, edge_type_str(e.edge_type));
  st.bind_double(4, e.weight);
  st.bind_text(5, mj.dump());
  st.bind_text(6, time_util::rfc3339_utc(e.created_at));
  st.step();
}

// Incident edges: union of outgoing and incoming (exclude duplicate row when source==target).
std::vector<Edge> Database::get_edges_by_node(const std::string& node_id) {
  Statement st(db_,
               "SELECT source_id, target_id, edge_type, weight, metadata, created_at FROM edges WHERE source_id = ? "
               "UNION ALL "
               "SELECT source_id, target_id, edge_type, weight, metadata, created_at FROM edges WHERE target_id = ? "
               "AND source_id != ?");
  st.bind_text(1, node_id);
  st.bind_text(2, node_id);
  st.bind_text(3, node_id);
  std::vector<Edge> out;
  while (st.step()) {
    out.push_back(scan_edge_row(st));
  }
  return out;
}

std::vector<Edge> Database::get_edges_by_node_and_type(const std::string& node_id, EdgeType t) {
  std::string ts = edge_type_str(t);
  Statement st(db_,
               "SELECT source_id, target_id, edge_type, weight, metadata, created_at FROM edges WHERE source_id = ? "
               "AND edge_type = ? "
               "UNION ALL "
               "SELECT source_id, target_id, edge_type, weight, metadata, created_at FROM edges WHERE target_id = ? "
               "AND edge_type = ? AND source_id != ?");
  st.bind_text(1, node_id);
  st.bind_text(2, ts);
  st.bind_text(3, node_id);
  st.bind_text(4, ts);
  st.bind_text(5, node_id);
  std::vector<Edge> out;
  while (st.step()) {
    out.push_back(scan_edge_row(st));
  }
  return out;
}

std::vector<Edge> Database::get_edges_by_source_and_type(const std::string& source_id, EdgeType t) {
  Statement st(db_,
               "SELECT source_id, target_id, edge_type, weight, metadata, created_at FROM edges WHERE source_id = ? "
               "AND edge_type = ?");
  st.bind_text(1, source_id);
  st.bind_text(2, edge_type_str(t));
  std::vector<Edge> out;
  while (st.step()) {
    out.push_back(scan_edge_row(st));
  }
  return out;
}

std::vector<std::string> Database::find_insights_with_entity(const std::string& entity, const std::string& exclude_id,
                                                             int limit) {
  Statement st(db_,
               "SELECT DISTINCT i.id FROM insights i, json_each(i.entities) je "
               "WHERE i.deleted_at IS NULL AND i.id != ? AND je.value = ? "
               "ORDER BY i.created_at DESC LIMIT ?");
  st.bind_text(1, exclude_id);
  st.bind_text(2, entity);
  st.bind_int(3, limit);
  std::vector<std::string> out;
  while (st.step()) {
    out.push_back(st.column_text(0));
  }
  return out;
}

std::vector<Edge> Database::get_all_edges() {
  Statement st(db_, "SELECT source_id, target_id, edge_type, weight, metadata, created_at FROM edges");
  std::vector<Edge> out;
  while (st.step()) {
    out.push_back(scan_edge_row(st));
  }
  return out;
}

void Database::delete_edge(const std::string& source_id, const std::string& target_id,
                           EdgeType edge_type) {
  Statement st(db_, "DELETE FROM edges WHERE source_id = ? AND target_id = ? AND edge_type = ?");
  st.bind_text(1, source_id);
  st.bind_text(2, target_id);
  st.bind_text(3, edge_type_str(edge_type));
  st.step();
}

void Database::delete_edges_by_node(const std::string& node_id) {
  Statement st(db_, "DELETE FROM edges WHERE source_id = ? OR target_id = ?");
  st.bind_text(1, node_id);
  st.bind_text(2, node_id);
  st.step();
}

std::vector<Insight> Database::get_active_insights_by_source_ordered(const std::string& source) {
  Statement st(db_,
               "SELECT id, content, category, importance, tags, entities, source, access_count, created_at, updated_at, "
               "deleted_at FROM insights WHERE source = ? AND deleted_at IS NULL "
               "ORDER BY created_at ASC, rowid ASC");
  st.bind_text(1, source);
  std::vector<Insight> out;
  while (st.step()) {
    out.push_back(scan_insight_row(st));
  }
  return out;
}

// Audit trail; trimmed lazily to kMaxOplogEntries. Read-only and failures are best-effort (stderr only).
void Database::log_op(const std::string& operation, const std::string& insight_id, const std::string& detail) {
  if (readonly_) {
    return;
  }
  try {
    Statement st(db_, "INSERT INTO oplog (operation, insight_id, detail, created_at) VALUES (?,?,?,?)");
    st.bind_text(1, operation);
    if (insight_id.empty()) {
      st.bind_null(2);
    } else {
      st.bind_text(2, insight_id);
    }
    st.bind_text(3, detail);
    st.bind_text(4, time_util::rfc3339_utc(time_util::now_utc()));
    st.step();
    const auto inserted_id = sqlite3_last_insert_rowid(db_);
    if (inserted_id > kMaxOplogEntries && inserted_id % kOplogTrimInterval == 0) {
      Statement tr(db_, "DELETE FROM oplog WHERE id <= (SELECT MAX(id) FROM oplog) - ?");
      tr.bind_int(1, kMaxOplogEntries);
      tr.step();
    }
  } catch (const std::exception& ex) {
    std::cerr << "warning: oplog: " << ex.what() << "\n";
  }
}

std::vector<OplogEntry> Database::get_oplog(int limit) {
  int lim = limit > 0 ? limit : 20;
  Statement st(db_, "SELECT id, operation, insight_id, detail, created_at FROM oplog ORDER BY id DESC LIMIT ?");
  st.bind_int(1, lim);
  std::vector<OplogEntry> out;
  while (st.step()) {
    OplogEntry e;
    e.id = st.column_int(0);
    e.operation = st.column_text(1);
    if (!st.column_null(2)) {
      e.insight_id = st.column_text(2);
    }
    e.detail = st.column_text(3);
    e.created_at = st.column_text(4);
    out.push_back(std::move(e));
  }
  return out;
}

} // namespace mnemon
