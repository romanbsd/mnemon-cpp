#pragma once

#include "store.hpp"
#include "model.hpp"

#include <string>
#include <vector>

namespace mnemon::graph_eng {

int create_causal_edges(Store& db, Insight& insight);

struct CausalCandidate {
  std::string id;
  std::string content;
  std::string category;
  int hop{0};
  std::string via_edge;
  std::string causal_signal;
  std::string suggested_sub_type;
};

std::vector<CausalCandidate> find_causal_candidates(Store& db, const Insight& insight);

} // namespace mnemon::graph_eng
