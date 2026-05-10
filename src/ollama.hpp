#pragma once

#include <string>
#include <vector>

namespace mnemon {

struct OllamaClient {
  std::string endpoint;
  std::string model;
  int dimensions{0};

  static OllamaClient from_env();

  bool available() const;
  std::vector<double> embed(const std::string& text) const;
};

} // namespace mnemon
