#pragma once

// SQLite access layer: schema, CRUD, retention/GC helpers, edge queries (implementation in db.cpp).
#include "model.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace mnemon {

inline constexpr double kHalfLifeDays = 30.0;
inline constexpr int kMaxInsights = 1000;
inline constexpr int kPruneBatchSize = 10;
inline constexpr int kMaxOplogEntries = 5000;

struct QueryFilter {
  std::string keyword;
  std::string category;
  std::string source;
  int limit{20};
};

/** Single-use prepared statement */
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

class Database {
public:
  static std::unique_ptr<Database> open_readwrite(const std::string& data_dir);
  static std::unique_ptr<Database> open_readonly(const std::string& data_dir);

  ~Database();

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  const std::string& path() const { return path_; }
  bool is_readonly() const noexcept { return readonly_; }

  void in_transaction(std::function<void()> fn);

  void insert_insight(const Insight& i);
  std::optional<Insight> get_insight_by_id(const std::string& id);
  std::optional<Insight> get_insight_by_id_include_deleted(const std::string& id);
  std::vector<Insight> query_insights(const QueryFilter& f);
  void soft_delete_insight(const std::string& id);
  void update_entities(const std::string& id, const std::vector<std::string>& entities);
  void increment_access_count(const std::string& id);

  double compute_effective_importance(int importance, int access_count, double days_since_access,
                                      int edge_count);
  bool is_immune(int importance, int access_count);
  std::pair<double, bool> refresh_effective_importance(const std::string& id);

  std::tuple<std::vector<RetentionCandidate>, int> get_retention_candidates(double threshold, int limit);
  int auto_prune(int max_insights, const std::vector<std::string>& exclude_ids);
  void boost_retention(const std::string& id);

  std::vector<Insight> get_recent_insights_in_window(const std::string& exclude_id, double window_hours,
                                                     int limit);
  std::optional<Insight> get_latest_insight_by_source(const std::string& source, const std::string& exclude_id);
  std::vector<Insight> get_recent_insights_by_source(const std::string& source, const std::string& exclude_id,
                                                     int limit);
  std::vector<Insight> get_all_active_insights();
  InsightStats get_stats();

  void update_embedding(const std::string& id, const std::vector<float>& v);
  std::vector<float> get_embedding(const std::string& id);
  std::vector<EmbeddedRow> get_all_embeddings();
  std::tuple<int, int> embedding_stats();
  std::vector<Insight> get_insights_without_embedding(int limit);

  void insert_edge(const Edge& e);
  std::vector<Edge> get_edges_by_node(const std::string& node_id);
  std::vector<Edge> get_edges_by_node_and_type(const std::string& node_id, EdgeType t);
  std::vector<Edge> get_edges_by_source_and_type(const std::string& source_id, EdgeType t);
  std::vector<std::string> find_insights_with_entity(const std::string& entity, const std::string& exclude_id,
                                                     int limit);
  std::unordered_set<std::string> load_known_entities();
  std::vector<Edge> get_all_edges();

  void delete_edge(const std::string& source_id, const std::string& target_id, EdgeType edge_type);
  void delete_edges_by_node(const std::string& node_id);
  std::vector<Insight> get_active_insights_by_source_ordered(const std::string& source);

  void log_op(const std::string& operation, const std::string& insight_id, const std::string& detail);
  std::vector<OplogEntry> get_oplog(int limit);

private:
  explicit Database(sqlite3* h, std::string path, bool readonly);

  void migrate();
  void migrate_remove_narrative_edges();
  void exec_sql(const char* sql);
  Insight scan_insight_row(Statement& st);
  Edge scan_edge_row(Statement& st);

  sqlite3* db_{nullptr};
  std::string path_;
  bool readonly_{false};
  int tx_depth_{0};
};

} // namespace mnemon
