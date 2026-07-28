#include "ollama.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace mnemon {

namespace {

// Mirrors Go: http.Client Post(c.endpoint+"/api/embed", …) — any path prefix after host:port is preserved.
// HTTPS requires cpp-httplib built with OpenSSL (CPPHTTPLIB_OPENSSL_SUPPORT); otherwise we fail fast below.
struct EndpointParts {
  std::string host_with_scheme;
  std::string path_prefix;
};

static EndpointParts split_endpoint(std::string ep) {
  while (!ep.empty() && ep.back() == '/') {
    ep.pop_back();
  }
  if (ep.find("://") == std::string::npos) {
    ep = "http://" + ep;
  }
  const size_t scheme_sep = ep.find("://");
  if (scheme_sep == std::string::npos) {
    throw std::runtime_error("invalid MNEMON_EMBED_ENDPOINT");
  }
  const std::string scheme = ep.substr(0, scheme_sep);
  const size_t after_scheme = scheme_sep + 3;
  const size_t path_sep = ep.find('/', after_scheme);
  std::string authority;
  std::string path_prefix;
  if (path_sep == std::string::npos) {
    authority = ep.substr(after_scheme);
  } else {
    authority = ep.substr(after_scheme, path_sep - after_scheme);
    path_prefix = ep.substr(path_sep);
    while (!path_prefix.empty() && path_prefix.back() == '/') {
      path_prefix.pop_back();
    }
  }
  EndpointParts p;
  p.host_with_scheme = scheme + "://" + authority;
  p.path_prefix = path_prefix;
  return p;
}

static std::string api_path(const EndpointParts& p, const std::string& suffix) {
  return p.path_prefix + suffix;
}

static void configure_client(httplib::Client& cli) {
  cli.set_connection_timeout(2, 0);
  cli.set_read_timeout(2, 0);
  cli.set_write_timeout(2, 0);
}

static void configure_client_embed(httplib::Client& cli) {
  cli.set_connection_timeout(5, 0);
  cli.set_read_timeout(30, 0);
  cli.set_write_timeout(30, 0);
}

} // namespace

OllamaClient OllamaClient::from_env_with_model(const std::string& model_override) {
  OllamaClient c;
  if (const char* e = std::getenv("MNEMON_EMBED_ENDPOINT"); e && *e) {
    c.endpoint = e;
  } else {
    c.endpoint = "http://localhost:11434";
  }
  if (!model_override.empty()) {
    c.model = model_override;
  } else if (const char* m = std::getenv("MNEMON_EMBED_MODEL"); m && *m) {
    c.model = m;
  } else {
    c.model = "nomic-embed-text";
  }
  if (const char* d = std::getenv("MNEMON_EMBED_DIMENSIONS"); d && *d) {
    c.dimensions = std::atoi(d);
  }
  if (const char* api = std::getenv("MNEMON_EMBED_API"); api && *api) {
    std::string value = api;
    if (value == "llama.cpp") {
      c.api = EmbedApi::LlamaCpp;
    } else if (value != "ollama") {
      throw std::runtime_error("MNEMON_EMBED_API must be \"ollama\" or \"llama.cpp\"");
    }
  }
  return c;
}

OllamaClient OllamaClient::from_env() {
  return from_env_with_model("");
}

std::string OllamaClient::availability_path() const {
  return api == EmbedApi::LlamaCpp ? "/health" : "/api/tags";
}

std::string OllamaClient::embedding_path() const {
  return api == EmbedApi::LlamaCpp ? "/v1/embeddings" : "/api/embed";
}

std::string OllamaClient::embedding_request_json(const std::string& text, EmbedTask task) const {
  nlohmann::json body;
  body["model"] = model;
  if (api == EmbedApi::LlamaCpp) {
    const char* prefix = task == EmbedTask::Query ? "search_query: " : "search_document: ";
    body["input"] = std::string(prefix) + text;
    body["encoding_format"] = "float";
  } else {
    body["input"] = text;
  }
  if (dimensions > 0) {
    body["dimensions"] = dimensions;
  }
  return body.dump();
}

std::vector<float> OllamaClient::parse_embedding_response(const std::string& body) const {
  auto json = nlohmann::json::parse(body, nullptr, false);
  const nlohmann::json* embedding = nullptr;
  if (api == EmbedApi::LlamaCpp) {
    if (json.is_object() && json.contains("data") && json["data"].is_array() && !json["data"].empty() &&
        json["data"][0].is_object() && json["data"][0].contains("embedding")) {
      embedding = &json["data"][0]["embedding"];
    }
  } else if (json.is_object() && json.contains("embeddings") && json["embeddings"].is_array() &&
             !json["embeddings"].empty()) {
    embedding = &json["embeddings"][0];
  }
  if (embedding == nullptr || !embedding->is_array() || embedding->empty()) {
    throw std::runtime_error(api == EmbedApi::LlamaCpp ? "llama.cpp empty embedding" : "ollama empty embedding");
  }
  std::vector<float> result;
  result.reserve(embedding->size());
  for (const auto& value : *embedding) {
    result.push_back(value.get<float>());
  }
  if (dimensions > 0 && static_cast<int>(result.size()) != dimensions) {
    if (api == EmbedApi::LlamaCpp && static_cast<int>(result.size()) > dimensions) {
      // Current llama-server accepts the OpenAI `dimensions` field but may
      // still return the model's native vector. Nomic v1.5 is Matryoshka
      // trained, so keeping the leading dimensions is the intended reduction;
      // command callers normalize the shortened vector before use.
      result.resize(static_cast<size_t>(dimensions));
    } else {
      throw std::runtime_error("embedding dimension mismatch: requested " + std::to_string(dimensions) +
                               ", received " + std::to_string(result.size()));
    }
  }
  return result;
}

bool OllamaClient::available() const {
  const auto parts = split_endpoint(endpoint);
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
  if (parts.host_with_scheme.rfind("https://", 0) == 0) {
    return false;
  }
#endif
  httplib::Client cli(parts.host_with_scheme);
  configure_client(cli);
  const std::string suffix = availability_path();
  const std::string path = api_path(parts, suffix);
  auto res = cli.Get(path);
  return res && res->status == 200;
}

std::vector<float> OllamaClient::embed(const std::string& text, EmbedTask task) const {
  const auto parts = split_endpoint(endpoint);
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
  if (parts.host_with_scheme.rfind("https://", 0) == 0) {
    throw std::runtime_error(
        "MNEMON_EMBED_ENDPOINT is https: rebuild mnemon with OpenSSL (CPPHTTPLIB_OPENSSL_SUPPORT) for HTTPS");
  }
#endif
  httplib::Client cli(parts.host_with_scheme);
  configure_client_embed(cli);
  const std::string suffix = embedding_path();
  const std::string path = api_path(parts, suffix);
  auto res = cli.Post(path, embedding_request_json(text, task), "application/json");
  if (!res || res->status != 200) {
    throw std::runtime_error(api == EmbedApi::LlamaCpp ? "llama.cpp embed failed" : "ollama embed failed");
  }
  return parse_embedding_response(res->body);
}

} // namespace mnemon
