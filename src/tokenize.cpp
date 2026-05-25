#include "tokenize.hpp"

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace mnemon::search_engine {

// Manual UTF-8 decode: advances i and sets cp; invalid sequences consume one byte (lossy but safe).
static bool utf8_next(const std::string& s, size_t& i, uint32_t& cp) {
  if (i >= s.size()) {
    return false;
  }
  unsigned char c0 = static_cast<unsigned char>(s[i]);
  if (c0 < 0x80) {
    cp = c0;
    ++i;
    return true;
  }
  if ((c0 & 0xE0) == 0xC0 && i + 1 < s.size()) {
    cp = ((c0 & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
    i += 2;
    return true;
  }
  if ((c0 & 0xF0) == 0xE0 && i + 2 < s.size()) {
    cp = ((c0 & 0x0F) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
         (static_cast<unsigned char>(s[i + 2]) & 0x3F);
    i += 3;
    return true;
  }
  if ((c0 & 0xF8) == 0xF0 && i + 3 < s.size()) {
    cp = ((c0 & 0x07) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
         ((static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) | (static_cast<unsigned char>(s[i + 3]) & 0x3F);
    i += 4;
    return true;
  }
  cp = c0;
  ++i;
  return true;
}

// CJK Unified Ideographs + extensions — drives bigram tokenization for Chinese-like text.
bool is_han(uint32_t cp) {
  return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x20000 && cp <= 0x2A6DF);
}

static void append_rune_utf8(std::string& s, uint32_t cp) {
  if (cp < 0x80) {
    s.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// For CJK runs of length ≥2, emit overlapping bigrams (matches keyword overlap semantics for Han).
static void flush_cjk(std::u32string& buf, TokenSet& tokens) {
  if (buf.size() < 2) {
    if (buf.size() == 1) {
      std::string u8;
      append_rune_utf8(u8, buf[0]);
      tokens[std::move(u8)] = true;
    }
    buf.clear();
    return;
  }
  for (size_t j = 0; j + 1 < buf.size(); ++j) {
    std::string pair;
    append_rune_utf8(pair, buf[j]);
    append_rune_utf8(pair, buf[j + 1]);
    tokens[std::move(pair)] = true;
  }
  buf.clear();
}

const std::unordered_set<std::string>& stopwords() {
  static const std::unordered_set<std::string> sw = {
      "a",     "an",    "the",   "is",    "are",   "was",   "were",  "be",     "been",  "being", "have",  "has",
      "had",   "do",    "does",  "did",   "will",  "would", "could", "should", "may",   "might", "shall", "can",
      "to",    "of",    "in",    "for",   "on",    "with",  "at",    "by",     "from",  "as",    "into",  "about",
      "that",  "this",  "it",    "its",   "or",    "and",   "but",   "if",     "not",   "no",    "so",    "up",
      "out",   "than",  "then",  "too",   "very",  "just",  "also",  "more",   "some",  "any",   "all",   "each",
      "i",     "me",    "my",    "we",    "you",   "your",  "he",    "she",    "they",  "them",  "his",   "her",
      "our",   "their", "what",  "which", "who",   "how",   "when",  "where"};
  return sw;
}

// Lowercase ASCII, split alnum/Latin words (minus stopwords), and bigram Han sequences.
TokenSet tokenize(std::string_view text_sv) {
  std::string text(text_sv);
  for (auto& c : text) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  TokenSet tokens;
  std::string word;
  std::u32string cjk_buf;
  size_t i = 0;
  while (i < text.size()) {
    uint32_t cp = 0;
    size_t j = i;
    if (!utf8_next(text, i, cp)) {
      break;
    }
    (void)j;
    if (is_han(cp)) {
      if (!word.empty()) {
        if (!stopwords().count(word)) {
          tokens[word] = true;
        }
        word.clear();
      }
      cjk_buf.push_back(cp);
    } else {
      if (!cjk_buf.empty()) {
        flush_cjk(cjk_buf, tokens);
      }
      if (std::isalnum(static_cast<unsigned char>(cp)) || cp > 127) {
        // append UTF-8 for this codepoint to word (text already lowercased ASCII)
        if (cp < 128) {
          word.push_back(static_cast<char>(cp));
        } else {
          if (cp < 0x800) {
            word.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            word.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
          } else if (cp < 0x10000) {
            word.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            word.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            word.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
          }
        }
      } else {
        if (!word.empty()) {
          if (!stopwords().count(word)) {
            tokens[word] = true;
          }
          word.clear();
        }
      }
    }
  }
  if (!word.empty()) {
    if (!stopwords().count(word)) {
      tokens[word] = true;
    }
  }
  if (!cjk_buf.empty()) {
    flush_cjk(cjk_buf, tokens);
  }
  return tokens;
}

// Max directional overlap: intersection / |A| vs intersection / |B| — asymmetric but cheap dedup signal.
double content_similarity(std::string_view a, std::string_view b) {
  auto ta = tokenize(a);
  auto tb = tokenize(b);
  if (ta.empty() || tb.empty()) {
    return 0;
  }
  int inter = 0;
  for (const auto& [k, _] : ta) {
    if (tb.count(k)) {
      inter++;
    }
  }
  double sa = static_cast<double>(inter) / static_cast<double>(ta.size());
  double sb = static_cast<double>(inter) / static_cast<double>(tb.size());
  return sa > sb ? sa : sb;
}

double jaccard_similarity(std::string_view a, std::string_view b) {
  auto ta = tokenize(a);
  auto tb = tokenize(b);
  if (ta.empty() || tb.empty()) {
    return 0;
  }
  int inter = 0;
  for (const auto& [k, _] : ta) {
    if (tb.count(k)) {
      inter++;
    }
  }
  int uni = static_cast<int>(ta.size()) + static_cast<int>(tb.size()) - inter;
  if (uni == 0) {
    return 0;
  }
  return static_cast<double>(inter) / static_cast<double>(uni);
}

} // namespace mnemon::search_engine
