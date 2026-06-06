#pragma once

#include "model.hpp"

#include <string>
#include <unordered_set>
#include <vector>

namespace mnemon::graph_eng {

bool valid_entity_mode(const std::string& mode);

std::vector<std::string> extract_entities(std::string_view text);
std::vector<std::string> merge_entities(const std::vector<std::string>& provided,
                                        const std::vector<std::string>& extracted);

// Resolve final entity list based on mode: "merge" (default), "provided", or "auto".
std::vector<std::string> resolve_entities(std::string_view content,
                                          const std::vector<std::string>& provided,
                                          const std::string& mode);

// Index-aware variants: knownEntities acts as a filter for the fourth extraction
// path (wide-cast capitalized tokens + tokenized words). Passing an empty set is
// equivalent to calling the non-indexed variants.
std::vector<std::string> extract_entities_indexed(std::string_view text,
                                                   const std::unordered_set<std::string>& known_entities);
std::vector<std::string> resolve_entities_indexed(std::string_view content,
                                                   const std::vector<std::string>& provided,
                                                   const std::string& mode,
                                                   const std::unordered_set<std::string>& known_entities);

} // namespace mnemon::graph_eng
