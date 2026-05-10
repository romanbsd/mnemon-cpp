#pragma once

#include "model.hpp"

#include <nlohmann/json.hpp>

namespace mnemon {

nlohmann::json insight_to_json(const Insight& i);
nlohmann::json edge_metadata_json(const std::map<std::string, std::string>& m);
void parse_metadata(const std::string& s, std::map<std::string, std::string>& out);

} // namespace mnemon
