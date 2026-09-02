#pragma once

// Storage interface: the abstract surface every backend implements.
// SqliteStore (db.hpp) is the only implementation today; a Postgres backend
// would be a second one. commands.cpp and the engine layer depend on this
// interface, never on a concrete backend. See docs/postgres-pgvector.md.
#include "model.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

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

class Store {
public:
  // Factory: inspects config and returns the right backend. Today always SQLite.
  static std::unique_ptr<Store> open_readwrite(const std::string& data_dir);
  static std::unique_ptr<Store> open_readonly(const std::string& data_dir);

  virtual ~Store() = default;

  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  // Stateless retention math — same for every backend, so not virtual.
  static double compute_effective_importance(int importance, int access_count, double days_since_access,
                                             int edge_count);
  static bool is_immune(int importance, int access_count);

  virtual const std::string& path() const = 0;
  virtual bool is_readonly() const noexcept = 0;

  virtual void in_transaction(std::function<void()> fn) = 0;

  virtual void insert_insight(const Insight& i) = 0;
  virtual std::optional<Insight> get_insight_by_id(const std::string& id) = 0;
  virtual std::optional<Insight> get_insight_by_id_include_deleted(const std::string& id) = 0;
  virtual std::vector<Insight> query_insights(const QueryFilter& f) = 0;
  virtual void soft_delete_insight(const std::string& id) = 0;
  virtual void update_entities(const std::string& id, const std::vector<std::string>& entities) = 0;
  virtual void increment_access_count(const std::string& id) = 0;

  virtual std::pair<double, bool> refresh_effective_importance(const std::string& id) = 0;

  virtual std::tuple<std::vector<RetentionCandidate>, int> get_retention_candidates(double threshold, int limit) = 0;
  virtual int auto_prune(int max_insights, const std::vector<std::string>& exclude_ids) = 0;
  virtual void boost_retention(const std::string& id) = 0;

  virtual std::vector<Insight> get_recent_insights_in_window(const std::string& exclude_id, double window_hours,
                                                             int limit) = 0;
  virtual std::optional<Insight> get_latest_insight_by_source(const std::string& source,
                                                              const std::string& exclude_id) = 0;
  virtual std::vector<Insight> get_recent_insights_by_source(const std::string& source, const std::string& exclude_id,
                                                             int limit) = 0;
  virtual std::vector<Insight> get_all_active_insights() = 0;
  virtual InsightStats get_stats() = 0;

  virtual void update_embedding(const std::string& id, const std::vector<float>& v) = 0;
  virtual std::vector<float> get_embedding(const std::string& id) = 0;
  virtual std::vector<EmbeddedRow> get_all_embeddings() = 0;
  // Top-k nearest by cosine similarity. SQLite ranks in-process; a pgvector
  // backend would override this with an indexed query. See docs/postgres-pgvector.md §6.3.
  virtual std::vector<ScoredId> nearest_embeddings(std::span<const float> query, int k,
                                                   std::optional<float> min_cosine) = 0;
  virtual std::tuple<int, int> embedding_stats() = 0;
  virtual std::vector<Insight> get_insights_without_embedding(int limit) = 0;

  virtual void insert_edge(const Edge& e) = 0;
  virtual std::vector<Edge> get_edges_by_node(const std::string& node_id) = 0;
  virtual std::vector<Edge> get_edges_by_node_and_type(const std::string& node_id, EdgeType t) = 0;
  virtual std::vector<Edge> get_edges_by_source_and_type(const std::string& source_id, EdgeType t) = 0;
  virtual std::vector<std::string> find_insights_with_entity(const std::string& entity, const std::string& exclude_id,
                                                             int limit) = 0;
  virtual std::unordered_set<std::string> load_known_entities() = 0;
  virtual std::vector<Edge> get_all_edges() = 0;

  virtual void delete_edge(const std::string& source_id, const std::string& target_id, EdgeType edge_type) = 0;
  virtual void delete_edges_by_node(const std::string& node_id) = 0;
  virtual std::vector<Insight> get_active_insights_by_source_ordered(const std::string& source) = 0;

  virtual void log_op(const std::string& operation, const std::string& insight_id, const std::string& detail) = 0;
  virtual std::vector<OplogEntry> get_oplog(int limit) = 0;

protected:
  Store() = default;
};

} // namespace mnemon
