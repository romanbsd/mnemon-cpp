#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mnemon {

float cosine_similarity(std::span<const float> a, std::span<const float> b);
std::vector<float> cosine_similarity_many(std::span<const float> query,
                                          const std::vector<std::span<const float>>& vectors);
std::vector<uint8_t> serialize_vector(std::span<const float> v);
std::vector<float> deserialize_vector(const std::vector<uint8_t>& blob);
std::vector<float> deserialize_vector(const void* data, size_t bytes);
void normalize_vector(std::vector<float>& v);

} // namespace mnemon
