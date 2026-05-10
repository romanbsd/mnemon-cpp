#pragma once

#include <cstdint>
#include <vector>

namespace mnemon {

double cosine_similarity(const std::vector<double>& a, const std::vector<double>& b);
std::vector<uint8_t> serialize_vector(const std::vector<double>& v);
std::vector<double> deserialize_vector(const std::vector<uint8_t>& blob);

} // namespace mnemon
