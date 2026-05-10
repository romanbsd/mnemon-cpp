#pragma once

// Public recall types + intent_aware_recall entry (graph/keyword/vector pipeline in recall.cpp).
#include "intent.hpp"
#include "model.hpp"

#include <optional>
#include <string>
#include <vector>

namespace mnemon {

class Database;

namespace search_engine {

struct SignalScores {
  double keyword{0};
  double entity{0};
  double similarity{0};
  double graph{0};
};

struct RecallResult {
  Insight insight;
  double score{0};
  Intent intent{Intent::General};
  std::string via;
  SignalScores signals;
};

struct RecallMeta {
  Intent intent{Intent::General};
  std::string intent_source{"auto"};
  int anchor_count{0};
  int traversed{0};
  std::string hint;
};

struct RecallResponse {
  std::vector<RecallResult> results;
  RecallMeta meta;
};

RecallResponse intent_aware_recall(Database& db, std::string_view query, const std::vector<double>& query_vec,
                                   const std::vector<std::string>& query_entities, int limit,
                                   std::optional<Intent> intent_override);

} // namespace search_engine
} // namespace mnemon
