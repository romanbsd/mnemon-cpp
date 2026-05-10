// Temporal: per-source backbone (prev↔new) plus time-decayed proximity links inside kTemporalWindowHours.
#include "graph_temporal.hpp"

#include "time_util.hpp"

#include <cmath>
#include <sstream>

namespace mnemon::graph_eng {

static constexpr double kTemporalWindowHours = 24.0;
static constexpr int kMaxProximityEdges = 10;

int create_temporal_edges(Database& db, Insight& insight) {
  auto now = time_util::now_utc();
  int count = 0;
  auto prev = db.get_latest_insight_by_source(insight.source, insight.id);
  if (prev) {
    Edge e1;
    e1.source_id = prev->id;
    e1.target_id = insight.id;
    e1.edge_type = EdgeType::temporal;
    e1.weight = 1.0;
    e1.metadata = {{"sub_type", "backbone"}, {"direction", "precedes"}};
    e1.created_at = now;
    db.insert_edge(e1);
    count++;
    Edge e2;
    e2.source_id = insight.id;
    e2.target_id = prev->id;
    e2.edge_type = EdgeType::temporal;
    e2.weight = 1.0;
    e2.metadata = {{"sub_type", "backbone"}, {"direction", "succeeds"}};
    e2.created_at = now;
    db.insert_edge(e2);
    count++;
  }
  auto recent = db.get_recent_insights_in_window(insight.id, kTemporalWindowHours, kMaxProximityEdges);
  std::string backbone_id = prev ? prev->id : "";
  for (const auto& near : recent) {
    if (near.id == backbone_id) {
      continue;
    }
    double hours_diff = std::abs(std::chrono::duration<double>(insight.created_at - near.created_at).count() / 3600.0);
    double weight = 1.0 / (1.0 + hours_diff);
    std::ostringstream hd;
    hd.setf(std::ios::fixed);
    hd.precision(2);
    hd << hours_diff;
    std::string hd_s = hd.str();
    Edge a;
    a.source_id = insight.id;
    a.target_id = near.id;
    a.edge_type = EdgeType::temporal;
    a.weight = weight;
    a.metadata = {{"sub_type", "proximity"}, {"hours_diff", hd_s}};
    a.created_at = now;
    db.insert_edge(a);
    count++;
    Edge b;
    b.source_id = near.id;
    b.target_id = insight.id;
    b.edge_type = EdgeType::temporal;
    b.weight = weight;
    b.metadata = {{"sub_type", "proximity"}, {"hours_diff", hd_s}};
    b.created_at = now;
    db.insert_edge(b);
    count++;
  }
  return count;
}

} // namespace mnemon::graph_eng
