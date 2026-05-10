#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace mnemon::search_engine {

using TokenSet = std::unordered_map<std::string, bool>;

const std::unordered_set<std::string>& stopwords();

bool is_han(uint32_t cp);
TokenSet tokenize(std::string_view text);
double content_similarity(std::string_view a, std::string_view b);

} // namespace mnemon::search_engine
