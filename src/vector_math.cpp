#include "vector_math.hpp"

#include <bit>
#include <cmath>
#include <cstring>
#if defined(__APPLE__)
#include <Accelerate/Accelerate.h>
#endif

namespace mnemon {

float cosine_similarity(std::span<const float> a, std::span<const float> b) {
  if (a.size() != b.size() || a.empty()) {
    return 0;
  }
  float dot = 0;
  float na = 0;
  float nb = 0;
#if defined(__APPLE__)
  const auto n = static_cast<vDSP_Length>(a.size());
  vDSP_dotpr(a.data(), 1, b.data(), 1, &dot, n);
  vDSP_dotpr(a.data(), 1, a.data(), 1, &na, n);
  vDSP_dotpr(b.data(), 1, b.data(), 1, &nb, n);
#else
  for (size_t i = 0; i < a.size(); ++i) {
    dot += a[i] * b[i];
    na += a[i] * a[i];
    nb += b[i] * b[i];
  }
#endif
  if (na == 0 || nb == 0) {
    return 0;
  }
  return dot / (std::sqrt(na) * std::sqrt(nb));
}

std::vector<float> cosine_similarity_many(std::span<const float> query,
                                          const std::vector<std::span<const float>>& vectors) {
  std::vector<float> out(vectors.size(), 0.0f);
  if (query.empty() || vectors.empty()) {
    return out;
  }

#if defined(__APPLE__)
  float query_sq = 0;
  const auto n = static_cast<vDSP_Length>(query.size());
  vDSP_dotpr(query.data(), 1, query.data(), 1, &query_sq, n);
  if (query_sq == 0) {
    return out;
  }
  const float query_norm = std::sqrt(query_sq);

  for (size_t i = 0; i < vectors.size(); ++i) {
    const auto v = vectors[i];
    if (v.empty() || v.size() != query.size()) {
      continue;
    }
    float dot = 0;
    float row_sq = 0;
    vDSP_dotpr(query.data(), 1, v.data(), 1, &dot, n);
    vDSP_dotpr(v.data(), 1, v.data(), 1, &row_sq, n);
    if (row_sq == 0) {
      continue;
    }
    out[i] = dot / (query_norm * std::sqrt(row_sq));
  }
  return out;
#else
  for (size_t i = 0; i < vectors.size(); ++i) {
    const auto v = vectors[i];
    if (v.empty()) {
      continue;
    }
    out[i] = cosine_similarity(query, v);
  }
  return out;
#endif
}

// Embeddings are persisted as raw little-endian float32 blobs (4 bytes/dim).
// The runtime representation is already `float`, so storage is a direct
// bit-reinterpretation — no float64 round trip needed.
std::vector<uint8_t> serialize_vector(std::span<const float> v) {
  if (v.empty()) return {};

  std::vector<uint8_t> out(v.size() * sizeof(uint32_t));
  for (size_t i = 0; i < v.size(); ++i) {
    uint32_t bits = std::bit_cast<uint32_t>(v[i]);
    if constexpr (std::endian::native == std::endian::big) {
      bits = std::byteswap(bits);
    }
    std::memcpy(out.data() + i * sizeof(uint32_t), &bits, sizeof(uint32_t));
  }
  return out;
}

std::vector<float> deserialize_vector(const std::vector<uint8_t>& blob) {
  return deserialize_vector(blob.data(), blob.size());
}

std::vector<float> deserialize_vector(const void* data, size_t bytes) {
  if (!data || bytes == 0 || bytes % sizeof(uint32_t) != 0) {
    return {};
  }

  const size_t n = bytes / sizeof(uint32_t);
  std::vector<float> v(n);
  const char* cptr = static_cast<const char*>(data);

  for (size_t i = 0; i < n; ++i) {
    uint32_t bits;
    std::memcpy(&bits, cptr + i * sizeof(uint32_t), sizeof(uint32_t));
    if constexpr (std::endian::native == std::endian::big) {
      bits = std::byteswap(bits);
    }
    v[i] = std::bit_cast<float>(bits);
  }
  return v;
}

void normalize_vector(std::vector<float>& v) {
  if (v.empty()) {
    return;
  }
  float norm_sq = 0;
#if defined(__APPLE__)
  vDSP_dotpr(v.data(), 1, v.data(), 1, &norm_sq, static_cast<vDSP_Length>(v.size()));
#else
  for (float x : v) {
    norm_sq += x * x;
  }
#endif
  if (norm_sq == 0) {
    return;
  }
  const float inv_norm = 1.0f / std::sqrt(norm_sq);
#if defined(__APPLE__)
  vDSP_vsmul(v.data(), 1, &inv_norm, v.data(), 1, static_cast<vDSP_Length>(v.size()));
#else
  for (auto& x : v) {
    x *= inv_norm;
  }
#endif
}

} // namespace mnemon
