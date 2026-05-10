#pragma once

#include "model.hpp"

#include <string>

namespace mnemon::time_util {

std::string rfc3339_utc(TimePoint tp);
TimePoint parse_rfc3339(const std::string& s);
TimePoint now_utc();

} // namespace mnemon::time_util
