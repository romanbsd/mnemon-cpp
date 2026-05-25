#include "keyword.hpp"

#include <algorithm>

namespace mnemon::search_engine {

// Union of tokens from body, tags, and entities so keyword recall matches “about this insight” broadly.
TokenSet insight_tokens(const Insight& ins) {
  TokenSet tokens = tokenize(ins.content);
  for (const auto& tag : ins.tags) {
    for (const auto& k : tokenize(tag)) {
      tokens.insert(k);
    }
  }
  for (const auto& ent : ins.entities) {
    for (const auto& k : tokenize(ent)) {
      tokens.insert(k);
    }
  }
  return tokens;
}

// Score = (# query terms hit in insight) / |query terms|; optional cache avoids re-tokenizing insights in recall.
std::vector<ScoredInsight> keyword_search_cached(const std::vector<Insight>& insights, std::string_view query,
                                                 int limit,
                                                 std::unordered_map<std::string, TokenSet>* token_cache) {
  TokenSet query_tokens = tokenize(query);
  if (query_tokens.empty()) {
    return {};
  }
  std::vector<ScoredInsight> all;
  for (const auto& ins : insights) {
    TokenSet content_tokens = insight_tokens(ins);
    if (token_cache) {
      (*token_cache)[ins.id] = content_tokens;
    }
    int inter = 0;
    for (const auto& t : query_tokens) {
      if (content_tokens.count(t)) {
        inter++;
      }
    }
    if (inter == 0) {
      continue;
    }
    double score = static_cast<double>(inter) / static_cast<double>(query_tokens.size());
    all.push_back(ScoredInsight{ins, score});
  }
  std::sort(all.begin(), all.end(), [](const ScoredInsight& a, const ScoredInsight& b) {
    if (a.score != b.score) {
      return a.score > b.score;
    }
    return a.insight.importance > b.insight.importance;
  });
  if (limit > 0 && static_cast<int>(all.size()) > limit) {
    all.resize(static_cast<size_t>(limit));
  }
  return all;
}

std::vector<ScoredInsight> keyword_search(const std::vector<Insight>& insights, std::string_view query, int limit) {
  return keyword_search_cached(insights, query, limit, nullptr);
}

} // namespace mnemon::search_engine
