#pragma once

#include "model.hpp"

#include <vector>

namespace mnemon::graph_eng {

std::vector<std::string> extract_entities(std::string_view text);
std::vector<std::string> merge_entities(const std::vector<std::string>& provided,
                                        const std::vector<std::string>& extracted);

} // namespace mnemon::graph_eng
