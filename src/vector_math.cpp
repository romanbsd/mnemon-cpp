#include "vector_math.hpp"

#include <cmath>
#include <cstring>

namespace mnemon {

// Standard cosine in R^n; returns 0 for length mismatch or zero vectors (caller treats as “no signal”).
double cosine_similarity(const std::vector<double>& a, const std::vector<double>& b) {
  if (a.size() != b.size() || a.empty()) {
    return 0;
  }
  double dot = 0, na = 0, nb = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    dot += a[i] * b[i];
    na += a[i] * a[i];
    nb += b[i] * b[i];
  }
  if (na == 0 || nb == 0) {
    return 0;
  }
  return dot / (std::sqrt(na) * std::sqrt(nb));
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

} // namespace mnemon
