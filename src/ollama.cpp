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

static std::string api_path(const EndpointParts& p, const char* suffix) {
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
  return c;
}

OllamaClient OllamaClient::from_env() {
  return from_env_with_model("");
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
  const std::string path = api_path(parts, "/api/tags");
  auto res = cli.Get(path);
  return res && res->status == 200;
}

std::vector<double> OllamaClient::embed(const std::string& text) const {
  const auto parts = split_endpoint(endpoint);
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
  if (parts.host_with_scheme.rfind("https://", 0) == 0) {
    throw std::runtime_error(
        "MNEMON_EMBED_ENDPOINT is https: rebuild mnemon with OpenSSL (CPPHTTPLIB_OPENSSL_SUPPORT) for HTTPS");
  }
#endif
  httplib::Client cli(parts.host_with_scheme);
  configure_client_embed(cli);
  nlohmann::json body;
  body["model"] = model;
  body["input"] = text;
  if (dimensions > 0) {
    body["dimensions"] = dimensions;
  }
  const std::string path = api_path(parts, "/api/embed");
  auto res = cli.Post(path, body.dump(), "application/json");
  if (!res || res->status != 200) {
    throw std::runtime_error("ollama embed failed");
  }
  auto j = nlohmann::json::parse(res->body, nullptr, false);
  if (!j.is_object() || !j.contains("embeddings")) {
    throw std::runtime_error("ollama bad response");
  }
  auto& arr = j["embeddings"];
  if (!arr.is_array() || arr.empty() || !arr[0].is_array()) {
    throw std::runtime_error("ollama empty embedding");
  }
  std::vector<double> v;
  for (const auto& x : arr[0]) {
    v.push_back(x.get<double>());
  }
  return v;
}

} // namespace mnemon
