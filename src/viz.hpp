#pragma once

#include "db.hpp"
#include "model.hpp"

#include <string>

namespace mnemon::viz {

std::string render_dot(const std::vector<Insight>& insights, const std::vector<Edge>& edges);
std::string render_html(const std::vector<Insight>& insights, const std::vector<Edge>& edges);

} // namespace mnemon::viz
