// Smart recall: RRF anchor fusion (keyword / vector / recency) → weighted beam expansion → multi-signal rerank.
// Intent tweaks traversal width/depth and edge weights; "why" reorders by causal DAG (Kahn + score tie-break).
#include "recall.hpp"

#include "db.hpp"
#include "intent.hpp"
#include "keyword.hpp"
#include "tokenize.hpp"
#include "vector_math.hpp"

#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace mnemon::search_engine {

namespace {

// Appendix B / Go parity: RRF k, anchor pool size, beam lambdas, rerank weights (with vs without embeddings).
constexpr int kAnchorTopK = 20;
constexpr double kLambda1 = 1.0;
constexpr double kLambda2 = 0.4;
constexpr int kRrfK = 60;

constexpr double kRerankKwE = 0.30;
constexpr double kRerankEntE = 0.15;
constexpr double kRerankSimE = 0.35;
constexpr double kRerankGrE = 0.20;
constexpr double kRerankKwN = 0.45;
constexpr double kRerankEntN = 0.25;
constexpr double kRerankGrN = 0.30;

struct Trav {
  int beam{10};
  int depth{4};
  int visited{500};
};

// Wider/deeper graph walk for "why"/"when" queries where explanation chains matter more.
Trav trav_for(Intent i) {
  switch (i) {
  case Intent::Why:
    return {15, 5, 500};
  case Intent::When:
    return {10, 5, 400};
  case Intent::Entity:
    return {10, 4, 400};
  case Intent::General:
    return {10, 4, 500};
  }
  return {10, 4, 500};
}

struct VecHit {
  std::string id;
  double similarity;
};

struct BeamItem {
  std::string id;
  double score{0};
  int depth{0};
};

struct KahnItem {
  std::string id;
  double score{0};
};

struct Anchor {
  Insight insight;
  double score{0};
  std::string via;
};

std::vector<VecHit> vector_search_from_cache(const std::unordered_map<std::string, std::vector<double>>& cache,
                                             const std::vector<double>& query_vec, int limit) {
  if (query_vec.empty() || limit <= 0) {
    return {};
  }
  std::vector<VecHit> hits;
  std::vector<std::string> ids;
  std::vector<const std::vector<double>*> vec_refs;
  ids.reserve(cache.size());
  vec_refs.reserve(cache.size());
  for (const auto& [id, vec] : cache) {
    ids.push_back(id);
    vec_refs.push_back(&vec);
  }

  auto sims = mnemon::cosine_similarity_many(query_vec, vec_refs);
  for (size_t i = 0; i < ids.size(); ++i) {
    double sim = sims[i];
    if (sim <= 0.1) { // same cutoff as semantic edge builder noise floor
      continue;
    }
    hits.push_back({ids[i], sim});
  }
  std::sort(hits.begin(), hits.end(), [](const VecHit& a, const VecHit& b) { return a.similarity > b.similarity; });
  if (static_cast<int>(hits.size()) > limit) {
    hits.resize(static_cast<size_t>(limit));
  }
  return hits;
}

// Layered beam: expand neighbors by structural edge weight + optional cosine to query; keep top `beam` per depth.
void beam_search_from_anchor(Database& db, const std::string& start_id, double start_score,
                             const std::vector<double>& query_vec, const IntentWeights& weights, const Trav& params,
                             std::unordered_map<std::string, double>& score_map,
                             std::unordered_map<std::string, std::string>& via_map,
                             std::unordered_map<std::string, Insight>& insight_map,
                             const std::unordered_map<std::string, std::vector<double>>* embed_cache) {
  std::unordered_set<std::string> visited;
  visited.insert(start_id);
  int total_visited = 1;

  auto cmp = [](const BeamItem& a, const BeamItem& b) { return a.score < b.score; };
  std::priority_queue<BeamItem, std::vector<BeamItem>, decltype(cmp)> current(cmp);
  current.push({start_id, start_score, 0});

  for (int depth = 0; depth < params.depth; ++depth) {
    if (current.empty() || total_visited >= params.visited) {
      break;
    }
    std::priority_queue<BeamItem, std::vector<BeamItem>, decltype(cmp)> next(cmp);
    while (!current.empty() && total_visited < params.visited) {
      BeamItem cur = current.top();
      current.pop();
      if (cur.depth != depth) {
        current.push(cur);
        break;
      }
      auto edges = db.get_edges_by_node(cur.id);
      for (const auto& e : edges) {
        if (total_visited >= params.visited) {
          break;
        }
        std::string neighbor = e.target_id;
        if (neighbor == cur.id) {
          neighbor = e.source_id;
        }
        double structural = 0;
        auto wit = weights.find(e.edge_type);
        if (wit != weights.end()) {
          structural = wit->second * e.weight;
        }
        double semantic = 0;
        if (!query_vec.empty() && embed_cache) {
          auto nit = embed_cache->find(neighbor);
          if (nit != embed_cache->end()) {
            double cos_sim = mnemon::cosine_similarity(query_vec, nit->second);
            if (cos_sim > 0) {
              semantic = cos_sim;
            }
          }
        }
        double neighbor_score = cur.score + kLambda1 * structural + kLambda2 * semantic;
        auto sit = score_map.find(neighbor);
        if (sit == score_map.end() || neighbor_score > sit->second) {
          score_map[neighbor] = neighbor_score;
          via_map[neighbor] = edge_type_str(e.edge_type);
          if (!insight_map.count(neighbor)) {
            auto ins = db.get_insight_by_id(neighbor);
            if (ins) {
              insight_map[neighbor] = *ins;
            }
          }
        }
        if (!visited.count(neighbor)) {
          visited.insert(neighbor);
          total_visited++;
          next.push({neighbor, neighbor_score, depth + 1});
        }
      }
    }
    std::priority_queue<BeamItem, std::vector<BeamItem>, decltype(cmp)> pruned(cmp);
    int kept = 0;
    while (!next.empty() && kept < params.beam) {
      pruned.push(next.top());
      next.pop();
      kept++;
    }
    current = std::move(pruned);
  }
}

// Order results along causal edges among the candidate set; max-heap tie-break preserves high rerank scores.
std::vector<RecallResult> causal_topological_sort(Database& db, std::vector<RecallResult> results) {
  if (results.size() <= 1) {
    return results;
  }
  std::unordered_set<std::string> id_set;
  std::unordered_map<std::string, RecallResult> id_res;
  for (const auto& r : results) {
    id_set.insert(r.insight.id);
    id_res[r.insight.id] = r;
  }
  std::unordered_map<std::string, std::vector<std::string>> adj;
  std::unordered_map<std::string, int> indeg;
  for (const auto& r : results) {
    indeg[r.insight.id] = 0;
  }
  for (const auto& r : results) {
    auto edges = db.get_edges_by_source_and_type(r.insight.id, EdgeType::causal);
    for (const auto& e : edges) {
      if (id_set.count(e.target_id)) {
        adj[e.source_id].push_back(e.target_id);
        indeg[e.target_id]++;
      }
    }
  }
  auto cmp = [](const KahnItem& a, const KahnItem& b) { return a.score < b.score; };
  std::priority_queue<KahnItem, std::vector<KahnItem>, decltype(cmp)> pq(cmp);
  for (const auto& r : results) {
    if (indeg[r.insight.id] == 0) {
      pq.push({r.insight.id, r.score});
    }
  }
  std::vector<RecallResult> ordered;
  while (!pq.empty()) {
    auto item = pq.top();
    pq.pop();
    ordered.push_back(id_res[item.id]);
    for (const auto& tgt : adj[item.id]) {
      indeg[tgt]--;
      if (indeg[tgt] == 0) {
        pq.push({tgt, id_res[tgt].score});
      }
    }
  }
  if (ordered.size() < results.size()) {
    std::unordered_set<std::string> covered;
    for (const auto& r : ordered) {
      covered.insert(r.insight.id);
    }
    for (const auto& r : results) {
      if (!covered.count(r.insight.id)) {
        ordered.push_back(r);
      }
    }
  }
  return ordered;
}

} // namespace

// Fused anchors (RRF + L2-normalize) seed the graph; each anchor runs beam_search. Rerank mixes kw/entity/sim/graph.
RecallResponse intent_aware_recall(Database& db, std::string_view query, const std::vector<double>& query_vec,
                                 const std::vector<std::string>& query_entities, int limit,
                                 std::optional<Intent> intent_override) {
  Intent intent = Intent::General;
  std::string intent_source = "auto";
  if (intent_override) {
    intent = *intent_override;
    intent_source = "override";
  } else {
    intent = detect_intent(query);
  }
  IntentWeights weights = get_weights(intent);
  Trav params = trav_for(intent);

  auto all = db.get_all_active_insights();
  std::unordered_map<std::string, std::vector<double>> embed_cache;
  bool has_embeddings = false;
  if (!query_vec.empty()) {
    for (const auto& row : db.get_all_embeddings()) {
      auto v = mnemon::deserialize_vector(row.embedding);
      if (!v.empty()) {
        embed_cache[row.id] = std::move(v);
      }
    }
    has_embeddings = !embed_cache.empty();
  }

  std::unordered_map<std::string, Anchor> anchor_map;
  std::unordered_map<std::string, TokenSet> token_cache;

  auto kw_anchors = keyword_search_cached(all, query, kAnchorTopK, &token_cache);
  for (size_t rank = 0; rank < kw_anchors.size(); ++rank) {
    const auto& a = kw_anchors[rank];
    anchor_map[a.insight.id] = Anchor{a.insight, 1.0 / (kRrfK + static_cast<int>(rank) + 1), "keyword"};
  }

  if (has_embeddings) {
    auto vec_hits = vector_search_from_cache(embed_cache, query_vec, kAnchorTopK);
    for (size_t rank = 0; rank < vec_hits.size(); ++rank) {
      double rrf = 1.0 / (kRrfK + static_cast<int>(rank) + 1);
      auto it = anchor_map.find(vec_hits[rank].id);
      if (it != anchor_map.end()) {
        it->second.score += rrf;
        if (it->second.via == "keyword" || it->second.via == "vector") {
          it->second.via = "hybrid";
        }
      } else {
        auto ins = db.get_insight_by_id(vec_hits[rank].id);
        if (ins) {
          anchor_map[vec_hits[rank].id] = Anchor{*ins, rrf, "vector"};
        }
      }
    }
  }

  std::vector<Insight> time_sorted = all;
  std::sort(time_sorted.begin(), time_sorted.end(), [](const Insight& a, const Insight& b) {
    return a.created_at > b.created_at;
  });
  int time_limit = std::min(kAnchorTopK, static_cast<int>(time_sorted.size()));
  for (int rank = 0; rank < time_limit; ++rank) {
    const auto& ins = time_sorted[static_cast<size_t>(rank)];
    double rrf = 1.0 / (kRrfK + rank + 1);
    auto it = anchor_map.find(ins.id);
    if (it != anchor_map.end()) {
      it->second.score += rrf;
      if (it->second.via == "keyword" || it->second.via == "vector") {
        it->second.via = "hybrid";
      }
    } else {
      anchor_map[ins.id] = Anchor{ins, rrf, "time"};
    }
  }

  double max_anchor = 0;
  for (const auto& [_, a] : anchor_map) {
    max_anchor = std::max(max_anchor, a.score);
  }
  if (max_anchor > 0) {
    for (auto& [_, a] : anchor_map) {
      a.score /= max_anchor;
    }
  }
  int anchor_count = static_cast<int>(anchor_map.size());

  std::unordered_map<std::string, double> score_map;
  std::unordered_map<std::string, std::string> via_map;
  std::unordered_map<std::string, Insight> insight_map;
  for (const auto& [id, a] : anchor_map) {
    score_map[id] = a.score;
    via_map[id] = a.via;
    insight_map[id] = a.insight;
  }

  const std::unordered_map<std::string, std::vector<double>>* ec_ptr =
      has_embeddings ? &embed_cache : nullptr;
  for (const auto& [id, a] : anchor_map) {
    beam_search_from_anchor(db, id, a.score, query_vec, weights, params, score_map, via_map, insight_map, ec_ptr);
  }

  int traversed_count = static_cast<int>(score_map.size());

  TokenSet query_tokens = tokenize(query);
  std::unordered_set<std::string> query_entity_lower;
  for (const auto& e : query_entities) {
    std::string el = e;
    for (auto& c : el) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    query_entity_lower.insert(std::move(el));
  }

  struct Cand {
    std::string id;
    Insight ins;
    std::string via;
    double graph_raw{0};
    double kw{0};
    double ent{0};
    double sim{0};
    double graph_n{0};
  };
  std::vector<Cand> cands;
  double graph_min = 0, graph_max = 0;
  bool first = true;
  for (const auto& [id, g] : score_map) {
    auto it = insight_map.find(id);
    if (it == insight_map.end()) {
      continue;
    }
    if (first) {
      graph_min = graph_max = g;
      first = false;
    } else {
      graph_min = std::min(graph_min, g);
      graph_max = std::max(graph_max, g);
    }
    cands.push_back(Cand{id, it->second, via_map[id], g, 0, 0, 0, 0});
  }
  double graph_range = graph_max - graph_min;
  if (graph_range == 0) {
    graph_range = 1.0;
  }

  for (auto& c : cands) {
    if (!query_tokens.empty()) {
      TokenSet content_tokens;
      auto tit = token_cache.find(c.id);
      if (tit != token_cache.end()) {
        content_tokens = tit->second;
      } else {
        content_tokens = insight_tokens(c.ins);
      }
      int inter = 0;
      for (const auto& t : query_tokens) {
        if (content_tokens.count(t)) {
          inter++;
        }
      }
      c.kw = static_cast<double>(inter) / static_cast<double>(query_tokens.size());
    }
    if (!query_entity_lower.empty()) {
      int matched = 0;
      for (const auto& ent : c.ins.entities) {
        std::string el = ent;
        for (auto& ch : el) {
          ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (query_entity_lower.count(el)) {
          matched++;
        }
      }
      c.ent = static_cast<double>(matched) / static_cast<double>(std::max<size_t>(query_entity_lower.size(), 1));
    }
    if (has_embeddings) {
      auto vit = embed_cache.find(c.id);
      if (vit != embed_cache.end()) {
        double sim = mnemon::cosine_similarity(query_vec, vit->second);
        if (sim > 0) {
          c.sim = sim;
        }
      }
    }
    c.graph_n = (c.graph_raw - graph_min) / graph_range;
  }

  // Without Ollama vectors, redistribute weight to keyword/entity/graph (Go behavior).
  double w_kw = kRerankKwE, w_ent = kRerankEntE, w_sim = kRerankSimE, w_gr = kRerankGrE;
  if (!has_embeddings) {
    w_kw = kRerankKwN;
    w_ent = kRerankEntN;
    w_sim = 0;
    w_gr = kRerankGrN;
  }

  std::vector<RecallResult> results;
  for (const auto& c : cands) {
    double final_score = w_kw * c.kw + w_ent * c.ent + w_sim * c.sim + w_gr * c.graph_n;
    RecallResult rr;
    rr.insight = c.ins;
    rr.score = final_score;
    rr.intent = intent;
    rr.via = c.via;
    rr.signals.keyword = c.kw;
    rr.signals.entity = c.ent;
    rr.signals.similarity = c.sim;
    rr.signals.graph = c.graph_n;
    results.push_back(std::move(rr));
  }
  std::sort(results.begin(), results.end(), [](const RecallResult& a, const RecallResult& b) {
    if (a.score != b.score) {
      return a.score > b.score;
    }
    return a.insight.importance > b.insight.importance;
  });
  if (limit > 0 && static_cast<int>(results.size()) > limit) {
    results.resize(static_cast<size_t>(limit));
  }

  if (intent == Intent::Why) {
    results = causal_topological_sort(db, std::move(results));
  }

  std::string hint;
  if (results.empty() || (limit > 0 && static_cast<int>(results.size()) < limit / 2)) {
    hint = "sparse_results";
  }

  RecallMeta meta;
  meta.intent = intent;
  meta.intent_source = intent_source;
  meta.anchor_count = anchor_count;
  meta.traversed = traversed_count;
  meta.hint = hint;
  return RecallResponse{std::move(results), std::move(meta)};
}

} // namespace mnemon::search_engine
