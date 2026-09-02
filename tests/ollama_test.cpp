#include "ollama.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <optional>
#include <string>

namespace {

class ScopedEnvironment {
 public:
  explicit ScopedEnvironment(const char* name) : name_(name) {
    if (const char* value = std::getenv(name); value != nullptr) {
      original_ = value;
    }
  }

  ~ScopedEnvironment() {
    if (original_) {
      setenv(name_.c_str(), original_->c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
  }

 private:
  std::string name_;
  std::optional<std::string> original_;
};

} // namespace

TEST_CASE("Ollama remains the default embedding API") {
  ScopedEnvironment api_env("MNEMON_EMBED_API");
  unsetenv("MNEMON_EMBED_API");

  const auto client = mnemon::OllamaClient::from_env_with_model("test-model");

  REQUIRE(client.api == mnemon::EmbedApi::Ollama);
  REQUIRE(client.availability_path() == "/api/tags");
  REQUIRE(client.embedding_path() == "/api/embed");

  const auto request = nlohmann::json::parse(client.embedding_request_json("some text"));
  REQUIRE(request == nlohmann::json{{"model", "test-model"}, {"input", "some text"}});
}

TEST_CASE("llama.cpp API uses OpenAI-compatible endpoints and Nomic task prefixes") {
  mnemon::OllamaClient client;
  client.api = mnemon::EmbedApi::LlamaCpp;
  client.model = "nomic-embed-text";
  client.dimensions = 3;

  REQUIRE(client.availability_path() == "/health");
  REQUIRE(client.embedding_path() == "/v1/embeddings");

  const auto document =
      nlohmann::json::parse(client.embedding_request_json("stored fact", mnemon::EmbedTask::Document));
  REQUIRE(document["model"] == "nomic-embed-text");
  REQUIRE(document["input"] == "search_document: stored fact");
  REQUIRE(document["encoding_format"] == "float");
  REQUIRE(document["dimensions"] == 3);

  const auto query = nlohmann::json::parse(client.embedding_request_json("what happened?", mnemon::EmbedTask::Query));
  REQUIRE(query["input"] == "search_query: what happened?");
}

TEST_CASE("embedding responses are parsed for both APIs") {
  mnemon::OllamaClient ollama;
  REQUIRE(ollama.parse_embedding_response(R"({"embeddings":[[0.25,-0.5]]})") ==
          std::vector<float>{0.25F, -0.5F});

  mnemon::OllamaClient llama;
  llama.api = mnemon::EmbedApi::LlamaCpp;
  REQUIRE(llama.parse_embedding_response(R"({"data":[{"embedding":[0.75,0.125],"index":0}]})") ==
          std::vector<float>{0.75F, 0.125F});
}

TEST_CASE("MNEMON_EMBED_API selects llama.cpp and rejects unknown values") {
  ScopedEnvironment api_env("MNEMON_EMBED_API");

  setenv("MNEMON_EMBED_API", "llama.cpp", 1);
  REQUIRE(mnemon::OllamaClient::from_env().api == mnemon::EmbedApi::LlamaCpp);

  setenv("MNEMON_EMBED_API", "other", 1);
  REQUIRE_THROWS(mnemon::OllamaClient::from_env());
}

TEST_CASE("MNEMON_EMBED_DIMENSIONS requires a positive integer") {
  ScopedEnvironment api_env("MNEMON_EMBED_API");
  ScopedEnvironment dimensions_env("MNEMON_EMBED_DIMENSIONS");
  unsetenv("MNEMON_EMBED_API");

  setenv("MNEMON_EMBED_DIMENSIONS", "384", 1);
  REQUIRE(mnemon::OllamaClient::from_env().dimensions == 384);

  for (const char* invalid : {"0", "-1", "abc", "12x", "999999999999999999999"}) {
    setenv("MNEMON_EMBED_DIMENSIONS", invalid, 1);
    REQUIRE_THROWS(mnemon::OllamaClient::from_env());
  }
}

TEST_CASE("OpenAI protocol uses /models and /embeddings without task prefixes") {
  mnemon::OllamaClient client;
  client.api = mnemon::EmbedApi::OpenAI;
  client.model = "BAAI/bge-m3";
  client.dimensions = 1024;

  REQUIRE(client.protocol_string() == "openai");
  REQUIRE(client.availability_path() == "/models");
  REQUIRE(client.embedding_path() == "/embeddings");

  const auto body = nlohmann::json::parse(client.embedding_request_json("跨会话记忆测试", mnemon::EmbedTask::Query));
  REQUIRE(body["model"] == "BAAI/bge-m3");
  REQUIRE(body["input"] == "跨会话记忆测试"); // no search_query prefix
  REQUIRE(body["dimensions"] == 1024);
  REQUIRE_FALSE(body.contains("encoding_format"));

  REQUIRE(client.parse_embedding_response(R"({"data":[{"embedding":[0.1,0.2]}]})") ==
          std::vector<float>{0.1F, 0.2F});
}

TEST_CASE("Ollama protocol reports protocol string ollama") {
  mnemon::OllamaClient client;
  REQUIRE(client.protocol_string() == "ollama");
}

TEST_CASE("MNEMON_EMBED_PROTOCOL and /v1 auto-detect select the OpenAI protocol") {
  ScopedEnvironment api_env("MNEMON_EMBED_API");
  ScopedEnvironment proto_env("MNEMON_EMBED_PROTOCOL");
  ScopedEnvironment endpoint_env("MNEMON_EMBED_ENDPOINT");
  ScopedEnvironment key_env("MNEMON_EMBED_API_KEY");
  unsetenv("MNEMON_EMBED_API");

  // Explicit protocol wins.
  setenv("MNEMON_EMBED_PROTOCOL", "openai", 1);
  unsetenv("MNEMON_EMBED_ENDPOINT");
  REQUIRE(mnemon::OllamaClient::from_env().api == mnemon::EmbedApi::OpenAI);

  // Invalid protocol falls back to auto-detect (no /v1 → ollama).
  setenv("MNEMON_EMBED_PROTOCOL", "bogus", 1);
  REQUIRE(mnemon::OllamaClient::from_env().api == mnemon::EmbedApi::Ollama);

  // Auto-detect: endpoint path ending in /v1 → OpenAI.
  unsetenv("MNEMON_EMBED_PROTOCOL");
  setenv("MNEMON_EMBED_ENDPOINT", "http://localhost:8080/v1", 1);
  REQUIRE(mnemon::OllamaClient::from_env().api == mnemon::EmbedApi::OpenAI);

  // MNEMON_EMBED_API (legacy) takes precedence over auto-detect.
  setenv("MNEMON_EMBED_API", "ollama", 1);
  REQUIRE(mnemon::OllamaClient::from_env().api == mnemon::EmbedApi::Ollama);
  unsetenv("MNEMON_EMBED_API");

  // API key is read from the environment.
  setenv("MNEMON_EMBED_API_KEY", "sk-test", 1);
  REQUIRE(mnemon::OllamaClient::from_env().api_key == "sk-test");
}

TEST_CASE("configured embedding dimensions are validated") {
  mnemon::OllamaClient client;
  client.api = mnemon::EmbedApi::LlamaCpp;
  client.dimensions = 3;

  REQUIRE(client.parse_embedding_response(R"({"data":[{"embedding":[1,2,3,4,5]}]})") ==
          std::vector<float>{1.0F, 2.0F, 3.0F});
  REQUIRE_THROWS(client.parse_embedding_response(R"({"data":[{"embedding":[1,2]}]})"));
}
