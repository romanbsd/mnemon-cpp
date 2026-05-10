// Causal edges: regex cue phrases (EN/ZH), token overlap vs recent window, optional sub_type in metadata.
#include "graph_causal.hpp"

#include "graph_bfs.hpp"
#include "time_util.hpp"
#include "tokenize.hpp"

#include <cmath>
#include <regex>
#include <sstream>

namespace mnemon::graph_eng {

static constexpr double kMinCausalOverlap = 0.15;
static constexpr int kCausalLookback = 10;
static constexpr int kMaxCausalCandidates = 10;

static const std::regex kCausalPattern(
    "\\b(because|therefore|due to|caused by|as a result|decided to|chosen because|so that|in order to|leads "
    "to|results in)\\b|(因为|所以|由于|导致|因此|决定|为了|以便)",
    std::regex::ECMAScript | std::regex::icase);

static const std::regex kCausesPattern(R"(\b(because|caused by|due to)\b|(因为|由于))",
                                         std::regex::ECMAScript | std::regex::icase);
static const std::regex kEnablesPattern(R"(\b(so that|in order to|enables|leads to)\b|(为了|以便))",
                                        std::regex::ECMAScript | std::regex::icase);
static const std::regex kPreventsPattern(R"(\b(despite|prevented|prevents|blocked)\b|(阻止|防止))",
                                           std::regex::ECMAScript | std::regex::icase);

bool has_causal_signal(std::string_view text) {
  std::string s(text);
  return std::regex_search(s, kCausalPattern);
}

static double token_overlap_sets(const search_engine::TokenSet& a, const search_engine::TokenSet& b) {
  if (a.empty() || b.empty()) {
    return 0;
  }
  const search_engine::TokenSet* small = &a;
  const search_engine::TokenSet* big = &b;
  if (a.size() > b.size()) {
    small = &b;
    big = &a;
  }
  int inter = 0;
  for (const auto& [k, _] : *small) {
    if (big->count(k)) {
      inter++;
    }
  }
  size_t max_len = std::max(a.size(), b.size());
  return static_cast<double>(inter) / static_cast<double>(max_len);
}

static std::string suggest_sub_type(std::string_view text) {
  std::string s(text);
  if (std::regex_search(s, kPreventsPattern)) {
    return "prevents";
  }
  if (std::regex_search(s, kEnablesPattern)) {
    return "enables";
  }
  if (std::regex_search(s, kCausesPattern)) {
    return "causes";
  }
  return "causes";
}

static std::string find_causal_signal(std::string_view text) {
  std::string s(text);
  std::smatch m;
  if (std::regex_search(s, m, kCausalPattern)) {
    return m.str();
  }
  return "";
}

int create_causal_edges(Database& db, Insight& insight) {
  auto recent = db.get_recent_insights_by_source(insight.source, insight.id, kCausalLookback);
  if (recent.empty()) {
    return 0;
  }
  auto new_tokens = search_engine::tokenize(insight.content);
  if (new_tokens.empty()) {
    return 0;
  }
  bool new_has = has_causal_signal(insight.content);
  auto now = time_util::now_utc();
  int count = 0;
  for (const auto& prev : recent) {
    bool prev_has = has_causal_signal(prev.content);
    if (!new_has && !prev_has) {
      continue;
    }
    auto prev_tokens = search_engine::tokenize(prev.content);
    double overlap = token_overlap_sets(new_tokens, prev_tokens);
    if (overlap < kMinCausalOverlap) {
      continue;
    }
    std::string source_id = prev.id;
    std::string target_id = insight.id;
    if (!new_has && prev_has) {
      source_id = insight.id;
      target_id = prev.id;
    }
    Edge e;
    e.source_id = source_id;
    e.target_id = target_id;
    e.edge_type = EdgeType::causal;
    e.weight = overlap;
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(4);
    os << overlap;
    e.metadata = {{"overlap", os.str()}, {"sub_type", suggest_sub_type(insight.content + " " + prev.content)}};
    e.created_at = now;
    db.insert_edge(e);
    count++;
  }
  return count;
}

std::vector<CausalCandidate> find_causal_candidates(Database& db, const Insight& insight) {
  auto nodes = bfs(db, insight.id, BFSOptions{2, kMaxCausalCandidates, std::nullopt});
  std::vector<CausalCandidate> out;
  for (const auto& n : nodes) {
    std::string combined = insight.content + " " + n.insight.content;
    std::string sig = find_causal_signal(n.insight.content);
    if (sig.empty()) {
      sig = find_causal_signal(insight.content);
    }
    out.push_back(CausalCandidate{n.insight.id, n.insight.content, n.insight.category, n.hop,
                                   edge_type_str(n.via_edge.edge_type), sig, suggest_sub_type(combined)});
  }
  return out;
}

} // namespace mnemon::graph_eng
