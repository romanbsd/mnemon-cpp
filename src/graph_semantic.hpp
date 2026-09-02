#pragma once

#include "store.hpp"
#include "model.hpp"

#include <unordered_map>
#include <vector>

namespace mnemon::graph_eng {

using EmbedCache = std::unordered_map<std::string, std::vector<float>>;

struct SemanticCandidate {
  std::string id;
  std::string content;
  std::string category;
  double similarity{0};
  bool auto_linked{false};
};

int create_semantic_edges(Store& db, Insight& insight, EmbedCache* cache);
std::vector<SemanticCandidate> find_semantic_candidates(Store& db, const Insight& insight, EmbedCache* cache);

EmbedCache build_embed_cache(Store& db);

} // namespace mnemon::graph_eng
