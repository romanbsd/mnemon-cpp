#pragma once

#include <string>
#include <vector>

namespace mnemon {

enum class EmbedApi {
  Ollama,
  LlamaCpp,
};

enum class EmbedTask {
  Document,
  Query,
};

struct OllamaClient {
  std::string endpoint;
  std::string model;
  int dimensions{0};
  EmbedApi api{EmbedApi::Ollama};

  static OllamaClient from_env();
  static OllamaClient from_env_with_model(const std::string& model_override);

  std::string availability_path() const;
  std::string embedding_path() const;
  std::string embedding_request_json(const std::string& text, EmbedTask task = EmbedTask::Document) const;
  std::vector<float> parse_embedding_response(const std::string& body) const;

  bool available() const;
  std::vector<float> embed(const std::string& text, EmbedTask task = EmbedTask::Document) const;
};

} // namespace mnemon
