#pragma once

#include "store.hpp"
#include "model.hpp"

namespace mnemon::graph_eng {

int create_entity_edges(Store& db, const Insight& insight);

} // namespace mnemon::graph_eng
