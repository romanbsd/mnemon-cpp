#pragma once

// SQLite implementation of the Store interface: schema, CRUD, retention/GC
// helpers, edge queries (implementation in db.cpp).
#include "store.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace mnemon {

/** Single-use prepared statement. SQLite-only; never appears in the Store interface. */
class Statement {
public:
  Statement(sqlite3* db, const char* sql);
  ~Statement();
  Statement(const Statement&) = delete;
  void bind_int(int idx, int v);
  void bind_int64(int idx, int64_t v);
  void bind_double(int idx, double v);
  void bind_text(int idx, const std::string& s);
  void bind_blob(int idx, const void* data, size_t len);
  void bind_null(int idx);
  bool step();
  std::string column_text(int idx);
  int column_int(int idx);
  int64_t column_int64(int idx);
  double column_double(int idx);
  const void* column_blob(int idx);
  int column_bytes(int idx);
  bool column_null(int idx);
  void reset();

private:
  sqlite3_stmt* stmt_{nullptr};
};

class SqliteStore : public Store {
public:
  static std::unique_ptr<SqliteStore> open_readwrite(const std::string& data_dir);
  static std::unique_ptr<SqliteStore> open_readonly(const std::string& data_dir);

  ~SqliteStore() override;

  const std::string& path() const override { return path_; }
  bool is_readonly() const noexcept override { return readonly_; }

  void in_transaction(std::function<void()> fn) override;

  void insert_insight(const Insight& i) override;
  std::optional<Insight> get_insight_by_id(const std::string& id) override;
  std::optional<Insight> get_insight_by_id_include_deleted(const std::string& id) override;
  std::vector<Insight> query_insights(const QueryFilter& f) override;
  void soft_delete_insight(const std::string& id) override;
  void update_entities(const std::string& id, const std::vector<std::string>& entities) override;
  void increment_access_count(const std::string& id) override;

  std::pair<double, bool> refresh_effective_importance(const std::string& id) override;

  std::tuple<std::vector<RetentionCandidate>, int> get_retention_candidates(double threshold, int limit) override;
  int auto_prune(int max_insights, const std::vector<std::string>& exclude_ids) override;
  void boost_retention(const std::string& id) override;

  std::vector<Insight> get_recent_insights_in_window(const std::string& exclude_id, double window_hours,
                                                     int limit) override;
  std::optional<Insight> get_latest_insight_by_source(const std::string& source,
                                                      const std::string& exclude_id) override;
  std::vector<Insight> get_recent_insights_by_source(const std::string& source, const std::string& exclude_id,
                                                     int limit) override;
  std::vector<Insight> get_all_active_insights() override;
  InsightStats get_stats() override;

  void update_embedding(const std::string& id, const std::vector<float>& v) override;
  std::vector<float> get_embedding(const std::string& id) override;
  std::vector<EmbeddedRow> get_all_embeddings() override;
  std::vector<ScoredId> nearest_embeddings(std::span<const float> query, int k,
                                           std::optional<float> min_cosine) override;
  std::tuple<int, int> embedding_stats() override;
  std::vector<Insight> get_insights_without_embedding(int limit) override;

  void insert_edge(const Edge& e) override;
  std::vector<Edge> get_edges_by_node(const std::string& node_id) override;
  std::vector<Edge> get_edges_by_node_and_type(const std::string& node_id, EdgeType t) override;
  std::vector<Edge> get_edges_by_source_and_type(const std::string& source_id, EdgeType t) override;
  std::vector<std::string> find_insights_with_entity(const std::string& entity, const std::string& exclude_id,
                                                     int limit) override;
  std::unordered_set<std::string> load_known_entities() override;
  std::vector<Edge> get_all_edges() override;

  void delete_edge(const std::string& source_id, const std::string& target_id, EdgeType edge_type) override;
  void delete_edges_by_node(const std::string& node_id) override;
  std::vector<Insight> get_active_insights_by_source_ordered(const std::string& source) override;

  void log_op(const std::string& operation, const std::string& insight_id, const std::string& detail) override;
  std::vector<OplogEntry> get_oplog(int limit) override;

private:
  explicit SqliteStore(sqlite3* h, std::string path, bool readonly);

  void migrate();
  void migrate_remove_narrative_edges();
  void migrate_embeddings_to_float32();
  void exec_sql(const char* sql);
  static Insight scan_insight_row(Statement& st);
  static std::vector<Insight> scan_insight_rows(Statement& st);
  static Edge scan_edge_row(Statement& st);
  static std::vector<Edge> scan_edge_rows(Statement& st);

  sqlite3* db_{nullptr};
  std::string path_;
  bool readonly_{false};
  int tx_depth_{0};
};

} // namespace mnemon
