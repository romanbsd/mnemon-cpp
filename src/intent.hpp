#pragma once

#include "model.hpp"

#include <optional>
#include <string>
#include <unordered_map>

namespace mnemon::search_engine {

enum class Intent { Why, When, Entity, General };

std::string intent_str(Intent i);
std::optional<Intent> intent_from_string(std::string_view s);
Intent detect_intent(std::string_view query);

using IntentWeights = std::unordered_map<EdgeType, double>;
IntentWeights get_weights(Intent intent);

} // namespace mnemon::search_engine
