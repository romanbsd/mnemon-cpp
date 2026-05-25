#include "diff.hpp"

#include "keyword.hpp"
#include "tokenize.hpp"
#include "vector_math.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>

namespace mnemon::search_engine {

// Clear state-change signals. Single common words like "not" are excluded —
// they appear constantly in scientific/research text and cause false CONFLICT.
static const char* kNeg[] = {
    "no longer", "don't", "doesn't", "never", "switched from",
    "instead of", "rather than", "replaced", "deprecated",
    "\xe4\xb8\x8d\xe5\x86\x8d",  // 不再
    "\xe6\x94\xbe\xe5\xbc\x83",  // 放弃
    "\xe6\x9b\xbf\xe6\x8d\xa2",  // 替换
    "\xe5\x8f\x96\xe6\xb6\x88",  // 取消
};

// Thresholds: low sim → ADD; negation → CONFLICT; very high → DUPLICATE; else UPDATE.
static DiffSuggestion classify_suggestion(double similarity, std::string_view new_text, std::string_view old_text) {
  if (similarity < 0.5) {
    return DiffSuggestion::Add;
  }
  // Only check conflict signals when texts are substantially similar.
  // At borderline similarity (0.5–0.7) texts may share domain vocabulary
  // without being about the same subject.
  if (similarity >= 0.7) {
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
  std::vector<const std::vector<double>*> cand_vecs;
  std::vector<double> cand_cos;
  if (!opts.new_embedding.empty()) {
    cand_vecs.reserve(candidates.size());
    for (const auto& c : candidates) {
      auto it = embed_map.find(c.insight.id);
      if (it != embed_map.end() && !it->second.empty()) {
        cand_vecs.push_back(&it->second);
      } else {
        cand_vecs.push_back(nullptr);
      }
    }
    cand_cos = mnemon::cosine_similarity_many(opts.new_embedding, cand_vecs);
  }
  for (size_t idx = 0; idx < candidates.size(); ++idx) {
    const auto& c = candidates[idx];
    double token_sim = jaccard_similarity(new_content, c.insight.content);
    double cosine_sim = 0;
    if (!cand_cos.empty()) {
      cosine_sim = cand_cos[idx];
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
    std::vector<const std::vector<double>*> second_vecs;
    std::vector<std::string> second_ids;
    second_vecs.reserve(opts.existing_embed.size());
    second_ids.reserve(opts.existing_embed.size());
    for (const auto& ei : opts.existing_embed) {
      if (seen[ei.id]) {
        continue;
      }
      second_ids.push_back(ei.id);
      second_vecs.push_back(&ei.embedding);
    }
    auto second_cos = mnemon::cosine_similarity_many(opts.new_embedding, second_vecs);
    for (size_t i = 0; i < second_ids.size(); ++i) {
      double cs = second_cos[i];
      if (cs >= 0.85) {
        top_cos.push_back({second_ids[i], cs});
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
