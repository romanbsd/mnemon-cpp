#pragma once

#include "store.hpp"
#include "model.hpp"

#include <optional>
#include <vector>

namespace mnemon::graph_eng {

struct BFSNode {
  Insight insight;
  int hop{0};
  Edge via_edge;
};

struct BFSOptions {
  int max_depth{2};
  int max_nodes{0};
  std::optional<EdgeType> edge_filter;
};

std::vector<BFSNode> bfs(Store& db, const std::string& start_id, const BFSOptions& opts);

} // namespace mnemon::graph_eng
