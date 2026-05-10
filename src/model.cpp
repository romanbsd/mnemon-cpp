#include "model.hpp"

#include <stdexcept>

namespace mnemon {

std::string edge_type_str(EdgeType t) {
  switch (t) {
  case EdgeType::temporal:
    return "temporal";
  case EdgeType::semantic:
    return "semantic";
  case EdgeType::causal:
    return "causal";
  case EdgeType::entity:
    return "entity";
  }
  return "temporal";
}

std::optional<EdgeType> parse_edge_type(std::string_view s) {
  if (s == "temporal") {
    return EdgeType::temporal;
  }
  if (s == "semantic") {
    return EdgeType::semantic;
  }
  if (s == "causal") {
    return EdgeType::causal;
  }
  if (s == "entity") {
    return EdgeType::entity;
  }
  return std::nullopt;
}

} // namespace mnemon
