#pragma once

#include "model.hpp"
#include "tokenize.hpp"

#include <unordered_map>
#include <vector>

namespace mnemon::search_engine {

struct ScoredInsight {
  Insight insight;
  double score{0};
};

std::vector<ScoredInsight> keyword_search(const std::vector<Insight>& insights, std::string_view query, int limit);

/** Fills token_cache[insight_id] = content+tag+entity tokens when non-null */
std::vector<ScoredInsight> keyword_search_cached(const std::vector<Insight>& insights, std::string_view query,
                                                 int limit,
                                                 std::unordered_map<std::string, TokenSet>* token_cache);

TokenSet insight_tokens(const Insight& ins);

} // namespace mnemon::search_engine
