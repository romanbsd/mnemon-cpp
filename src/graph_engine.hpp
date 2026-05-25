#pragma once

#include "graph_semantic.hpp"
#include "model.hpp"

#include <string>

namespace mnemon {

class Database;

namespace graph_eng {

struct EdgeStats {
  int temporal{0};
  int entity{0};
  int causal{0};
  int semantic{0};
};

EdgeStats on_insight_created(Database& db, Insight& insight, EmbedCache* embed_cache,
                             const std::string& entity_mode = "merge");

} // namespace graph_eng
} // namespace mnemon
