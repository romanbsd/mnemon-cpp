#include "yaml_lite.hpp"

#include <cctype>
#include <stdexcept>

namespace mnemon::yaml {

namespace {

// --- helpers ---

std::string pad(int n) { return std::string(static_cast<size_t>(n < 0 ? 0 : n), ' '); }

std::string rtrim(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
    s.pop_back();
  }
  return s;
}

std::string ltrim(const std::string& s) {
  size_t i = s.find_first_not_of(" \t");
  return i == std::string::npos ? std::string() : s.substr(i);
}

// Strip a trailing `# comment` (must be preceded by whitespace / line start and sit outside
// any quotes). yaml.v3 drops comments on re-marshal, so we never reproduce them.
std::string strip_comment(const std::string& s) {
  bool in_single = false, in_double = false;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == '\'' && !in_double) {
      in_single = !in_single;
    } else if (c == '"' && !in_single) {
      in_double = !in_double;
    } else if (c == '#' && !in_single && !in_double && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) {
      return s.substr(0, i);
    }
  }
  return s;
}

bool is_integer(const std::string& s) {
  if (s.empty()) {
    return false;
  }
  size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
  if (i == s.size()) {
    return false;
  }
  for (; i < s.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(s[i]))) {
      return false;
    }
  }
  return true;
}

std::string unquote(const std::string& s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
    std::string out;
    for (size_t i = 1; i + 1 < s.size(); ++i) {
      if (s[i] == '\\' && i + 2 < s.size()) {
        char n = s[i + 1];
        switch (n) {
          case 'n': out.push_back('\n'); break;
          case 't': out.push_back('\t'); break;
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          default: out.push_back(n); break;
        }
        ++i;
      } else {
        out.push_back(s[i]);
      }
    }
    return out;
  }
  if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

Value scalar_from(const std::string& raw) {
  std::string s = rtrim(raw);
  if (s.empty() || s == "~" || s == "null") {
    return Value{};  // Null
  }
  if (s.front() == '"' || s.front() == '\'') {
    return Value::make_string(unquote(s));
  }
  if (s == "true" || s == "false") {
    Value v;
    v.kind = Kind::Bool;
    v.b = (s == "true");
    return v;
  }
  if (is_integer(s)) {
    return Value::make_int(std::stoll(s));
  }
  return Value::make_string(s);
}

bool is_seq_line(const std::string& text) {
  return text == "-" || (text.size() >= 2 && text[0] == '-' && text[1] == ' ');
}

// Split "key: value" into key and the raw value. Returns false if no key/colon separator is
// found. The separating colon is the first `:` followed by a space or end-of-line, so colons
// inside values (URLs, paths) are left intact.
bool split_kv(const std::string& text, std::string& key, std::string& value) {
  bool in_single = false, in_double = false;
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (c == '\'' && !in_double) {
      in_single = !in_single;
    } else if (c == '"' && !in_single) {
      in_double = !in_double;
    } else if (c == ':' && !in_single && !in_double && (i + 1 == text.size() || text[i + 1] == ' ')) {
      key = unquote(rtrim(text.substr(0, i)));
      value = (i + 1 == text.size()) ? std::string() : ltrim(text.substr(i + 1));
      return true;
    }
  }
  return false;
}

struct Line {
  int indent;
  std::string text;
};

class Parser {
 public:
  explicit Parser(std::vector<Line> lines) : lines_(std::move(lines)) {}

  Value parse_document() {
    if (pos_ >= lines_.size()) {
      return Value::make_map();
    }
    return parse_node(lines_[pos_].indent);
  }

 private:
  Value parse_node(int indent) {
    if (pos_ < lines_.size() && is_seq_line(lines_[pos_].text)) {
      return parse_seq(indent);
    }
    return parse_map(indent);
  }

  Value parse_map(int indent) {
    Value m = Value::make_map();
    while (pos_ < lines_.size() && lines_[pos_].indent == indent && !is_seq_line(lines_[pos_].text)) {
      const std::string text = lines_[pos_].text;
      std::string key, value;
      if (!split_kv(text, key, value)) {
        throw std::runtime_error("yaml: expected 'key: value' near: " + text);
      }
      ++pos_;
      if (!value.empty()) {
        m.map[key] = scalar_from(value);
        continue;
      }
      // Block value on following lines: either a deeper-indented node, or a sequence whose
      // dashes sit at the same column as the key.
      if (pos_ < lines_.size() && lines_[pos_].indent > indent) {
        m.map[key] = parse_node(lines_[pos_].indent);
      } else if (pos_ < lines_.size() && lines_[pos_].indent == indent && is_seq_line(lines_[pos_].text)) {
        m.map[key] = parse_seq(indent);
      } else {
        m.map[key] = Value{};  // Null
      }
    }
    return m;
  }

  Value parse_seq(int indent) {
    Value s = Value::make_seq();
    while (pos_ < lines_.size() && lines_[pos_].indent == indent && is_seq_line(lines_[pos_].text)) {
      const std::string text = lines_[pos_].text;
      // Content after the dash; its column establishes the indent of an inline mapping.
      size_t off = 1;
      while (off < text.size() && text[off] == ' ') {
        ++off;
      }
      const int item_indent = indent + static_cast<int>(off);
      const std::string content = text.substr(off);
      ++pos_;

      if (content.empty()) {
        if (pos_ < lines_.size() && lines_[pos_].indent > indent) {
          s.seq.push_back(parse_node(lines_[pos_].indent));
        } else {
          s.seq.push_back(Value{});
        }
        continue;
      }

      std::string key, value;
      if (split_kv(content, key, value)) {
        // Mapping item: first entry inline after "- ", remaining entries aligned at item_indent.
        Value item = Value::make_map();
        if (!value.empty()) {
          item.map[key] = scalar_from(value);
        } else if (pos_ < lines_.size() && lines_[pos_].indent > item_indent) {
          item.map[key] = parse_node(lines_[pos_].indent);
        } else {
          item.map[key] = Value{};
        }
        while (pos_ < lines_.size() && lines_[pos_].indent == item_indent && !is_seq_line(lines_[pos_].text)) {
          std::string k2, v2;
          if (!split_kv(lines_[pos_].text, k2, v2)) {
            throw std::runtime_error("yaml: expected 'key: value' near: " + lines_[pos_].text);
          }
          ++pos_;
          if (!v2.empty()) {
            item.map[k2] = scalar_from(v2);
          } else if (pos_ < lines_.size() && lines_[pos_].indent > item_indent) {
            item.map[k2] = parse_node(lines_[pos_].indent);
          } else {
            item.map[k2] = Value{};
          }
        }
        s.seq.push_back(std::move(item));
      } else {
        s.seq.push_back(scalar_from(content));
      }
    }
    return s;
  }

  std::vector<Line> lines_;
  size_t pos_{0};
};

// --- emitter ---

bool needs_quote(const std::string& s) {
  if (s.empty()) {
    return true;
  }
  if (s == "null" || s == "~" || s == "true" || s == "false" || s == "yes" || s == "no" ||
      s == "on" || s == "off") {
    return true;
  }
  if (is_integer(s)) {
    return true;
  }
  if (s.front() == ' ' || s.back() == ' ') {
    return true;
  }
  // Leading YAML indicator characters that can't start a plain scalar.
  static const std::string kLeading = "!&*?|>%@`\"'#,[]{}:-";
  if (kLeading.find(s.front()) != std::string::npos) {
    return true;
  }
  // ": " and " #" carry structural meaning inside a plain scalar.
  if (s.find(": ") != std::string::npos || s.find(" #") != std::string::npos) {
    return true;
  }
  return false;
}

std::string quote(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

std::string scalar_text(const Value& v) {
  switch (v.kind) {
    case Kind::Null: return "null";
    case Kind::Bool: return v.b ? "true" : "false";
    case Kind::Int: return std::to_string(v.i);
    case Kind::String: return needs_quote(v.str) ? quote(v.str) : v.str;
    default: return "";
  }
}

std::string emit_key(const std::string& k) { return needs_quote(k) ? quote(k) : k; }

void emit_map(const Value& m, int spaces, std::string& out);
void emit_seq(const Value& s, int spaces, std::string& out);

// Emit a value positioned immediately after a "key:" already written by the caller.
void emit_after_key(const Value& v, int spaces, std::string& out) {
  if (v.kind == Kind::Map) {
    if (v.map.empty()) {
      out += " {}\n";
    } else {
      out += "\n";
      emit_map(v, spaces + 4, out);
    }
  } else if (v.kind == Kind::Sequence) {
    if (v.seq.empty()) {
      out += " []\n";
    } else {
      out += "\n";
      emit_seq(v, spaces + 4, out);
    }
  } else {
    out += " " + scalar_text(v) + "\n";
  }
}

void emit_map(const Value& m, int spaces, std::string& out) {
  for (const auto& [k, v] : m.map) {  // std::map iterates in sorted key order
    out += pad(spaces) + emit_key(k) + ":";
    emit_after_key(v, spaces, out);
  }
}

void emit_seq(const Value& s, int spaces, std::string& out) {
  for (const auto& item : s.seq) {
    out += pad(spaces) + "-";
    if (item.kind == Kind::Map) {
      if (item.map.empty()) {
        out += " {}\n";
        continue;
      }
      const int child = spaces + 2;
      bool first = true;
      for (const auto& [k, v] : item.map) {
        if (first) {
          out += " " + emit_key(k) + ":";
          first = false;
        } else {
          out += pad(child) + emit_key(k) + ":";
        }
        emit_after_key(v, child, out);
      }
    } else if (item.kind == Kind::Sequence) {
      if (item.seq.empty()) {
        out += " []\n";
      } else {
        out += "\n";
        emit_seq(item, spaces + 2, out);
      }
    } else {
      out += " " + scalar_text(item) + "\n";
    }
  }
}

}  // namespace

Value parse(const std::string& text) {
  std::vector<Line> lines;
  size_t i = 0;
  while (i < text.size()) {
    size_t nl = text.find('\n', i);
    std::string raw = text.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
    i = (nl == std::string::npos) ? text.size() : nl + 1;

    std::string stripped = rtrim(strip_comment(raw));
    if (stripped.find_first_not_of(" \t") == std::string::npos) {
      continue;  // blank / comment-only line
    }
    int indent = static_cast<int>(stripped.find_first_not_of(' '));
    lines.push_back(Line{indent, stripped.substr(static_cast<size_t>(indent))});
  }
  Parser p(std::move(lines));
  return p.parse_document();
}

std::string marshal(const Value& v) {
  std::string out;
  if (v.kind == Kind::Map) {
    if (v.map.empty()) {
      return "{}\n";
    }
    emit_map(v, 0, out);
  } else if (v.kind == Kind::Sequence) {
    if (v.seq.empty()) {
      return "[]\n";
    }
    emit_seq(v, 0, out);
  } else {
    out += scalar_text(v) + "\n";
  }
  return out;
}

bool contains_mnemon(const Value& v) {
  switch (v.kind) {
    case Kind::String:
      return v.str.find("mnemon") != std::string::npos;
    case Kind::Sequence:
      for (const auto& e : v.seq) {
        if (contains_mnemon(e)) {
          return true;
        }
      }
      return false;
    case Kind::Map:
      for (const auto& [k, e] : v.map) {
        if (contains_mnemon(e)) {
          return true;
        }
      }
      return false;
    default:
      return false;
  }
}

}  // namespace mnemon::yaml
