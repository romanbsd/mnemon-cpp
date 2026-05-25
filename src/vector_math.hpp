#pragma once

#include <cstdint>
#include <vector>

namespace mnemon {

double cosine_similarity(const std::vector<double>& a, const std::vector<double>& b);
std::vector<double> cosine_similarity_many(const std::vector<double>& query,
                                           const std::vector<const std::vector<double>*>& vectors);
float cosine_similarity_f32(const std::vector<float>& a, const std::vector<float>& b);
std::vector<float> cosine_similarity_many_f32(const std::vector<float>& query,
                                              const std::vector<const std::vector<float>*>& vectors);
std::vector<uint8_t> serialize_vector(const std::vector<double>& v);
std::vector<uint8_t> serialize_vector(const std::vector<float>& v);
std::vector<double> deserialize_vector(const std::vector<uint8_t>& blob);
std::vector<float> deserialize_vector_f32(const std::vector<uint8_t>& blob);
std::vector<float> to_float_vector(const std::vector<double>& v);
void normalize_vector(std::vector<float>& v);

} // namespace mnemon
