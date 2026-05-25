#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>

namespace mnemon::search_engine {

using TokenSet = std::unordered_set<std::string>;

const std::unordered_set<std::string>& stopwords();

bool is_han(uint32_t cp);
TokenSet tokenize(std::string_view text);
double content_similarity_tokens(const TokenSet& a, const TokenSet& b);
double content_similarity(std::string_view a, std::string_view b);

// Jaccard similarity: |A∩B| / |A∪B|. Stricter than content_similarity for dedup —
// penalises texts sharing domain vocabulary but differing in specific facts.
double jaccard_similarity(std::string_view a, std::string_view b);

} // namespace mnemon::search_engine
