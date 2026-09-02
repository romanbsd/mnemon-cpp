// PostgreSQL backend (docs/postgres-pgvector.md, Phase 2). See pg_store.hpp for
// the deliberate TEXT/BYTEA parity choices. PostgresStore itself is private to
// this translation unit — libpq never appears in the Store interface.
#include "pg_store.hpp"

#include "model_json.hpp"
#include "time_util.hpp"
#include "vector_math.hpp"

#include <libpq-fe.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace mnemon::pg {
namespace {

using OptStr = std::optional<std::string>;
using Params = std::vector<OptStr>;

std::string quote_ident(const std::string& s) {
  // Store names are validated against ^[a-zA-Z0-9][a-zA-Z0-9_-]*$ upstream, so no
  // embedded double-quote is possible; wrapping in quotes handles the '-' case.
  return "\"" + s + "\"";
}

std::string schema_for(const std::string& store) { return "mnemon_" + store; }

// Mask the password in a DSN before it is ever shown (e.g. `status` db_path).
// Covers both URI (user:pass@host) and keyword (password=...) forms.
std::string sanitize_dsn(const std::string& dsn) {
  std::string s = std::regex_replace(dsn, std::regex(R"((://[^:/@\s]+):[^@\s]+@)"), "$1:***@");
  s = std::regex_replace(s, std::regex(R"((password\s*=\s*)('[^']*'|[^\s]+))", std::regex::icase), "$1***");
  return s;
}

std::vector<std::string> parse_json_str_array(const std::string& encoded) {
  std::vector<std::string> values;
  auto json = nlohmann::json::parse(encoded, nullptr, false);
  if (!json.is_array()) {
    return values;
  }
  values.reserve(json.size());
  for (const auto& v : json) {
    if (v.is_string()) {
      values.push_back(v.get<std::string>());
    }
  }
  return values;
}

// pgvector text literal: "[v0,v1,...]". %.9g round-trips a float32 exactly.
std::string vector_literal(std::span<const float> v) {
  std::string out = "[";
  char buf[24];
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) {
      out += ',';
    }
    std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(v[i]));
    out += buf;
  }
  out += "]";
  return out;
}

// Parse pgvector's text output "[v0,v1,...]" back into floats.
std::vector<float> parse_vector_text(const std::string& s) {
  std::vector<float> out;
  size_t i = 0;
  while (i < s.size() && s[i] != '[') {
    ++i;
  }
  if (i < s.size()) {
    ++i; // past '['
  }
  while (i < s.size() && s[i] != ']') {
    while (i < s.size() && (s[i] == ' ' || s[i] == ',')) {
      ++i;
    }
    if (i >= s.size() || s[i] == ']') {
      break;
    }
    size_t start = i;
    while (i < s.size() && s[i] != ',' && s[i] != ']') {
      ++i;
    }
    out.push_back(std::strtof(s.c_str() + start, nullptr));
  }
  return out;
}

class PgResult {
public:
  explicit PgResult(PGresult* r) : r_(r) {}
  ~PgResult() {
    if (r_) {
      PQclear(r_);
    }
  }
  PgResult(PgResult&& o) noexcept : r_(o.r_) { o.r_ = nullptr; }
  PgResult& operator=(PgResult&& o) noexcept {
    if (this != &o) {
      if (r_) {
        PQclear(r_);
      }
      r_ = o.r_;
      o.r_ = nullptr;
    }
    return *this;
  }
  PgResult(const PgResult&) = delete;
  PgResult& operator=(const PgResult&) = delete;

  int rows() const { return PQntuples(r_); }
  bool is_null(int row, int col) const { return PQgetisnull(r_, row, col) != 0; }
  std::string str(int row, int col) const {
    return is_null(row, col) ? std::string() : std::string(PQgetvalue(r_, row, col));
  }
  int as_int(int row, int col) const { return static_cast<int>(std::stol(str(row, col))); }
  double as_double(int row, int col) const { return std::stod(str(row, col)); }
  long affected() const {
    const char* c = PQcmdTuples(r_);
    return (c && *c) ? std::stol(c) : 0;
  }

private:
  PGresult* r_{nullptr};
};

PGconn* connect_or_throw(const std::string& dsn) {
  PGconn* c = PQconnectdb(dsn.c_str());
  if (PQstatus(c) != CONNECTION_OK) {
    std::string msg = PQerrorMessage(c);
    PQfinish(c);
    throw std::runtime_error("cannot connect to postgres: " + msg);
  }
  // Idempotent DDL and DROP ... CASCADE emit NOTICEs; keep them off stderr.
  PQclear(PQexec(c, "SET client_min_messages = warning"));
  return c;
}

class PostgresStore : public Store {
public:
  PostgresStore(PGconn* c, std::string dsn, bool readonly) : conn_(c), path_(std::move(dsn)), readonly_(readonly) {}
  ~PostgresStore() override {
    if (conn_) {
      PQfinish(conn_);
    }
  }

  const std::string& path() const override { return path_; }
  bool is_readonly() const noexcept override { return readonly_; }

  void in_transaction(std::function<void()> fn) override {
    const bool outermost = tx_depth_ == 0;
    if (outermost) {
      exec_simple("BEGIN");
    }
    ++tx_depth_;
    bool depth_decremented = false;
    try {
      fn();
      --tx_depth_;
      depth_decremented = true;
      if (outermost) {
        exec_simple("COMMIT");
      }
    } catch (...) {
      if (!depth_decremented) {
        --tx_depth_;
      }
      if (outermost) {
        try {
          exec_simple("ROLLBACK");
        } catch (...) {
        }
      }
      throw;
    }
  }

  void insert_insight(const Insight& i) override {
    nlohmann::json tj = i.tags;
    nlohmann::json ej = i.entities;
    exec("INSERT INTO insights (id, content, category, importance, tags, entities, source, access_count, "
         "created_at, updated_at) VALUES ($1,$2,$3,$4,$5::jsonb,$6::jsonb,$7,$8,$9,$10)",
         {i.id, i.content, i.category, std::to_string(i.importance), tj.dump(), ej.dump(), i.source,
          std::to_string(i.access_count), time_util::rfc3339_utc(i.created_at),
          time_util::rfc3339_utc(i.updated_at)});
  }

  std::optional<Insight> get_insight_by_id(const std::string& id) override {
    auto q = exec("SELECT " + insight_cols() + " FROM insights WHERE id = $1 AND deleted_at IS NULL", {id});
    if (q.rows() == 0) {
      return std::nullopt;
    }
    return scan_insight(q, 0);
  }

  std::optional<Insight> get_insight_by_id_include_deleted(const std::string& id) override {
    auto q = exec("SELECT " + insight_cols() + " FROM insights WHERE id = $1", {id});
    if (q.rows() == 0) {
      return std::nullopt;
    }
    return scan_insight(q, 0);
  }

  std::vector<Insight> query_insights(const QueryFilter& f) override {
    std::string q = "SELECT " + insight_cols() + " FROM insights WHERE deleted_at IS NULL";
    Params binds;
    int n = 0;
    if (!f.keyword.empty()) {
      q += " AND content LIKE $" + std::to_string(++n);
      binds.push_back("%" + f.keyword + "%");
    }
    if (!f.category.empty()) {
      q += " AND category = $" + std::to_string(++n);
      binds.push_back(f.category);
    }
    if (!f.source.empty()) {
      q += " AND source = $" + std::to_string(++n);
      binds.push_back(f.source);
    }
    q += " ORDER BY importance DESC, created_at DESC LIMIT $" + std::to_string(++n);
    binds.push_back(std::to_string(f.limit > 0 ? f.limit : 20));
    return scan_insights(exec(q, binds));
  }

  void soft_delete_insight(const std::string& id) override {
    update_active("UPDATE insights SET deleted_at = $1, updated_at = $1 WHERE id = $2 AND deleted_at IS NULL", id);
    delete_edges_by_node(id);
  }

  void update_entities(const std::string& id, const std::vector<std::string>& entities) override {
    nlohmann::json ej = entities;
    exec("UPDATE insights SET entities = $1::jsonb, updated_at = $2 WHERE id = $3",
         {ej.dump(), now_str(), id});
  }

  void increment_access_count(const std::string& id) override {
    if (readonly_) {
      return;
    }
    exec("UPDATE insights SET access_count = access_count + 1, last_accessed_at = $1 WHERE id = $2", {now_str(), id});
  }

  std::pair<double, bool> refresh_effective_importance(const std::string& id) override {
    auto q = exec("SELECT importance, access_count, created_at, last_accessed_at FROM insights "
                  "WHERE id = $1 AND deleted_at IS NULL",
                  {id});
    if (q.rows() == 0) {
      return {0.0, false};
    }
    int imp = q.as_int(0, 0);
    int ac = q.as_int(0, 1);
    auto created = time_util::parse_rfc3339(q.str(0, 2));
    TimePoint last_access = created;
    if (!q.is_null(0, 3)) {
      last_access = time_util::parse_rfc3339(q.str(0, 3));
    }
    double days = std::chrono::duration<double>(time_util::now_utc() - last_access).count() / 86400.0;

    auto ec = exec("SELECT (SELECT COUNT(*) FROM edges WHERE source_id = $1) + "
                   "(SELECT COUNT(*) FROM edges WHERE target_id = $1)",
                   {id});
    int edge_c = ec.as_int(0, 0);

    double ei = compute_effective_importance(imp, ac, days, edge_c);
    if (!readonly_) {
      exec("UPDATE insights SET effective_importance = $1 WHERE id = $2", {fmt_double(ei), id});
    }
    return {ei, true};
  }

  std::tuple<std::vector<RetentionCandidate>, int> get_retention_candidates(double threshold, int limit) override {
    std::map<std::string, int> edge_counts;
    {
      auto q = exec("SELECT id, SUM(cnt) FROM ("
                    "SELECT source_id AS id, COUNT(*) AS cnt FROM edges GROUP BY source_id "
                    "UNION ALL "
                    "SELECT target_id AS id, COUNT(*) AS cnt FROM edges GROUP BY target_id "
                    ") s GROUP BY id",
                    {});
      for (int r = 0; r < q.rows(); ++r) {
        edge_counts[q.str(r, 0)] = q.as_int(r, 1);
      }
    }
    struct Row {
      Insight ins;
      TimePoint last_access;
    };
    std::vector<Row> rows;
    {
      auto q = exec("SELECT " + insight_cols() + ", last_accessed_at FROM insights WHERE deleted_at IS NULL", {});
      for (int r = 0; r < q.rows(); ++r) {
        Row row;
        row.ins = scan_insight(q, r);
        row.last_access = row.ins.created_at;
        if (!q.is_null(r, 11)) {
          row.last_access = time_util::parse_rfc3339(q.str(r, 11));
        }
        rows.push_back(std::move(row));
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
    if (!ei_updates.empty() && !readonly_) {
      try {
        in_transaction([&] {
          for (const auto& u : ei_updates) {
            exec("UPDATE insights SET effective_importance = $1 WHERE id = $2", {fmt_double(u.second), u.first});
          }
        });
      } catch (const std::exception& ex) {
        std::cerr << "warning: batch EI update failed, rolled back: " << ex.what() << "\n";
      }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const RetentionCandidate& a, const RetentionCandidate& b) {
                return a.effective_importance < b.effective_importance;
              });
    int total = static_cast<int>(rows.size());
    if (limit > 0 && static_cast<int>(candidates.size()) > limit) {
      candidates.resize(static_cast<size_t>(limit));
    }
    return {candidates, total};
  }

  int auto_prune(int max_insights, const std::vector<std::string>& exclude_ids) override {
    int pruned = 0;
    auto run = [&] {
      auto ct = exec("SELECT COUNT(*) FROM insights WHERE deleted_at IS NULL", {});
      int total = ct.as_int(0, 0);
      if (total <= max_insights) {
        return;
      }
      int excess = total - max_insights;
      if (excess > kPruneBatchSize) {
        excess = kPruneBatchSize;
      }
      std::string q = "SELECT id FROM insights WHERE deleted_at IS NULL AND importance < 4 AND access_count < 3 ";
      Params binds;
      int n = 0;
      if (!exclude_ids.empty()) {
        q += "AND id NOT IN (";
        for (size_t i = 0; i < exclude_ids.size(); ++i) {
          if (i) {
            q += ",";
          }
          q += "$" + std::to_string(++n);
          binds.push_back(exclude_ids[i]);
        }
        q += ") ";
      }
      q += "ORDER BY effective_importance ASC LIMIT $" + std::to_string(++n);
      binds.push_back(std::to_string(excess));
      auto sel = exec(q, binds);
      std::vector<std::string> ids;
      for (int r = 0; r < sel.rows(); ++r) {
        ids.push_back(sel.str(r, 0));
      }
      std::string now = now_str();
      for (const auto& id : ids) {
        auto u = exec("UPDATE insights SET deleted_at = $1, updated_at = $1 WHERE id = $2 AND deleted_at IS NULL",
                      {now, id});
        if (u.affected() > 0) {
          delete_edges_by_node(id);
          log_op("prune", id,
                 "auto-prune: over capacity (active=" + std::to_string(total) + ", max=" +
                     std::to_string(max_insights) + ")");
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

  void boost_retention(const std::string& id) override {
    update_active("UPDATE insights SET access_count = access_count + 3, last_accessed_at = $1, updated_at = $1 "
                  "WHERE id = $2 AND deleted_at IS NULL",
                  id);
  }

  std::vector<Insight> get_recent_insights_in_window(const std::string& exclude_id, double window_hours,
                                                     int limit) override {
    auto cutoff = time_util::now_utc() - std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                             std::chrono::duration<double>(window_hours * 3600.0));
    return scan_insights(exec("SELECT " + insight_cols() +
                                  " FROM insights WHERE id != $1 AND deleted_at IS NULL AND created_at >= $2 "
                                  "ORDER BY created_at DESC LIMIT $3",
                              {exclude_id, time_util::rfc3339_utc(cutoff), std::to_string(limit)}));
  }

  std::optional<Insight> get_latest_insight_by_source(const std::string& source,
                                                      const std::string& exclude_id) override {
    auto q = exec("SELECT " + insight_cols() +
                      " FROM insights WHERE source = $1 AND id != $2 AND deleted_at IS NULL "
                      "ORDER BY created_at DESC, seq DESC LIMIT 1",
                  {source, exclude_id});
    if (q.rows() == 0) {
      return std::nullopt;
    }
    return scan_insight(q, 0);
  }

  std::vector<Insight> get_recent_insights_by_source(const std::string& source, const std::string& exclude_id,
                                                     int limit) override {
    return scan_insights(exec("SELECT " + insight_cols() +
                                  " FROM insights WHERE source = $1 AND id != $2 AND deleted_at IS NULL "
                                  "ORDER BY created_at DESC LIMIT $3",
                              {source, exclude_id, std::to_string(limit)}));
  }

  std::vector<Insight> get_all_active_insights() override {
    return scan_insights(
        exec("SELECT " + insight_cols() + " FROM insights WHERE deleted_at IS NULL ORDER BY created_at DESC", {}));
  }

  InsightStats get_stats() override {
    InsightStats s;
    s.total = exec("SELECT COUNT(*) FROM insights WHERE deleted_at IS NULL", {}).as_int(0, 0);
    s.deleted_count = exec("SELECT COUNT(*) FROM insights WHERE deleted_at IS NOT NULL", {}).as_int(0, 0);
    {
      auto q = exec("SELECT category, COUNT(*) FROM insights WHERE deleted_at IS NULL GROUP BY category", {});
      for (int r = 0; r < q.rows(); ++r) {
        s.by_category[q.str(r, 0)] = q.as_int(r, 1);
      }
    }
    s.edge_count = exec("SELECT COUNT(*) FROM edges", {}).as_int(0, 0);
    s.oplog_count = exec("SELECT COUNT(*) FROM oplog", {}).as_int(0, 0);
    {
      auto q = exec("SELECT e AS entity, COUNT(DISTINCT i.id) AS cnt "
                    "FROM insights i, jsonb_array_elements_text(i.entities) e "
                    "WHERE i.deleted_at IS NULL GROUP BY e ORDER BY cnt DESC, entity ASC LIMIT 20",
                    {});
      for (int r = 0; r < q.rows(); ++r) {
        EntityStat es;
        es.entity = q.str(r, 0);
        es.count = q.as_int(r, 1);
        s.top_entities.push_back(std::move(es));
      }
    }
    return s;
  }

  void update_embedding(const std::string& id, const std::vector<float>& v) override {
    exec("UPDATE insights SET embedding = $1::vector, updated_at = $2 WHERE id = $3",
         {vector_literal(v), now_str(), id});
    maybe_build_index(static_cast<int>(v.size()));
  }

  std::vector<float> get_embedding(const std::string& id) override {
    auto q = exec("SELECT embedding::text FROM insights WHERE id = $1 AND deleted_at IS NULL", {id});
    if (q.rows() == 0) {
      throw std::runtime_error("no embedding");
    }
    if (q.is_null(0, 0)) {
      return {};
    }
    return parse_vector_text(q.str(0, 0));
  }

  std::vector<EmbeddedRow> get_all_embeddings() override {
    auto q = exec("SELECT id, embedding::text FROM insights WHERE deleted_at IS NULL AND embedding IS NOT NULL", {});
    std::vector<EmbeddedRow> out;
    for (int r = 0; r < q.rows(); ++r) {
      EmbeddedRow row;
      row.id = q.str(r, 0);
      if (!q.is_null(r, 1)) {
        row.embedding = parse_vector_text(q.str(r, 1));
      }
      if (!row.embedding.empty()) {
        out.push_back(std::move(row));
      }
    }
    return out;
  }

  // Exact over-fetch + rerank (docs §6.3, confirmed default): the HNSW index (or a
  // seq scan below the index threshold) picks k*4 nearest candidates, then we
  // re-score those with our own cosine_similarity for SQLite-identical ranking.
  // Any pgvector error (e.g. a stray mismatched-dimension vector) falls back to a
  // full dimension-safe C++ scan.
  std::vector<ScoredId> nearest_embeddings(std::span<const float> query, int k,
                                           std::optional<float> min_cosine) override {
    if (query.empty() || k <= 0) {
      return {};
    }
    int overfetch = k * 4;
    try {
      auto q = exec("SELECT id, embedding::text FROM insights "
                    "WHERE deleted_at IS NULL AND embedding IS NOT NULL "
                    "ORDER BY embedding <=> $1::vector LIMIT $2",
                    {vector_literal(query), std::to_string(overfetch)});
      std::vector<std::string> ids;
      std::vector<std::vector<float>> embs;
      ids.reserve(q.rows());
      embs.reserve(q.rows());
      for (int r = 0; r < q.rows(); ++r) {
        ids.push_back(q.str(r, 0));
        embs.push_back(parse_vector_text(q.str(r, 1)));
      }
      return rerank(query, ids, embs, k, min_cosine);
    } catch (const std::exception&) {
      auto rows = get_all_embeddings();
      std::vector<std::string> ids;
      std::vector<std::vector<float>> embs;
      ids.reserve(rows.size());
      embs.reserve(rows.size());
      for (auto& r : rows) {
        ids.push_back(std::move(r.id));
        embs.push_back(std::move(r.embedding));
      }
      return rerank(query, ids, embs, k, min_cosine);
    }
  }

  std::tuple<int, int> embedding_stats() override {
    int total = exec("SELECT COUNT(*) FROM insights WHERE deleted_at IS NULL", {}).as_int(0, 0);
    int emb =
        exec("SELECT COUNT(*) FROM insights WHERE deleted_at IS NULL AND embedding IS NOT NULL", {}).as_int(0, 0);
    return {total, emb};
  }

  std::vector<Insight> get_insights_without_embedding(int limit) override {
    int lim = limit > 0 ? limit : 100;
    return scan_insights(exec("SELECT " + insight_cols() +
                                  " FROM insights WHERE deleted_at IS NULL AND embedding IS NULL "
                                  "ORDER BY importance DESC, created_at DESC LIMIT $1",
                              {std::to_string(lim)}));
  }

  void insert_edge(const Edge& e) override {
    nlohmann::json mj(e.metadata);
    exec("INSERT INTO edges (source_id, target_id, edge_type, weight, metadata, created_at) "
         "VALUES ($1,$2,$3,$4,$5::jsonb,$6) "
         "ON CONFLICT (source_id, target_id, edge_type) DO UPDATE SET "
         "weight = EXCLUDED.weight, metadata = EXCLUDED.metadata, created_at = EXCLUDED.created_at",
         {e.source_id, e.target_id, edge_type_str(e.edge_type), fmt_double(e.weight), mj.dump(),
          time_util::rfc3339_utc(e.created_at)});
  }

  std::vector<Edge> get_edges_by_node(const std::string& node_id) override {
    return scan_edges(exec("SELECT " + edge_cols() + " FROM edges WHERE source_id = $1 "
                           "UNION ALL "
                           "SELECT " + edge_cols() + " FROM edges WHERE target_id = $1 AND source_id != $1",
                           {node_id}));
  }

  std::vector<Edge> get_edges_by_node_and_type(const std::string& node_id, EdgeType t) override {
    return scan_edges(exec("SELECT " + edge_cols() + " FROM edges WHERE source_id = $1 AND edge_type = $2 "
                           "UNION ALL "
                           "SELECT " + edge_cols() +
                               " FROM edges WHERE target_id = $1 AND edge_type = $2 AND source_id != $1",
                           {node_id, edge_type_str(t)}));
  }

  std::vector<Edge> get_edges_by_source_and_type(const std::string& source_id, EdgeType t) override {
    return scan_edges(exec("SELECT " + edge_cols() + " FROM edges WHERE source_id = $1 AND edge_type = $2",
                           {source_id, edge_type_str(t)}));
  }

  std::vector<std::string> find_insights_with_entity(const std::string& entity, const std::string& exclude_id,
                                                     int limit) override {
    // JSONB `?` tests membership of a string in a top-level array — one row per
    // matching insight, so no DISTINCT (and thus no ORDER-BY-in-select) needed.
    auto q = exec("SELECT id FROM insights WHERE deleted_at IS NULL AND id != $1 AND entities ? $2 "
                  "ORDER BY created_at DESC LIMIT $3",
                  {exclude_id, entity, std::to_string(limit)});
    std::vector<std::string> out;
    for (int r = 0; r < q.rows(); ++r) {
      out.push_back(q.str(r, 0));
    }
    return out;
  }

  std::unordered_set<std::string> load_known_entities() override {
    auto q = exec("SELECT DISTINCT jsonb_array_elements_text(entities) FROM insights WHERE deleted_at IS NULL", {});
    std::unordered_set<std::string> known;
    for (int r = 0; r < q.rows(); ++r) {
      std::string v = q.str(r, 0);
      if (!v.empty()) {
        known.insert(std::move(v));
      }
    }
    return known;
  }

  std::vector<Edge> get_all_edges() override {
    return scan_edges(exec("SELECT " + edge_cols() + " FROM edges", {}));
  }

  void delete_edge(const std::string& source_id, const std::string& target_id, EdgeType edge_type) override {
    exec("DELETE FROM edges WHERE source_id = $1 AND target_id = $2 AND edge_type = $3",
         {source_id, target_id, edge_type_str(edge_type)});
  }

  void delete_edges_by_node(const std::string& node_id) override {
    exec("DELETE FROM edges WHERE source_id = $1 OR target_id = $1", {node_id});
  }

  std::vector<Insight> get_active_insights_by_source_ordered(const std::string& source) override {
    return scan_insights(exec("SELECT " + insight_cols() +
                                  " FROM insights WHERE source = $1 AND deleted_at IS NULL "
                                  "ORDER BY created_at ASC, seq ASC",
                              {source}));
  }

  void log_op(const std::string& operation, const std::string& insight_id, const std::string& detail) override {
    if (readonly_) {
      return;
    }
    try {
      auto q = exec("INSERT INTO oplog (operation, insight_id, detail, created_at) VALUES ($1,$2,$3,$4) RETURNING id",
                    {operation, insight_id.empty() ? OptStr{} : OptStr{insight_id}, detail, now_str()});
      long inserted_id = std::stol(q.str(0, 0));
      if (inserted_id > kMaxOplogEntries && inserted_id % kOplogTrimInterval == 0) {
        exec("DELETE FROM oplog WHERE id <= (SELECT MAX(id) FROM oplog) - $1", {std::to_string(kMaxOplogEntries)});
      }
    } catch (const std::exception& ex) {
      std::cerr << "warning: oplog: " << ex.what() << "\n";
    }
  }

  std::vector<OplogEntry> get_oplog(int limit) override {
    int lim = limit > 0 ? limit : 20;
    auto q = exec("SELECT id, operation, insight_id, detail, created_at FROM oplog ORDER BY id DESC LIMIT $1",
                  {std::to_string(lim)});
    std::vector<OplogEntry> out;
    for (int r = 0; r < q.rows(); ++r) {
      OplogEntry e;
      e.id = q.as_int(r, 0);
      e.operation = q.str(r, 1);
      if (!q.is_null(r, 2)) {
        e.insight_id = q.str(r, 2);
      }
      e.detail = q.str(r, 3);
      e.created_at = q.str(r, 4);
      out.push_back(std::move(e));
    }
    return out;
  }

private:
  static constexpr int kOplogTrimInterval = 100;

  std::string insight_cols() const {
    return "id, content, category, importance, tags, entities, source, access_count, created_at, updated_at, "
           "deleted_at";
  }
  std::string edge_cols() const { return "source_id, target_id, edge_type, weight, metadata, created_at"; }

  static std::string now_str() { return time_util::rfc3339_utc(time_util::now_utc()); }
  // Round-trip-exact: %.17g preserves a binary64 through TEXT so DB-stored weight
  // and effective_importance match SQLite's REAL rather than truncating to 6dp.
  static std::string fmt_double(double d) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.17g", d);
    return buf;
  }

  Insight scan_insight(const PgResult& q, int row) {
    Insight i;
    i.id = q.str(row, 0);
    i.content = q.str(row, 1);
    i.category = q.str(row, 2);
    i.importance = q.as_int(row, 3);
    i.tags = parse_json_str_array(q.str(row, 4));
    i.entities = parse_json_str_array(q.str(row, 5));
    i.source = q.str(row, 6);
    i.access_count = q.as_int(row, 7);
    i.created_at = time_util::parse_rfc3339(q.str(row, 8));
    i.updated_at = time_util::parse_rfc3339(q.str(row, 9));
    if (!q.is_null(row, 10) && !q.str(row, 10).empty()) {
      i.deleted_at = time_util::parse_rfc3339(q.str(row, 10));
    }
    return i;
  }

  std::vector<Insight> scan_insights(const PgResult& q) {
    std::vector<Insight> out;
    for (int r = 0; r < q.rows(); ++r) {
      out.push_back(scan_insight(q, r));
    }
    return out;
  }

  Edge scan_edge(const PgResult& q, int row) {
    Edge e;
    e.source_id = q.str(row, 0);
    e.target_id = q.str(row, 1);
    auto et = parse_edge_type(q.str(row, 2));
    if (!et) {
      throw std::runtime_error("bad edge type");
    }
    e.edge_type = *et;
    e.weight = q.as_double(row, 3);
    parse_metadata(q.str(row, 4), e.metadata);
    e.created_at = time_util::parse_rfc3339(q.str(row, 5));
    return e;
  }

  std::vector<Edge> scan_edges(const PgResult& q) {
    std::vector<Edge> out;
    for (int r = 0; r < q.rows(); ++r) {
      out.push_back(scan_edge(q, r));
    }
    return out;
  }

  void update_active(const char* sql, const std::string& id) {
    auto q = exec(sql, {now_str(), id});
    if (q.affected() == 0) {
      throw std::runtime_error("insight " + id + " not found or already deleted");
    }
  }

  // Exact top-k over a candidate set using our own cosine, matching SqliteStore.
  static std::vector<ScoredId> rerank(std::span<const float> query, const std::vector<std::string>& ids,
                                      const std::vector<std::vector<float>>& embs, int k,
                                      std::optional<float> min_cosine) {
    std::vector<std::span<const float>> vecs;
    vecs.reserve(embs.size());
    for (const auto& e : embs) {
      vecs.emplace_back(e);
    }
    auto sims = mnemon::cosine_similarity_many(query, vecs);
    float thr = min_cosine.value_or(-1.0F);
    std::vector<ScoredId> out;
    out.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
      if (sims[i] < thr) {
        continue;
      }
      out.push_back({ids[i], sims[i]});
    }
    std::sort(out.begin(), out.end(), [](const ScoredId& a, const ScoredId& b) { return a.cosine > b.cosine; });
    if (static_cast<int>(out.size()) > k) {
      out.resize(static_cast<size_t>(k));
    }
    return out;
  }

  static int index_threshold() {
    if (const char* e = std::getenv("MNEMON_PG_INDEX_THRESHOLD"); e && *e) {
      int v = std::atoi(e);
      if (v > 0) {
        return v;
      }
    }
    return 1000;
  }

  // pgvector stores the pinned dimension directly in atttypmod (-1 = unspecified).
  int column_dim() {
    auto q = exec("SELECT atttypmod FROM pg_attribute WHERE attrelid = 'insights'::regclass AND attname = 'embedding'",
                  {});
    return q.rows() == 0 ? -1 : q.as_int(0, 0);
  }

  // Once enough rows exist, pin the column to vector(dim) and build the HNSW index
  // (docs §6.4). Probed once per process; the ALTER rewrites the table, so it is
  // deliberately lazy and one-time. Below the threshold the seq-scan <=> stays exact.
  void maybe_build_index(int dim) {
    if (readonly_ || dim <= 0 || index_checked_) {
      return;
    }
    index_checked_ = true;
    int n = exec("SELECT COUNT(*) FROM insights WHERE deleted_at IS NULL AND embedding IS NOT NULL", {}).as_int(0, 0);
    if (n < index_threshold() || column_dim() != -1) {
      return;
    }
    try {
      std::string d = std::to_string(dim);
      exec_simple(("ALTER TABLE insights ALTER COLUMN embedding TYPE vector(" + d + ") USING embedding::vector(" + d +
                   ")")
                      .c_str());
      exec_simple("CREATE INDEX IF NOT EXISTS idx_insights_embedding "
                  "ON insights USING hnsw (embedding vector_cosine_ops)");
    } catch (const std::exception& ex) {
      std::cerr << "warning: pgvector index build skipped: " << ex.what() << "\n";
    }
  }

  PgResult exec(const std::string& sql, const Params& params) {
    std::vector<const char*> vals;
    vals.reserve(params.size());
    for (const auto& p : params) {
      vals.push_back(p ? p->c_str() : nullptr);
    }
    PGresult* r = PQexecParams(conn_, sql.c_str(), static_cast<int>(params.size()), nullptr,
                               vals.empty() ? nullptr : vals.data(), nullptr, nullptr, 0);
    auto st = PQresultStatus(r);
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
      std::string msg = PQresultErrorMessage(r);
      PQclear(r);
      throw std::runtime_error("postgres: " + msg);
    }
    return PgResult(r);
  }

  void exec_simple(const char* sql) {
    PGresult* r = PQexec(conn_, sql);
    auto st = PQresultStatus(r);
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
      std::string msg = PQresultErrorMessage(r);
      PQclear(r);
      throw std::runtime_error("postgres: " + msg);
    }
    PQclear(r);
  }

  PGconn* conn_{nullptr};
  std::string path_;
  bool readonly_{false};
  int tx_depth_{0};
  bool index_checked_{false};

public:
  void migrate() {
    exec_simple(R"SQL(
CREATE EXTENSION IF NOT EXISTS vector;
CREATE TABLE IF NOT EXISTS insights (
    seq                   BIGSERIAL,
    id                    TEXT PRIMARY KEY,
    content               TEXT NOT NULL,
    category              TEXT NOT NULL DEFAULT 'general',
    importance            INTEGER NOT NULL DEFAULT 3,
    tags                  JSONB NOT NULL DEFAULT '[]'::jsonb,
    entities              JSONB NOT NULL DEFAULT '[]'::jsonb,
    source                TEXT NOT NULL DEFAULT 'user',
    access_count          INTEGER NOT NULL DEFAULT 0,
    created_at            TEXT NOT NULL,
    updated_at            TEXT NOT NULL,
    deleted_at            TEXT,
    last_accessed_at      TEXT,
    embedding             vector,
    effective_importance  DOUBLE PRECISION NOT NULL DEFAULT 0.5
);
CREATE TABLE IF NOT EXISTS edges (
    source_id   TEXT NOT NULL REFERENCES insights(id) ON DELETE CASCADE,
    target_id   TEXT NOT NULL REFERENCES insights(id) ON DELETE CASCADE,
    edge_type   TEXT NOT NULL CHECK (edge_type IN ('temporal','semantic','causal','entity')),
    weight      DOUBLE PRECISION NOT NULL DEFAULT 1.0,
    metadata    JSONB NOT NULL DEFAULT '{}'::jsonb,
    created_at  TEXT NOT NULL,
    PRIMARY KEY (source_id, target_id, edge_type)
);
CREATE TABLE IF NOT EXISTS oplog (
    id          BIGSERIAL PRIMARY KEY,
    operation   TEXT NOT NULL,
    insight_id  TEXT,
    detail      TEXT NOT NULL DEFAULT '',
    created_at  TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_insights_category   ON insights(category);
CREATE INDEX IF NOT EXISTS idx_insights_importance ON insights(importance);
CREATE INDEX IF NOT EXISTS idx_insights_created    ON insights(created_at);
CREATE INDEX IF NOT EXISTS idx_insights_source     ON insights(source);
CREATE INDEX IF NOT EXISTS idx_insights_active     ON insights(deleted_at) WHERE deleted_at IS NULL;
CREATE INDEX IF NOT EXISTS idx_insights_eff_imp    ON insights(effective_importance);
CREATE INDEX IF NOT EXISTS idx_insights_entities_gin ON insights USING GIN (entities);
CREATE INDEX IF NOT EXISTS idx_edges_source        ON edges(source_id);
CREATE INDEX IF NOT EXISTS idx_edges_target        ON edges(target_id);
CREATE INDEX IF NOT EXISTS idx_edges_type          ON edges(edge_type);
CREATE INDEX IF NOT EXISTS idx_edges_source_type   ON edges(source_id, edge_type);
CREATE INDEX IF NOT EXISTS idx_edges_target_type   ON edges(target_id, edge_type);
CREATE INDEX IF NOT EXISTS idx_oplog_created       ON oplog(created_at);
)SQL");
  }
};

// Prepare a connection whose search_path points at the store's schema.
PGconn* open_conn(const std::string& dsn, const std::string& store, bool readonly, bool create) {
  PGconn* c = connect_or_throw(dsn);
  std::string schema = quote_ident(schema_for(store));
  try {
    auto run = [&](const std::string& sql) {
      PGresult* r = PQexec(c, sql.c_str());
      auto st = PQresultStatus(r);
      bool ok = st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK;
      std::string msg = ok ? "" : PQresultErrorMessage(r);
      PQclear(r);
      if (!ok) {
        throw std::runtime_error("postgres: " + msg);
      }
    };
    if (create) {
      run("CREATE SCHEMA IF NOT EXISTS " + schema);
    }
    run("SET search_path TO " + schema);
    run("SET statement_timeout = '30s'");
    run("SET lock_timeout = '10s'");
    if (readonly) {
      run("SET default_transaction_read_only = on");
    }
  } catch (...) {
    PQfinish(c);
    throw;
  }
  return c;
}

bool schema_present(PGconn* c, const std::string& store) {
  std::string schema = schema_for(store);
  const char* vals[1] = {schema.c_str()};
  PGresult* r = PQexecParams(c, "SELECT 1 FROM information_schema.schemata WHERE schema_name = $1", 1, nullptr, vals,
                             nullptr, nullptr, 0);
  bool present = PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0;
  PQclear(r);
  return present;
}

} // namespace

std::unique_ptr<Store> open_readwrite(const std::string& dsn, const std::string& store) {
  PGconn* c = open_conn(dsn, store, false, true);
  auto s = std::make_unique<PostgresStore>(c, sanitize_dsn(dsn), false);
  s->migrate();
  return s;
}

std::unique_ptr<Store> open_readonly(const std::string& dsn, const std::string& store) {
  PGconn* probe = connect_or_throw(dsn);
  bool present = schema_present(probe, store);
  PQfinish(probe);
  if (!present) {
    throw std::runtime_error("store \"" + store + "\" not found");
  }
  PGconn* c = open_conn(dsn, store, true, false);
  return std::make_unique<PostgresStore>(c, sanitize_dsn(dsn), true);
}

std::vector<std::string> list_stores(const std::string& dsn) {
  PGconn* c = connect_or_throw(dsn);
  PGresult* r = PQexec(c, "SELECT schema_name FROM information_schema.schemata "
                          "WHERE schema_name LIKE 'mnemon\\_%' ORDER BY schema_name");
  std::vector<std::string> out;
  if (PQresultStatus(r) == PGRES_TUPLES_OK) {
    for (int i = 0; i < PQntuples(r); ++i) {
      std::string name = PQgetvalue(r, i, 0);
      out.push_back(name.substr(std::string("mnemon_").size()));
    }
  }
  PQclear(r);
  PQfinish(c);
  return out;
}

bool store_exists(const std::string& dsn, const std::string& store) {
  PGconn* c = connect_or_throw(dsn);
  bool present = schema_present(c, store);
  PQfinish(c);
  return present;
}

void create_store(const std::string& dsn, const std::string& store) {
  // Same path as opening read-write: create the schema and its tables.
  open_readwrite(dsn, store);
}

void remove_store(const std::string& dsn, const std::string& store) {
  PGconn* c = connect_or_throw(dsn);
  std::string sql = "DROP SCHEMA IF EXISTS " + quote_ident(schema_for(store)) + " CASCADE";
  PGresult* r = PQexec(c, sql.c_str());
  auto st = PQresultStatus(r);
  std::string msg = (st == PGRES_COMMAND_OK) ? "" : PQresultErrorMessage(r);
  PQclear(r);
  PQfinish(c);
  if (st != PGRES_COMMAND_OK) {
    throw std::runtime_error("postgres: " + msg);
  }
}

} // namespace mnemon::pg
