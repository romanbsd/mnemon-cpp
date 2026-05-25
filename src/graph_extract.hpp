#pragma once

#include "model.hpp"

#include <string>
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

} // namespace mnemon::graph_eng
