#include "intent.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>

namespace mnemon::search_engine {

static int count_matches(const std::regex& re, const std::string& s) {
  int n = 0;
  auto begin = std::sregex_iterator(s.begin(), s.end(), re);
  auto end = std::sregex_iterator();
  return static_cast<int>(std::distance(begin, end));
}

std::string intent_str(Intent i) {
  switch (i) {
  case Intent::Why:
    return "WHY";
  case Intent::When:
    return "WHEN";
  case Intent::Entity:
    return "ENTITY";
  case Intent::General:
    return "GENERAL";
  }
  return "GENERAL";
}

std::optional<Intent> intent_from_string(std::string_view sv) {
  std::string s(sv);
  for (auto& c : s) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  if (s == "WHY") {
    return Intent::Why;
  }
  if (s == "WHEN") {
    return Intent::When;
  }
  if (s == "ENTITY") {
    return Intent::Entity;
  }
  if (s == "GENERAL") {
    return Intent::General;
  }
  return std::nullopt;
}

// Heuristic keyword + bilingual regex counts; tie-break order WHY > WHEN > ENTITY > GENERAL.
// Uses std::regex::icase (not inline (?i)) — libc++ is not PCRE-compatible for inline flags.
Intent detect_intent(std::string_view query) {
  std::string q(query);
  for (auto& c : q) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  static const std::regex why_re(
      R"(\b(why|reason|because|cause|motivation|rationale)\b|(为什么|原因|理由))",
      std::regex::ECMAScript | std::regex::icase);
  static const std::regex when_re(
      R"(\b(when|time|date|before|after|during|timeline|history|sequence)\b|(什么时候|何时|时间|之前|之后))",
      std::regex::ECMAScript | std::regex::icase);
  static const std::regex ent_re(
      R"(\b(what is|who is|tell me about|describe|about)\b|(是什么|谁是|关于|介绍))",
      std::regex::ECMAScript | std::regex::icase);
  int why_score = count_matches(why_re, q);
  int when_score = count_matches(when_re, q);
  int entity_score = count_matches(ent_re, q);
  if (why_score > when_score && why_score > entity_score && why_score > 0) {
    return Intent::Why;
  }
  if (when_score > why_score && when_score > entity_score && when_score > 0) {
    return Intent::When;
  }
  if (entity_score > 0) {
    return Intent::Entity;
  }
  return Intent::General;
}

// Per-intent reranking prior over MAGMA edge types (sums need not be 1; recall normalizes downstream).
IntentWeights get_weights(Intent intent) {
  switch (intent) {
  case Intent::Why:
    return {{EdgeType::causal, 0.70}, {EdgeType::temporal, 0.20}, {EdgeType::entity, 0.05}, {EdgeType::semantic, 0.05}};
  case Intent::When:
    return {{EdgeType::temporal, 0.65}, {EdgeType::causal, 0.15}, {EdgeType::entity, 0.10}, {EdgeType::semantic, 0.10}};
  case Intent::Entity:
    return {{EdgeType::entity, 0.55}, {EdgeType::semantic, 0.30}, {EdgeType::temporal, 0.05}, {EdgeType::causal, 0.10}};
  case Intent::General:
    return {{EdgeType::temporal, 0.25}, {EdgeType::semantic, 0.25}, {EdgeType::causal, 0.25}, {EdgeType::entity, 0.25}};
  }
  return get_weights(Intent::General);
}

} // namespace mnemon::search_engine
