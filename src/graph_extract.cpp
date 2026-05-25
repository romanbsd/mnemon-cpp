// Heuristic entity extraction (CamelCase, caps, paths, URLs, @mentions, tech dict, CJK book-title brackets).
#include "graph_extract.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string_view>
#include <unordered_set>

namespace mnemon::graph_eng {

static const std::unordered_set<std::string> kTechDict = {
    "Go",       "Rust",     "Python",    "Java",      "Kotlin",     "Swift",      "Ruby",       "Elixir",
    "Zig",      "Lua",      "Dart",      "Scala",     "Perl",       "Haskell",    "OCaml",      "Julia",
    "Clojure",  "JavaScript", "TypeScript", "React",   "Vue",        "Angular",    "Svelte",     "Next",
    "Nuxt",     "Node",     "Deno",      "Bun",       "Vite",       "Webpack",    "SQLite",     "PostgreSQL",
    "Postgres", "MySQL",    "Redis",     "MongoDB",   "DynamoDB",   "Cassandra",  "Qdrant",     "Milvus",
    "Chroma",   "Pinecone", "Neo4j",     "Weaviate",  "Elasticsearch", "Docker", "Kubernetes", "Terraform",
    "Ansible",  "Nginx",    "Caddy",     "Kafka",     "RabbitMQ",   "AWS",        "GCP",        "Azure",
    "Vercel",   "Netlify",  "Cloudflare", "Supabase", "Firebase",   "Ollama",     "OpenAI",     "Claude",
    "Anthropic", "PyTorch", "TensorFlow", "LangChain", "LlamaIndex", "FAISS",      "Hugging",    "Git",
    "GitHub",   "GitLab",   "Cobra",     "FastAPI",   "Flask",      "Django",     "Rails",      "Spring",
    "Express",  "Gin",      "Echo",      "Fiber",     "Pytest",     "Jest",       "Vitest",     "gRPC",
    "GraphQL",  "WebSocket", "OAuth",    "JWT",       "YAML",       "TOML",       "Protobuf",   "MAGMA",
    "MCP",      "RLM"};

static const std::unordered_set<std::string> kAcronymStop = {
    "IN", "ON", "AT", "TO", "BY", "OR", "AN", "IF", "IS", "IT", "OF", "AS", "DO", "NO", "SO", "UP", "WE", "HE", "MY",
    "BE", "GO", "THE", "AND", "FOR", "ARE", "BUT", "NOT", "YOU", "ALL", "CAN", "HER", "WAS", "ONE", "OUR", "OUT",
    "HAS", "HAD", "HOW", "MAN", "NEW", "NOW", "OLD", "SEE", "WAY", "MAY", "SAY", "SHE", "TWO", "USE", "BOY", "DID",
    "GET", "HIM", "HIS", "LET", "PUT", "TOP", "TOO", "ANY"};

static std::vector<std::string> split_words(std::string_view text) {
  std::vector<std::string> w;
  std::string cur;
  for (unsigned char ch : text) {
    if (std::isalnum(ch)) {
      cur.push_back(static_cast<char>(ch));
    } else {
      if (!cur.empty()) {
        w.push_back(std::move(cur));
        cur.clear();
      }
    }
  }
  if (!cur.empty()) {
    w.push_back(std::move(cur));
  }
  return w;
}

// std::regex is byte-oriented on libc++; [^》」] can slice UTF-8 and yield invalid
// sequences. Match full-width brackets by explicit UTF-8 delimiters instead.
static void extract_cn_bracket_titles(std::string_view text, const auto& add) {
  struct Pair {
    std::string_view open;
    std::string_view close;
  };
  // U+300A/U+300B 《》, U+300C/U+300D 「」
  static constexpr Pair kPairs[] = {
      {std::string_view("\xE3\x80\x8A", 3), std::string_view("\xE3\x80\x8B", 3)},
      {std::string_view("\xE3\x80\x8C", 3), std::string_view("\xE3\x80\x8D", 3)},
  };
  for (const auto& p : kPairs) {
    std::size_t pos = 0;
    while (pos < text.size()) {
      const auto i = text.find(p.open, pos);
      if (i == std::string_view::npos) {
        break;
      }
      const auto content_start = i + p.open.size();
      const auto j = text.find(p.close, content_start);
      if (j == std::string_view::npos) {
        break;
      }
      add(std::string(text.substr(content_start, j - content_start)));
      pos = j + p.close.size();
    }
  }
}

std::vector<std::string> extract_entities(std::string_view text_sv) {
  std::string text(text_sv);
  std::unordered_set<std::string> seen;
  std::vector<std::string> out;
  static const std::regex re_camel(R"(\b([A-Z][a-z]+(?:[A-Z][a-z]+)+)\b)");
  static const std::regex re_caps(R"(\b([A-Z]{2,6})\b)");
  static const std::regex re_path(R"((?:^|[\s"'(])([.\w/-]+\.\w{1,10})(?:[\s"'),.]|$))");
  static const std::regex re_url(R"(https?://[^\s"'<>)]+)");
  static const std::regex re_mention(R"(@([a-zA-Z_]\w+))");

  auto add = [&](const std::string& entity) {
    if (entity.empty() || seen.count(entity)) {
      return;
    }
    if (kAcronymStop.count(entity)) {
      return;
    }
    seen.insert(entity);
    out.push_back(entity);
  };

  for (auto it = std::sregex_iterator(text.begin(), text.end(), re_camel); it != std::sregex_iterator(); ++it) {
    add((*it)[1].str());
  }
  for (auto it = std::sregex_iterator(text.begin(), text.end(), re_caps); it != std::sregex_iterator(); ++it) {
    add((*it)[1].str());
  }
  for (auto it = std::sregex_iterator(text.begin(), text.end(), re_path); it != std::sregex_iterator(); ++it) {
    add((*it)[1].str());
  }
  for (auto it = std::sregex_iterator(text.begin(), text.end(), re_url); it != std::sregex_iterator(); ++it) {
    add(it->str());
  }
  for (auto it = std::sregex_iterator(text.begin(), text.end(), re_mention); it != std::sregex_iterator(); ++it) {
    add((*it)[1].str());
  }
  extract_cn_bracket_titles(text_sv, add);

  for (const auto& word : split_words(text)) {
    if (kTechDict.count(word) && !seen.count(word)) {
      seen.insert(word);
      out.push_back(word);
    }
  }
  return out;
}

std::vector<std::string> merge_entities(const std::vector<std::string>& provided,
                                        const std::vector<std::string>& extracted) {
  std::unordered_set<std::string> seen;
  std::vector<std::string> merged;
  for (const auto& e : provided) {
    if (!e.empty() && !seen.count(e)) {
      seen.insert(e);
      merged.push_back(e);
    }
  }
  for (const auto& e : extracted) {
    if (!e.empty() && !seen.count(e)) {
      seen.insert(e);
      merged.push_back(e);
    }
  }
  return merged;
}

bool valid_entity_mode(const std::string& mode) {
  return mode == "merge" || mode == "provided" || mode == "auto";
}

std::vector<std::string> resolve_entities(std::string_view content,
                                          const std::vector<std::string>& provided,
                                          const std::string& mode) {
  if (mode == "provided") {
    return provided;
  }
  if (mode == "auto") {
    return extract_entities(content);
  }
  return merge_entities(provided, extract_entities(content));
}

} // namespace mnemon::graph_eng
