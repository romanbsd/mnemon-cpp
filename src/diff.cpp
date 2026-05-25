#include "diff.hpp"

#include "keyword.hpp"
#include "tokenize.hpp"
#include "vector_math.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>

namespace mnemon::search_engine {

// Phrases that flip “similar text” into a semantic conflict (English + Chinese), aligned with Go diff heuristics.
static const char* kNeg[] = {"not",           "no longer",     "don't",     "doesn't",    "never",
                             "switched from", "instead of",    "rather than", "replaced", "deprecated",
                             "不",            "没有",          "不再",      "放弃",       "替换",
                             "取消"};

// Thresholds: low sim → ADD; negation → CONFLICT; very high → DUPLICATE; else UPDATE.
static DiffSuggestion classify_suggestion(double similarity, std::string_view new_text, std::string_view old_text) {
  if (similarity < 0.5) {
    return DiffSuggestion::Add;
  }
  std::string nl(new_text);
  std::string ol(old_text);
  for (auto& c : nl) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  for (auto& c : ol) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  for (const char* neg : kNeg) {
    if (nl.find(neg) != std::string::npos || ol.find(neg) != std::string::npos) {
      return DiffSuggestion::Conflict;
    }
  }
  if (similarity > 0.9) {
    return DiffSuggestion::Duplicate;
  }
  return DiffSuggestion::Update;
}

std::string diff_suggestion_str(DiffSuggestion s) {
  switch (s) {
  case DiffSuggestion::Add:
    return "ADD";
  case DiffSuggestion::Duplicate:
    return "DUPLICATE";
  case DiffSuggestion::Conflict:
    return "CONFLICT";
  case DiffSuggestion::Update:
    return "UPDATE";
  }
  return "ADD";
}

// 1) Keyword candidates, blend token vs cosine sim when embeddings exist.
// 2) Second pass: high-cosine pairs missed by keyword search (embedding-only near-dupes).
DiffResult diff_insights(const std::vector<Insight>& insights, std::string_view new_content, const DiffOptions& opts) {
  int lim = opts.limit > 0 ? opts.limit : 5;
  auto candidates = keyword_search(insights, new_content, lim);
  std::unordered_map<std::string, std::vector<double>> embed_map;
  for (const auto& e : opts.existing_embed) {
    embed_map[e.id] = e.embedding;
  }

  std::vector<DiffMatch> matches;
  for (const auto& c : candidates) {
    double token_sim = jaccard_similarity(new_content, c.insight.content);
    double cosine_sim = 0;
    if (!opts.new_embedding.empty()) {
      auto it = embed_map.find(c.insight.id);
      if (it != embed_map.end() && !it->second.empty()) {
        cosine_sim = mnemon::cosine_similarity(opts.new_embedding, it->second);
      }
    }
    double similarity = token_sim;
    // Combined similarity: cosine only contributes when above 0.85.
    // Below that, same-domain content clusters around 0.70–0.84 and produces false UPDATE.
    if (cosine_sim >= 0.85 && cosine_sim > similarity) {
      similarity = cosine_sim;
    }
    auto sug = classify_suggestion(similarity, new_content, c.insight.content);
    matches.push_back(DiffMatch{c.insight.id, c.insight.content, token_sim, cosine_sim, similarity, sug});
  }

  if (!opts.new_embedding.empty() && !opts.existing_embed.empty()) {
    std::unordered_map<std::string, bool> seen;
    for (const auto& m : matches) {
      seen[m.id] = true;
    }
    struct P {
      std::string id;
      double sim;
    };
    std::vector<P> top_cos;
    for (const auto& ei : opts.existing_embed) {
      if (seen[ei.id]) {
        continue;
      }
      double cs = mnemon::cosine_similarity(opts.new_embedding, ei.embedding);
      if (cs >= 0.85) {
        top_cos.push_back({ei.id, cs});
      }
    }
    std::sort(top_cos.begin(), top_cos.end(), [](const P& a, const P& b) { return a.sim > b.sim; });
    if (static_cast<int>(top_cos.size()) > lim) {
      top_cos.resize(static_cast<size_t>(lim));
    }
    std::unordered_map<std::string, Insight> id_to_ins;
    for (const auto& ins : insights) {
      id_to_ins[ins.id] = ins;
    }
    for (const auto& cp : top_cos) {
      auto it = id_to_ins.find(cp.id);
      if (it == id_to_ins.end()) {
        continue;
      }
      double token_sim = jaccard_similarity(new_content, it->second.content);
      double similarity = token_sim;
      if (cp.sim >= 0.85 && cp.sim > similarity) {
        similarity = cp.sim;
      }
      auto sug = classify_suggestion(similarity, new_content, it->second.content);
      if (sug != DiffSuggestion::Add) {
        matches.push_back(
            DiffMatch{it->second.id, it->second.content, token_sim, cp.sim, similarity, sug});
      }
    }
  }

  // Overall suggestion: any DUPLICATE among candidates wins (stop remember-as-new).
  DiffSuggestion overall = DiffSuggestion::Add;
  if (!matches.empty()) {
    overall = matches[0].suggestion;
    for (const auto& m : matches) {
      if (m.suggestion == DiffSuggestion::Duplicate) {
        overall = DiffSuggestion::Duplicate;
        break;
      }
    }
  }
  return DiffResult{overall, std::move(matches)};
}

} // namespace mnemon::search_engine
