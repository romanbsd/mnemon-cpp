#include "vector_math.hpp"

#include <cmath>
#include <cstring>
#if defined(__APPLE__)
#include <Accelerate/Accelerate.h>
#endif

namespace mnemon {

// Standard cosine in R^n; returns 0 for length mismatch or zero vectors (caller treats as “no signal”).
double cosine_similarity(const std::vector<double>& a, const std::vector<double>& b) {
  if (a.size() != b.size() || a.empty()) {
    return 0;
  }
  double dot = 0;
  double na = 0;
  double nb = 0;
#if defined(__APPLE__)
  const auto n = static_cast<vDSP_Length>(a.size());
  vDSP_dotprD(a.data(), 1, b.data(), 1, &dot, n);
  vDSP_dotprD(a.data(), 1, a.data(), 1, &na, n);
  vDSP_dotprD(b.data(), 1, b.data(), 1, &nb, n);
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

float cosine_similarity_f32(const std::vector<float>& a, const std::vector<float>& b) {
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

std::vector<double> cosine_similarity_many(const std::vector<double>& query,
                                           const std::vector<const std::vector<double>*>& vectors) {
  std::vector<double> out(vectors.size(), 0.0);
  if (query.empty() || vectors.empty()) {
    return out;
  }

#if defined(__APPLE__)
  double query_sq = 0;
  const auto n = static_cast<vDSP_Length>(query.size());
  vDSP_dotprD(query.data(), 1, query.data(), 1, &query_sq, n);
  if (query_sq == 0) {
    return out;
  }
  const double query_norm = std::sqrt(query_sq);

  for (size_t i = 0; i < vectors.size(); ++i) {
    const auto* v = vectors[i];
    if (!v || v->size() != query.size()) {
      continue;
    }
    double dot = 0;
    double row_sq = 0;
    vDSP_dotprD(query.data(), 1, v->data(), 1, &dot, n);
    vDSP_dotprD(v->data(), 1, v->data(), 1, &row_sq, n);
    if (row_sq == 0) {
      continue;
    }
    out[i] = dot / (query_norm * std::sqrt(row_sq));
  }
  return out;
#else
  for (size_t i = 0; i < vectors.size(); ++i) {
    const auto* v = vectors[i];
    if (!v) {
      continue;
    }
    out[i] = cosine_similarity(query, *v);
  }
  return out;
#endif
}

std::vector<float> cosine_similarity_many_f32(const std::vector<float>& query,
                                              const std::vector<const std::vector<float>*>& vectors) {
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
    const auto* v = vectors[i];
    if (!v || v->size() != query.size()) {
      continue;
    }
    float dot = 0;
    float row_sq = 0;
    vDSP_dotpr(query.data(), 1, v->data(), 1, &dot, n);
    vDSP_dotpr(v->data(), 1, v->data(), 1, &row_sq, n);
    if (row_sq == 0) {
      continue;
    }
    out[i] = dot / (query_norm * std::sqrt(row_sq));
  }
  return out;
#else
  for (size_t i = 0; i < vectors.size(); ++i) {
    const auto* v = vectors[i];
    if (!v) {
      continue;
    }
    out[i] = cosine_similarity_f32(query, *v);
  }
  return out;
#endif
}

// Little-endian float64 bytes — must round-trip with Go’s blob layout for DB compatibility.
std::vector<uint8_t> serialize_vector(const std::vector<double>& v) {
  std::vector<uint8_t> out(v.size() * 8);
  for (size_t i = 0; i < v.size(); ++i) {
    uint64_t bits = 0;
    std::memcpy(&bits, &v[i], sizeof(double));
    for (int b = 0; b < 8; ++b) {
      out[i * 8 + b] = static_cast<uint8_t>((bits >> (8 * b)) & 0xFF);
    }
  }
  return out;
}

std::vector<uint8_t> serialize_vector(const std::vector<float>& v) {
  std::vector<uint8_t> out(v.size() * 8);
  for (size_t i = 0; i < v.size(); ++i) {
    double d = static_cast<double>(v[i]);
    uint64_t bits = 0;
    std::memcpy(&bits, &d, sizeof(double));
    for (int b = 0; b < 8; ++b) {
      out[i * 8 + b] = static_cast<uint8_t>((bits >> (8 * b)) & 0xFF);
    }
  }
  return out;
}

std::vector<double> deserialize_vector(const std::vector<uint8_t>& blob) {
  if (blob.empty() || blob.size() % 8 != 0) {
    return {};
  }
  std::vector<double> v(blob.size() / 8);
  for (size_t i = 0; i < v.size(); ++i) {
    uint64_t bits = 0;
    for (int b = 0; b < 8; ++b) {
      bits |= static_cast<uint64_t>(blob[i * 8 + b]) << (8 * b);
    }
    std::memcpy(&v[i], &bits, sizeof(double));
  }
  return v;
}

std::vector<float> deserialize_vector_f32(const std::vector<uint8_t>& blob) {
  if (blob.empty() || blob.size() % 8 != 0) {
    return {};
  }
  std::vector<float> v(blob.size() / 8);
  for (size_t i = 0; i < v.size(); ++i) {
    uint64_t bits = 0;
    for (int b = 0; b < 8; ++b) {
      bits |= static_cast<uint64_t>(blob[i * 8 + b]) << (8 * b);
    }
    double d = 0;
    std::memcpy(&d, &bits, sizeof(double));
    v[i] = static_cast<float>(d);
  }
  return v;
}

std::vector<float> to_float_vector(const std::vector<double>& v) {
  std::vector<float> out;
  out.reserve(v.size());
  for (double d : v) {
    out.push_back(static_cast<float>(d));
  }
  return out;
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
