#pragma once

#include <cstdint>
#include <vector>

namespace mnemon {

float cosine_similarity(const std::vector<float>& a, const std::vector<float>& b);
std::vector<float> cosine_similarity_many(const std::vector<float>& query,
                                          const std::vector<const std::vector<float>*>& vectors);
std::vector<uint8_t> serialize_vector(const std::vector<float>& v);
std::vector<float> deserialize_vector(const std::vector<uint8_t>& blob);
std::vector<float> deserialize_vector(const void* data, size_t bytes);
std::vector<double> deserialize_vector_double(const std::vector<uint8_t>& blob);
std::vector<double> deserialize_vector_double(const void* data, size_t bytes);
void normalize_vector(std::vector<float>& v);

} // namespace mnemon
