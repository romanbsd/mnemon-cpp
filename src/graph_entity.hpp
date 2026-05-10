#pragma once

#include "db.hpp"
#include "model.hpp"

namespace mnemon::graph_eng {

int create_entity_edges(Database& db, Insight& insight);

} // namespace mnemon::graph_eng
