#include <catch2/catch_test_macros.hpp>

#include "../src/yaml_lite.hpp"

#include <string>

using namespace mnemon;

// These mirror the Go reference's internal/setup/hermes_test.go invariants at the YAML-model
// layer: the model the C++ Hermes setup is built on must round-trip configs the way yaml.v3
// does and detect mnemon entries by substring.

namespace {

// Count hook entries under hooks.<event>.
size_t hook_count(const yaml::Value& doc, const char* event) {
  auto h = doc.map.find("hooks");
  if (h == doc.map.end() || !h->second.is_map()) {
    return 0;
  }
  auto e = h->second.map.find(event);
  if (e == h->second.map.end() || !e->second.is_seq()) {
    return 0;
  }
  return e->second.seq.size();
}

}  // namespace

TEST_CASE("yaml: parses nested map + block sequence of maps") {
  const std::string input =
      "model:\n"
      "  provider: openrouter\n"
      "hooks:\n"
      "  pre_llm_call:\n"
      "    - command: /custom/inject.sh\n"
      "      timeout: 3\n"
      "  post_llm_call:\n"
      "    - command: /old/mnemon/nudge.sh\n"
      "      timeout: 2\n";

  yaml::Value doc = yaml::parse(input);
  REQUIRE(doc.is_map());

  // Unrelated config preserved.
  REQUIRE(doc.map.at("model").map.at("provider").str == "openrouter");

  // Sequences parsed as one entry each, with both keys.
  REQUIRE(hook_count(doc, "pre_llm_call") == 1);
  REQUIRE(hook_count(doc, "post_llm_call") == 1);
  const auto& pre = doc.map.at("hooks").map.at("pre_llm_call").seq[0];
  REQUIRE(pre.map.at("command").str == "/custom/inject.sh");
  REQUIRE(pre.map.at("timeout").kind == yaml::Kind::Int);
  REQUIRE(pre.map.at("timeout").i == 3);
}

TEST_CASE("yaml: contains_mnemon matches substring anywhere in an entry") {
  yaml::Value mnemon_entry = yaml::Value::make_map();
  mnemon_entry.map["command"] = yaml::Value::make_string("/old/mnemon/nudge.sh");
  mnemon_entry.map["timeout"] = yaml::Value::make_int(2);
  REQUIRE(yaml::contains_mnemon(mnemon_entry));

  yaml::Value custom = yaml::Value::make_map();
  custom.map["command"] = yaml::Value::make_string("/custom/inject.sh");
  REQUIRE_FALSE(yaml::contains_mnemon(custom));
}

TEST_CASE("yaml: marshal is yaml.v3-style — sorted keys, 4-space indent, command before timeout") {
  yaml::Value entry = yaml::Value::make_map();
  entry.map["command"] = yaml::Value::make_string("/h/prime.sh");
  entry.map["timeout"] = yaml::Value::make_int(10);

  yaml::Value seq = yaml::Value::make_seq();
  seq.seq.push_back(entry);

  yaml::Value hooks = yaml::Value::make_map();
  hooks.map["on_session_start"] = seq;

  yaml::Value doc = yaml::Value::make_map();
  doc.map["model"] = yaml::Value::make_string("anthropic");  // sorts after "hooks"
  doc.map["hooks"] = hooks;

  const std::string expected =
      "hooks:\n"
      "    on_session_start:\n"
      "        - command: /h/prime.sh\n"
      "          timeout: 10\n"
      "model: anthropic\n";
  REQUIRE(yaml::marshal(doc) == expected);
}

TEST_CASE("yaml: round-trips the Go fixture structure") {
  const std::string input =
      "model:\n"
      "  provider: openrouter\n"
      "hooks:\n"
      "  pre_llm_call:\n"
      "    - command: /custom/inject.sh\n"
      "      timeout: 3\n";

  yaml::Value once = yaml::parse(input);
  yaml::Value twice = yaml::parse(yaml::marshal(once));
  REQUIRE(yaml::marshal(once) == yaml::marshal(twice));
  REQUIRE(twice.map.at("model").map.at("provider").str == "openrouter");
  REQUIRE(hook_count(twice, "pre_llm_call") == 1);
}

TEST_CASE("yaml: empty / blank document parses as empty map") {
  REQUIRE(yaml::parse("").map.empty());
  REQUIRE(yaml::parse("   \n\n").map.empty());
  REQUIRE(yaml::parse("# just a comment\n").map.empty());
}
