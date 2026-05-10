// Entity: bidirectional edges to other insights sharing an entity string (capped per entity and total).
#include "graph_entity.hpp"

#include "time_util.hpp"

namespace mnemon::graph_eng {

static constexpr int kMaxEntityLinks = 5;
static constexpr int kMaxTotalEntityEdges = 50;

int create_entity_edges(Database& db, Insight& insight) {
  if (insight.entities.empty()) {
    return 0;
  }
  auto now = time_util::now_utc();
  int count = 0;
  for (const auto& entity : insight.entities) {
    if (count >= kMaxTotalEntityEdges) {
      break;
    }
    auto ids = db.find_insights_with_entity(entity, insight.id, kMaxEntityLinks);
    for (const auto& target_id : ids) {
      if (count >= kMaxTotalEntityEdges) {
        break;
      }
      Edge e1;
      e1.source_id = insight.id;
      e1.target_id = target_id;
      e1.edge_type = EdgeType::entity;
      e1.weight = 1.0;
      e1.metadata = {{"entity", entity}};
      e1.created_at = now;
      db.insert_edge(e1);
      count++;
      Edge e2;
      e2.source_id = target_id;
      e2.target_id = insight.id;
      e2.edge_type = EdgeType::entity;
      e2.weight = 1.0;
      e2.metadata = {{"entity", entity}};
      e2.created_at = now;
      db.insert_edge(e2);
      count++;
    }
  }
  return count;
}

} // namespace mnemon::graph_eng
