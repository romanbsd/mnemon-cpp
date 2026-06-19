#pragma once

// Minimal YAML value model + block-style parser + yaml.v3-style emitter.
//
// This exists solely to keep `mnemon setup --target hermes` byte-compatible with the Go
// reference, which treats Hermes' `~/.hermes/config.yaml` as a real document: read into a
// `map[string]any`, mutate, then re-marshal with `go.yaml.in/yaml/v3`. We mirror that
// parse -> mutate -> marshal pipeline rather than doing line-based string surgery.
//
// Scope: the block-style YAML subset that realistic Hermes configs use — indentation-nested
// mappings, block sequences (`- `), and plain / single- / double-quoted scalars (string,
// int, bool, null). `#` comments and blank lines are dropped (yaml.v3 drops comments on
// re-marshal anyway). Flow style (`{}` / `[]`), anchors/aliases, and multiline block
// scalars are NOT supported; configs using them are out of contract.
//
// Emitter targets yaml.v3's documented defaults: map keys sorted, 4-space indent, block
// sequence dashes indented one level under their key with the first map key inline after
// `- `, scalars emitted plain unless quoting is required, trailing newline, no leading `---`.

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mnemon::yaml {

enum class Kind { Null, Bool, Int, String, Sequence, Map };

// A YAML value. Maps keep keys in a std::map so emission is sorted — which matches yaml.v3's
// marshal order for the lowercase identifier keys Hermes uses (e.g. command < timeout,
// model < on_session_finalize < on_session_start < post_llm_call < pre_llm_call).
struct Value {
  Kind kind{Kind::Null};
  bool b{false};
  long long i{0};
  std::string str;
  std::vector<Value> seq;
  std::map<std::string, Value> map;

  static Value make_map() {
    Value v;
    v.kind = Kind::Map;
    return v;
  }
  static Value make_seq() {
    Value v;
    v.kind = Kind::Sequence;
    return v;
  }
  static Value make_string(std::string s) {
    Value v;
    v.kind = Kind::String;
    v.str = std::move(s);
    return v;
  }
  static Value make_int(long long n) {
    Value v;
    v.kind = Kind::Int;
    v.i = n;
    return v;
  }

  bool is_map() const { return kind == Kind::Map; }
  bool is_seq() const { return kind == Kind::Sequence; }
};

// Parse a YAML document into a Value. An empty / blank document yields an empty map, matching
// the Go reference's readYAMLFile. Throws std::runtime_error on input outside the supported
// subset rather than silently mangling it.
Value parse(const std::string& text);

// Marshal a Value to yaml.v3-style text (always ends in a newline).
std::string marshal(const Value& v);

// Recursively reports whether any scalar string in the value contains the substring
// "mnemon" — the mirror of the Go reference's containsMnemon.
bool contains_mnemon(const Value& v);

}  // namespace mnemon::yaml
