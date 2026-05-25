// Semantic: cosine on stored embeddings; high-sim auto-links, mid-sim optional "review" edges, symmetric pairs.
#include "graph_semantic.hpp"

#include "time_util.hpp"
#include "tokenize.hpp"
#include "vector_math.hpp"

#include <algorithm>
#include <map>
#include <sstream>

namespace mnemon::graph_eng {

static constexpr double kMinSemanticSimilarity = 0.10;
static constexpr double kReviewSemanticThreshold = 0.40;
static constexpr double kAutoSemanticThreshold = 0.80;
static constexpr int kMaxSemanticCandidates = 5;
static constexpr int kMaxAutoSemanticEdges = 3;

EmbedCache build_embed_cache(Database& db) {
  EmbedCache c;
  for (const auto& row : db.get_all_embedding_blobs()) {
    auto v = mnemon::deserialize_vector_f32(row.embedding);
    if (!v.empty()) {
      mnemon::normalize_vector(v);
      c[row.id] = std::move(v);
    }
  }
  return c;
}

int create_semantic_edges(Database& db, Insight& insight, EmbedCache* cache) {
  EmbedCache owned;
  if (!cache) {
    owned = build_embed_cache(db);
    cache = &owned;
  }
  auto it = cache->find(insight.id);
  if (it == cache->end() || it->second.empty()) {
    return 0;
  }
  const auto& insight_vec = it->second;
  struct Sc {
    std::string id;
    double sim;
  };
  std::vector<Sc> candidates;
  std::vector<std::string> ids;
  std::vector<const std::vector<float>*> vec_refs;
  ids.reserve(cache->size());
  vec_refs.reserve(cache->size());
  for (const auto& [id, other_vec] : *cache) {
    if (id == insight.id) {
      continue;
    }
    ids.push_back(id);
    vec_refs.push_back(&other_vec);
  }
  auto sims = mnemon::cosine_similarity_many_f32(insight_vec, vec_refs);
  for (size_t i = 0; i < ids.size(); ++i) {
    double cos_sim = sims[i];
    if (cos_sim >= kAutoSemanticThreshold) {
      candidates.push_back({ids[i], cos_sim});
    }
  }
  std::sort(candidates.begin(), candidates.end(), [](const Sc& a, const Sc& b) { return a.sim > b.sim; });
  if (static_cast<int>(candidates.size()) > kMaxAutoSemanticEdges) {
    candidates.resize(static_cast<size_t>(kMaxAutoSemanticEdges));
  }
  auto now = time_util::now_utc();
  int count = 0;
  for (const auto& c : candidates) {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(4);
    os << c.sim;
    std::map<std::string, std::string> meta = {{"created_by", "auto"}, {"cosine", os.str()}};
    Edge e1;
    e1.source_id = insight.id;
    e1.target_id = c.id;
    e1.edge_type = EdgeType::semantic;
    e1.weight = c.sim;
    e1.metadata = meta;
    e1.created_at = now;
    db.insert_edge(e1);
    count++;
    Edge e2;
    e2.source_id = c.id;
    e2.target_id = insight.id;
    e2.edge_type = EdgeType::semantic;
    e2.weight = c.sim;
    e2.metadata = meta;
    e2.created_at = now;
    db.insert_edge(e2);
    count++;
  }
  return count;
}

static std::vector<SemanticCandidate> find_by_embedding(Database& db, const Insight& insight, EmbedCache& cache) {
  auto it = cache.find(insight.id);
  if (it == cache.end() || it->second.empty()) {
    return {};
  }
  struct Sc {
    std::string id;
    double sim;
  };
  std::vector<Sc> hits;
  std::vector<std::string> ids;
  std::vector<const std::vector<float>*> vec_refs;
  ids.reserve(cache.size());
  vec_refs.reserve(cache.size());
  for (const auto& [id, vec] : cache) {
    if (id == insight.id) {
      continue;
    }
    ids.push_back(id);
    vec_refs.push_back(&vec);
  }
  auto sims = mnemon::cosine_similarity_many_f32(it->second, vec_refs);
  for (size_t i = 0; i < ids.size(); ++i) {
    double cos_sim = sims[i];
    if (cos_sim >= kReviewSemanticThreshold) {
      hits.push_back({ids[i], cos_sim});
    }
  }
  if (hits.empty()) {
    return {};
  }
  std::sort(hits.begin(), hits.end(), [](const Sc& a, const Sc& b) { return a.sim > b.sim; });
  if (static_cast<int>(hits.size()) > kMaxSemanticCandidates) {
    hits.resize(static_cast<size_t>(kMaxSemanticCandidates));
  }
  std::vector<SemanticCandidate> out;
  for (const auto& h : hits) {
    auto ins = db.get_insight_by_id(h.id);
    if (!ins) {
      continue;
    }
    out.push_back(SemanticCandidate{ins->id, ins->content, ins->category, h.sim, h.sim >= kAutoSemanticThreshold});
  }
  if (out.empty()) {
    return {};
  }
  return out;
}

static std::vector<SemanticCandidate> find_by_token_overlap(Database& db, const Insight& insight) {
  auto all = db.get_all_active_insights();
  const auto insight_tokens = search_engine::tokenize(insight.content);
  struct Sc {
    const Insight* ins;
    double sim;
  };
  std::vector<Sc> cands;
  for (const auto& other : all) {
    if (other.id == insight.id) {
      continue;
    }
    const auto other_tokens = search_engine::tokenize(other.content);
    double sim = search_engine::content_similarity_tokens(insight_tokens, other_tokens);
    if (sim >= kMinSemanticSimilarity) {
      cands.push_back({&other, sim});
    }
  }
  std::sort(cands.begin(), cands.end(), [](const Sc& a, const Sc& b) { return a.sim > b.sim; });
  if (static_cast<int>(cands.size()) > kMaxSemanticCandidates) {
    cands.resize(static_cast<size_t>(kMaxSemanticCandidates));
  }
  std::vector<SemanticCandidate> out;
  for (const auto& c : cands) {
    out.push_back(SemanticCandidate{c.ins->id, c.ins->content, c.ins->category, c.sim, false});
  }
  return out;
}

std::vector<SemanticCandidate> find_semantic_candidates(Database& db, const Insight& insight, EmbedCache* cache) {
  EmbedCache owned;
  if (!cache) {
    owned = build_embed_cache(db);
    cache = &owned;
  }
  auto emb = find_by_embedding(db, insight, *cache);
  if (!emb.empty()) {
    return emb;
  }
  return find_by_token_overlap(db, insight);
}

} // namespace mnemon::graph_eng
