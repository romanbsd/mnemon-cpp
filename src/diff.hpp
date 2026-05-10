#pragma once

#include "model.hpp"

#include <string>
#include <vector>

namespace mnemon::search_engine {

enum class DiffSuggestion { Add, Duplicate, Conflict, Update };

struct DiffMatch {
  std::string id;
  std::string content;
  double token_similarity{0};
  double cosine_similarity{0};
  double similarity{0};
  DiffSuggestion suggestion{DiffSuggestion::Add};
};

struct DiffResult {
  DiffSuggestion suggestion{DiffSuggestion::Add};
  std::vector<DiffMatch> matches;
};

struct EmbeddedItem {
  std::string id;
  std::vector<double> embedding;
};

struct DiffOptions {
  int limit{5};
  std::vector<double> new_embedding;
  std::vector<EmbeddedItem> existing_embed;
};

DiffResult diff_insights(const std::vector<Insight>& insights, std::string_view new_content, const DiffOptions& opts);

std::string diff_suggestion_str(DiffSuggestion s);

} // namespace mnemon::search_engine
