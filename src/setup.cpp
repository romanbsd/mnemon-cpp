// Interactive and `--json` setup: detect host app, merge hook configs, install/eject embedded assets (go:embed parity).
#include "setup.hpp"

#include "db.hpp"
#include "embedded_assets.hpp"
#include "paths.hpp"
#include "yaml_lite.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace mnemon::setup {

namespace {

struct Environment {
  std::string name;
  std::string display;
  bool detected = false;
  std::string bin_path;
  bool installed = false;
  std::string version;
  std::string config_dir;
};

struct HookSelection {
  bool remind = true;
  bool nudge = true;
  bool compact = false;
};

// --- ANSI / TTY ---

static bool is_tty_in() { return isatty(STDIN_FILENO) != 0; }
static bool is_tty_out() { return isatty(STDOUT_FILENO) != 0; }

static const char* c_green() { return is_tty_out() ? "\033[32m" : ""; }
static const char* c_dim() { return is_tty_out() ? "\033[2m" : ""; }
static const char* c_red() { return is_tty_out() ? "\033[31m" : ""; }
static const char* c_bold() { return is_tty_out() ? "\033[1m" : ""; }
static const char* c_reset() { return is_tty_out() ? "\033[0m" : ""; }

static void status_ok(const std::string& label, const std::string& detail) {
  std::cout << "  " << c_green() << "✓" << c_reset() << " " << std::left << std::setw(12) << label << c_dim() << detail
            << c_reset() << "\n";
}
static void status_updated(const std::string& label, const std::string& detail) {
  std::cout << "  " << c_green() << "✓" << c_reset() << " " << std::left << std::setw(12) << label << c_dim() << detail
            << c_reset() << "  " << c_green() << "updated" << c_reset() << "\n";
}
static void status_error(const std::string& label, const std::string& err) {
  std::cout << "  " << c_red() << "✗" << c_reset() << " " << std::left << std::setw(12) << label << c_red() << err
            << c_reset() << "\n";
}

static std::string home_dir() {
  if (const char* h = std::getenv("HOME")) {
    return h;
  }
  return "";
}

static std::string clean_version(std::string v) {
  auto pos = v.find(" (");
  if (pos != std::string::npos && pos > 0) {
    v.resize(pos);
  }
  return v;
}

static bool look_path(const std::string& name, std::string& out) {
  const char* path_env = std::getenv("PATH");
  if (!path_env) {
    return false;
  }
  std::string pathstr = path_env;
  size_t start = 0;
  while (start < pathstr.size()) {
    size_t end = pathstr.find(':', start);
    if (end == std::string::npos) {
      end = pathstr.size();
    }
    std::string dir = pathstr.substr(start, end - start);
    if (!dir.empty()) {
      fs::path p = fs::path(dir) / name;
      std::error_code ec;
      if (fs::is_regular_file(p, ec)) {
        out = p.string();
        return true;
      }
    }
    start = end + 1;
  }
  return false;
}

static std::string exec_version(const std::string& bin) {
  std::string cmd = "\"" + bin + "\" --version 2>&1";
  FILE* p = popen(cmd.c_str(), "r");
  if (!p) {
    return "";
  }
  std::array<char, 256> buf{};
  std::string out;
  while (fgets(buf.data(), buf.size(), p)) {
    out += buf.data();
  }
  pclose(p);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
    out.pop_back();
  }
  return clean_version(out);
}

// --- JSON5 strip (matches Go internal/setup/settings.go) ---

static std::string strip_json5(std::string_view s) {
  std::string b;
  b.reserve(s.size());
  bool in_string = false;
  bool escaped = false;
  size_t i = 0;
  while (i < s.size()) {
    unsigned char ch = static_cast<unsigned char>(s[i]);
    if (escaped) {
      b.push_back(static_cast<char>(ch));
      escaped = false;
      ++i;
      continue;
    }
    if (in_string) {
      if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        in_string = false;
      }
      b.push_back(static_cast<char>(ch));
      ++i;
      continue;
    }
    if (ch == '"') {
      in_string = true;
      b.push_back(static_cast<char>(ch));
      ++i;
      continue;
    }
    if (ch == '/' && i + 1 < s.size() && s[i + 1] == '/') {
      while (i < s.size() && s[i] != '\n') {
        ++i;
      }
      continue;
    }
    if (ch == ',') {
      size_t j = i + 1;
      while (j < s.size() && (s[j] == ' ' || s[j] == '\t' || s[j] == '\n' || s[j] == '\r')) {
        ++j;
      }
      if (j < s.size() && (s[j] == ']' || s[j] == '}')) {
        ++i;
        continue;
      }
    }
    b.push_back(static_cast<char>(ch));
    ++i;
  }
  return b;
}

static nlohmann::json read_json_file(const fs::path& path) {
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    return nlohmann::json::object();
  }
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("read " + path.string());
  }
  std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (raw.empty()) {
    return nlohmann::json::object();
  }
  std::string cleaned = strip_json5(raw);
  return nlohmann::json::parse(cleaned);
}

static void write_json_file(const fs::path& path, const nlohmann::json& j) {
  std::string b = j.dump(2);
  b.push_back('\n');
  fs::create_directories(path.parent_path());
  fs::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) {
      throw std::runtime_error("write tmp " + tmp.string());
    }
    out << b;
  }
  fs::rename(tmp, path);
}

static void write_or_remove_json_file(const fs::path& path, const nlohmann::json& j) {
  if (j.empty() || (j.is_object() && j.begin() == j.end())) {
    std::error_code ec;
    fs::remove(path, ec);
    return;
  }
  write_json_file(path, j);
}

static bool json_contains_mnemon(const nlohmann::json& v) {
  if (v.is_string()) {
    return v.get<std::string>().find("mnemon") != std::string::npos;
  }
  if (v.is_object()) {
    for (auto it = v.begin(); it != v.end(); ++it) {
      if (json_contains_mnemon(it.value())) {
        return true;
      }
    }
  }
  if (v.is_array()) {
    for (const auto& x : v) {
      if (json_contains_mnemon(x)) {
        return true;
      }
    }
  }
  return false;
}

static nlohmann::json filter_hook_array(const nlohmann::json& arr) {
  nlohmann::json out = nlohmann::json::array();
  if (!arr.is_array()) {
    return out;
  }
  for (const auto& entry : arr) {
    if (!json_contains_mnemon(entry)) {
      out.push_back(entry);
    }
  }
  return out;
}

static void remove_claude_hooks(nlohmann::json& data) {
  if (!data.contains("hooks") || !data["hooks"].is_object()) {
    return;
  }
  auto& hooks = data["hooks"];
  static const char* keys[] = {"UserPromptSubmit", "Stop", "SessionStart", "PreCompact"};
  for (const char* key : keys) {
    if (!hooks.contains(key)) {
      continue;
    }
    nlohmann::json filtered = filter_hook_array(hooks[key]);
    if (filtered.empty() || (filtered.is_array() && filtered.size() == 0)) {
      hooks.erase(key);
    } else {
      hooks[key] = filtered;
    }
  }
  if (hooks.empty()) {
    data.erase("hooks");
  }
}

static void add_claude_hooks_selective(nlohmann::json& data, const std::string& hooks_dir, const HookSelection& sel) {
  remove_claude_hooks(data);
  if (!data.contains("hooks") || !data["hooks"].is_object()) {
    data["hooks"] = nlohmann::json::object();
  }
  auto& hooks = data["hooks"];
  fs::path hd = hooks_dir;

  auto prime_entry = nlohmann::json::object();
  prime_entry["hooks"] = nlohmann::json::array({{{"type", "command"}, {"command", (hd / "prime.sh").string()}}});
  if (!hooks.contains("SessionStart")) {
    hooks["SessionStart"] = nlohmann::json::array();
  }
  hooks["SessionStart"].push_back(prime_entry);

  if (sel.remind) {
    auto remind_entry = nlohmann::json::object();
    remind_entry["hooks"] =
        nlohmann::json::array({{{"type", "command"}, {"command", (hd / "user_prompt.sh").string()}}});
    if (!hooks.contains("UserPromptSubmit")) {
      hooks["UserPromptSubmit"] = nlohmann::json::array();
    }
    hooks["UserPromptSubmit"].push_back(remind_entry);
  }
  if (sel.nudge) {
    auto nudge_entry = nlohmann::json::object();
    nudge_entry["hooks"] = nlohmann::json::array({{{"type", "command"}, {"command", (hd / "stop.sh").string()}}});
    if (!hooks.contains("Stop")) {
      hooks["Stop"] = nlohmann::json::array();
    }
    hooks["Stop"].push_back(nudge_entry);
  }
  if (sel.compact) {
    auto compact_entry = nlohmann::json::object();
    compact_entry["hooks"] = nlohmann::json::array({{{"type", "command"}, {"command", (hd / "compact.sh").string()}}});
    if (!hooks.contains("PreCompact")) {
      hooks["PreCompact"] = nlohmann::json::array();
    }
    hooks["PreCompact"].push_back(compact_entry);
  }
}

static void remove_if_empty_dir(const fs::path& dir) {
  std::error_code ec;
  if (!fs::exists(dir, ec)) {
    return;
  }
  if (fs::is_empty(dir, ec)) {
    fs::remove(dir, ec);
  }
}

static void chmod_path(const fs::path& p, mode_t mode) {
  ::chmod(p.string().c_str(), mode);
}

static void write_bytes(const fs::path& path, std::string_view data, mode_t mode) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("write " + path.string());
  }
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  out.close();
  chmod_path(path, mode);
}

// --- prompts ---

static bool confirm(const std::string& prompt, bool default_yes) {
  std::string hint = default_yes ? "Y/n" : "y/N";
  std::cout << prompt << " " << c_dim() << "[" << hint << "]" << c_reset() << " › " << std::flush;
  std::string line;
  if (!std::getline(std::cin, line)) {
    return default_yes;
  }
  for (char& ch : line) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  if (line.empty()) {
    return default_yes;
  }
  return line == "y" || line == "yes";
}

static size_t select_one(const std::string& title, const std::vector<std::string>& options, size_t default_idx) {
  if (!is_tty_in() || options.empty()) {
    return default_idx;
  }
  std::cout << "\n  " << c_bold() << title << c_reset() << "\n";
  for (size_t i = 0; i < options.size(); ++i) {
    std::cout << "  " << (i + 1) << ") " << options[i] << "\n";
  }
  std::cout << "  " << c_dim() << "[1-" << options.size() << ", Enter=" << (default_idx + 1) << "]" << c_reset()
            << " › " << std::flush;
  std::string line;
  if (!std::getline(std::cin, line)) {
    return default_idx;
  }
  if (line.empty()) {
    return default_idx;
  }
  try {
    int n = std::stoi(line);
    if (n >= 1 && static_cast<size_t>(n) <= options.size()) {
      return static_cast<size_t>(n) - 1;
    }
  } catch (...) {
  }
  return default_idx;
}

static HookSelection select_multi(const std::string& title, const std::vector<std::string>& options,
                                  const HookSelection& defaults) {
  HookSelection sel = defaults;
  if (!is_tty_in()) {
    return sel;
  }
  while (true) {
    std::cout << "\n  " << c_bold() << title << c_reset() << " " << c_dim() << "(toggle: 1-" << options.size()
              << ", Enter=done)" << c_reset() << "\n";
    std::cout << "  1. " << (sel.remind ? "[x]" : "[ ]") << " " << options[0] << "\n";
    std::cout << "  2. " << (sel.nudge ? "[x]" : "[ ]") << " " << options[1] << "\n";
    std::cout << "  3. " << (sel.compact ? "[x]" : "[ ]") << " " << options[2] << "\n";
    std::cout << "› " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) {
      break;
    }
    if (line.empty()) {
      break;
    }
    try {
      int n = std::stoi(line);
      if (n == 1) {
        sel.remind = !sel.remind;
      } else if (n == 2) {
        sel.nudge = !sel.nudge;
      } else if (n == 3) {
        sel.compact = !sel.compact;
      }
    } catch (...) {
    }
  }
  return sel;
}

static void detection_line(bool detected, const std::string& display, const std::string& version,
                           const std::string& path) {
  std::string disp_path = path;
  std::string hd = home_dir();
  if (!hd.empty()) {
    size_t pos = disp_path.find(hd);
    if (pos == 0) {
      disp_path.replace(0, hd.size(), "~");
    }
  }
  if (detected) {
    std::cout << "  " << c_green() << "✓" << c_reset() << " " << std::left << std::setw(14) << display << c_dim()
              << std::setw(12) << version << disp_path << c_reset() << "\n";
  } else {
    std::cout << "  " << c_dim() << "·" << c_reset() << " " << std::left << std::setw(14) << display << " (not found)"
              << c_reset() << "\n";
  }
}

// --- detect ---

static Environment detect_claude(bool global) {
  Environment env;
  env.name = "claude-code";
  env.display = "Claude Code";
  fs::path global_dir = fs::path(home_dir()) / ".claude";
  std::string local_dir = ".claude";
  env.config_dir = global ? global_dir.string() : local_dir;

  std::string bin;
  if (look_path("claude", bin)) {
    env.detected = true;
    env.bin_path = bin;
    env.version = exec_version(bin);
  }
  std::error_code ec;
  if (fs::exists(global_dir, ec)) {
    env.detected = true;
  }
  fs::path skill = fs::path(env.config_dir) / "skills" / "mnemon" / "SKILL.md";
  if (fs::exists(skill, ec)) {
    env.installed = true;
  }
  return env;
}

static Environment detect_openclaw(bool global) {
  Environment env;
  env.name = "openclaw";
  env.display = "OpenClaw";
  fs::path global_dir = fs::path(home_dir()) / ".openclaw";
  std::string local_dir = ".openclaw";
  env.config_dir = global ? global_dir.string() : local_dir;

  std::string bin;
  if (look_path("openclaw", bin)) {
    env.detected = true;
    env.bin_path = bin;
    env.version = exec_version(bin);
  }
  std::error_code ec;
  if (fs::exists(global_dir, ec)) {
    env.detected = true;
  }
  fs::path skill = fs::path(env.config_dir) / "skills" / "mnemon" / "SKILL.md";
  if (fs::exists(skill, ec)) {
    env.installed = true;
  }
  return env;
}

static Environment detect_codex(bool global) {
  Environment env;
  env.name = "codex";
  env.display = "Codex";
  fs::path global_dir = fs::path(home_dir()) / ".codex";
  std::string local_dir = ".codex";
  env.config_dir = global ? global_dir.string() : local_dir;

  std::string bin;
  if (look_path("codex", bin)) {
    env.detected = true;
    env.bin_path = bin;
    env.version = exec_version(bin);
  }
  std::error_code ec;
  if (fs::exists(global_dir, ec)) {
    env.detected = true;
  }
  fs::path skill = fs::path(env.config_dir) / "skills" / "mnemon" / "SKILL.md";
  if (fs::exists(skill, ec)) {
    env.installed = true;
  }
  return env;
}

static Environment detect_cursor(bool global) {
  Environment env;
  env.name = "cursor";
  env.display = "Cursor";
  fs::path global_dir = fs::path(home_dir()) / ".cursor";
  std::string local_dir = ".cursor";
  env.config_dir = global ? global_dir.string() : local_dir;

  std::string bin;
  if (look_path("cursor", bin)) {
    env.detected = true;
    env.bin_path = bin;
    env.version = exec_version(bin);
  }
  std::error_code ec;
  if (fs::exists(global_dir, ec)) {
    env.detected = true;
  }
  fs::path skill = fs::path(env.config_dir) / "skills" / "mnemon" / "SKILL.md";
  if (fs::exists(skill, ec)) {
    env.installed = true;
  }
  return env;
}

static Environment detect_trae(bool global) {
  Environment env;
  env.name = "trae";
  env.display = "Trae";
  fs::path global_dir = fs::path(home_dir()) / ".trae";
  std::string local_dir = ".trae";
  env.config_dir = global ? global_dir.string() : local_dir;

  std::string bin;
  if (look_path("trae", bin)) {
    env.detected = true;
    env.bin_path = bin;
    env.version = exec_version(bin);
  }
  std::error_code ec;
  if (fs::exists(global_dir, ec)) {
    env.detected = true;
  }
  fs::path skill = fs::path(env.config_dir) / "skills" / "mnemon" / "SKILL.md";
  if (fs::exists(skill, ec)) {
    env.installed = true;
  } else {
    fs::path hooks_path = fs::path(env.config_dir) / "hooks.json";
    try {
      nlohmann::json data = read_json_file(hooks_path);
      if (json_contains_mnemon(data)) {
        env.installed = true;
      }
    } catch (...) {
    }
  }
  return env;
}

static Environment detect_qoder(bool global) {
  Environment env;
  env.name = "qoder";
  env.display = "Qoder";
  fs::path global_dir = fs::path(home_dir()) / ".qoder";
  std::string local_dir = ".qoder";
  env.config_dir = global ? global_dir.string() : local_dir;

  std::string bin;
  if (look_path("qoder", bin)) {
    env.detected = true;
    env.bin_path = bin;
    env.version = exec_version(bin);
  }
  std::error_code ec;
  if (fs::exists(global_dir, ec)) {
    env.detected = true;
  }
  fs::path skill = fs::path(env.config_dir) / "skills" / "mnemon" / "SKILL.md";
  if (fs::exists(skill, ec)) {
    env.installed = true;
  } else {
    fs::path settings_path = fs::path(env.config_dir) / "settings.json";
    try {
      nlohmann::json data = read_json_file(settings_path);
      if (json_contains_mnemon(data)) {
        env.installed = true;
      }
    } catch (...) {
    }
  }
  return env;
}

static Environment detect_qoderwork() {
  Environment env;
  env.name = "qoderwork";
  env.display = "QoderWork";
  fs::path config_dir = fs::path(home_dir()) / ".qoderwork";
  env.config_dir = config_dir.string();

  std::string bin;
  if (look_path("qoderwork", bin)) {
    env.detected = true;
    env.bin_path = bin;
    env.version = exec_version(bin);
  }
  std::error_code ec;
  if (fs::exists(config_dir, ec)) {
    env.detected = true;
  }
  fs::path skill = config_dir / "skills" / "mnemon" / "SKILL.md";
  if (fs::exists(skill, ec)) {
    env.installed = true;
  } else {
    fs::path settings_path = config_dir / "settings.json";
    try {
      nlohmann::json data = read_json_file(settings_path);
      if (json_contains_mnemon(data)) {
        env.installed = true;
      }
    } catch (...) {
    }
  }
  return env;
}

static Environment detect_codebuddy(bool global) {
  Environment env;
  env.name = "codebuddy";
  env.display = "CodeBuddy";
  fs::path global_dir = fs::path(home_dir()) / ".codebuddy";
  std::string local_dir = ".codebuddy";
  env.config_dir = global ? global_dir.string() : local_dir;

  std::string bin;
  if (look_path("codebuddy", bin)) {
    env.detected = true;
    env.bin_path = bin;
    env.version = exec_version(bin);
  }
  std::error_code ec;
  if (fs::exists(global_dir, ec)) {
    env.detected = true;
  }
  fs::path skill = fs::path(env.config_dir) / "skills" / "mnemon" / "SKILL.md";
  if (fs::exists(skill, ec)) {
    env.installed = true;
  } else {
    fs::path settings_path = fs::path(env.config_dir) / "settings.json";
    try {
      nlohmann::json data = read_json_file(settings_path);
      if (json_contains_mnemon(data)) {
        env.installed = true;
      }
    } catch (...) {
    }
  }
  return env;
}

static Environment detect_workbuddy(bool global) {
  Environment env;
  env.name = "workbuddy";
  env.display = "WorkBuddy";
  fs::path global_dir = fs::path(home_dir()) / ".workbuddy";
  std::string local_dir = ".workbuddy";
  env.config_dir = global ? global_dir.string() : local_dir;

  std::string bin;
  if (look_path("workbuddy", bin)) {
    env.detected = true;
    env.bin_path = bin;
    env.version = exec_version(bin);
  }
  std::error_code ec;
  if (fs::exists(global_dir, ec)) {
    env.detected = true;
  }
  fs::path skill = fs::path(env.config_dir) / "skills" / "mnemon" / "SKILL.md";
  if (fs::exists(skill, ec)) {
    env.installed = true;
  } else {
    fs::path settings_path = fs::path(env.config_dir) / "settings.json";
    try {
      nlohmann::json data = read_json_file(settings_path);
      if (json_contains_mnemon(data)) {
        env.installed = true;
      }
    } catch (...) {
    }
  }
  return env;
}

static Environment detect_kimi() {
  Environment env;
  env.name = "kimi";
  env.display = "Kimi Code";
  fs::path config_dir = fs::path(home_dir()) / ".kimi-code";
  if (const char* env_home = std::getenv("KIMI_CODE_HOME")) {
    std::string trimmed(env_home);
    size_t b = trimmed.find_first_not_of(" \t\r\n");
    size_t e = trimmed.find_last_not_of(" \t\r\n");
    trimmed = (b == std::string::npos) ? "" : trimmed.substr(b, e - b + 1);
    if (!trimmed.empty()) {
      config_dir = trimmed;
    }
  }
  env.config_dir = config_dir.string();

  std::string bin;
  if (look_path("kimi", bin)) {
    env.detected = true;
    env.bin_path = bin;
    env.version = exec_version(bin);
  }
  std::error_code ec;
  if (fs::exists(config_dir, ec)) {
    env.detected = true;
  }
  fs::path skill = config_dir / "skills" / "mnemon" / "SKILL.md";
  if (fs::exists(skill, ec)) {
    env.installed = true;
  } else {
    std::ifstream in(config_dir / "config.toml");
    if (in) {
      std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      if (raw.find("mnemon") != std::string::npos) {
        env.installed = true;
      }
    }
  }
  return env;
}

static Environment detect_nanobot(bool global) {
  Environment env;
  env.name = "nanobot";
  env.display = "Nanobot";
  fs::path global_dir = fs::path(home_dir()) / ".nanobot" / "workspace";
  std::string local_dir = ".nanobot";
  env.config_dir = global ? global_dir.string() : local_dir;

  std::string bin;
  if (look_path("nanobot", bin)) {
    env.detected = true;
    env.bin_path = bin;
    env.version = exec_version(bin);
  }
  std::error_code ec;
  if (fs::exists(global_dir, ec)) {
    env.detected = true;
  }
  fs::path skill = fs::path(env.config_dir) / "skills" / "mnemon" / "SKILL.md";
  if (fs::exists(skill, ec)) {
    env.installed = true;
  }
  return env;
}

static Environment detect_pi(bool global) {
  Environment env;
  env.name = "pi";
  env.display = "Pi";
  fs::path global_dir = fs::path(home_dir()) / ".pi" / "agent";
  std::string local_dir = ".pi";
  env.config_dir = global ? global_dir.string() : local_dir;

  std::string bin;
  if (look_path("pi", bin)) {
    env.detected = true;
    env.bin_path = bin;
    env.version = exec_version(bin);
  }
  std::error_code ec;
  if (fs::exists(global_dir, ec)) {
    env.detected = true;
  }
  fs::path skill = fs::path(env.config_dir) / "skills" / "mnemon" / "SKILL.md";
  if (fs::exists(skill, ec)) {
    env.installed = true;
  }
  return env;
}

static Environment detect_hermes() {
  Environment env;
  env.name = "hermes";
  env.display = "Hermes Agent";
  fs::path global_dir = fs::path(home_dir()) / ".hermes";
  env.config_dir = global_dir.string();

  std::string bin;
  if (look_path("hermes", bin)) {
    env.detected = true;
    env.bin_path = bin;
    env.version = exec_version(bin);
  }
  std::error_code ec;
  if (fs::exists(global_dir, ec)) {
    env.detected = true;
  }
  fs::path skill = global_dir / "skills" / "mnemon" / "SKILL.md";
  if (fs::exists(skill, ec)) {
    env.installed = true;
  }
  return env;
}

static std::vector<Environment> detect_environments(bool global) {
  return {detect_claude(global),     detect_codex(global),     detect_cursor(global),    detect_trae(global),
          detect_qoder(global),      detect_qoderwork(),       detect_codebuddy(global), detect_workbuddy(global),
          detect_kimi(),             detect_openclaw(global),  detect_nanobot(global),   detect_pi(global),
          detect_hermes()};
}

// --- install pieces ---

static fs::path write_prompt_files() {
  fs::path prompt_dir;
  const char* data_dir_env = std::getenv("MNEMON_DATA_DIR");
  if (data_dir_env && data_dir_env[0] != '\0') {
    prompt_dir = fs::path(data_dir_env) / "prompt";
  } else {
    prompt_dir = fs::path(home_dir()) / ".mnemon" / "prompt";
  }
  fs::create_directories(prompt_dir);
  write_bytes(prompt_dir / "guide.md", mnemon::embedded::claude_guide_md(), 0644);
  write_bytes(prompt_dir / "skill.md", mnemon::embedded::claude_SKILL_md(), 0644);
  return prompt_dir;
}

static fs::path claude_write_skill(const std::string& config_dir) {
  fs::path skill_path = fs::path(config_dir) / "skills" / "mnemon" / "SKILL.md";
  write_bytes(skill_path, mnemon::embedded::claude_SKILL_md(), 0644);
  return skill_path;
}

static fs::path claude_write_hook(const std::string& config_dir, const std::string& filename, std::string_view content) {
  fs::path hook_path = fs::path(config_dir) / "hooks" / "mnemon" / filename;
  write_bytes(hook_path, content, 0755);
  return hook_path;
}

// Directory Claude Code treats as user-global configuration: $CLAUDE_CONFIG_DIR
// when set, otherwise ~/.claude.
static std::string user_claude_config_dir() {
  if (const char* dir = std::getenv("CLAUDE_CONFIG_DIR"); dir && *dir) {
    return dir;
  }
  std::string home = home_dir();
  if (home.empty()) {
    return "";
  }
  return (fs::path(home) / ".claude").string();
}

// Symlink-resolved absolute form of p, falling back to the lexical absolute
// path when p does not (yet) exist — mirrors Go's filepath.EvalSymlinks fallback.
static fs::path canonical_path(const fs::path& p) {
  std::error_code ec;
  fs::path abs = fs::absolute(p, ec);
  if (ec) {
    return p;
  }
  fs::path resolved = fs::canonical(abs, ec);
  return ec ? abs : resolved;
}

// Reports whether a project-local config dir is in fact Claude Code's
// user-global config dir — the degenerate case of running a project-local
// setup with cwd == $HOME, where "./.claude" IS "~/.claude". Relative hook
// commands written into that file load for every session on the machine but
// only resolve when the session's working directory is $HOME; the user-global
// file's contract is absolute paths. Both sides are resolved through symlinks
// before comparison.
static bool collides_with_user_config(const std::string& config_dir) {
  std::string user_dir = user_claude_config_dir();
  if (user_dir.empty()) {
    return false;
  }
  return canonical_path(config_dir) == canonical_path(user_dir);
}

static fs::path claude_register_hooks(const std::string& config_dir, const HookSelection& sel) {
  fs::path hooks_dir = fs::path(config_dir) / "hooks" / "mnemon";
  // When the project-local config dir collides with the user-global one (setup
  // run from $HOME), hook commands are written as absolute paths so they honor
  // the global file's contract and resolve from any session directory.
  if (collides_with_user_config(config_dir)) {
    hooks_dir = fs::absolute(hooks_dir);
    std::cout << "  Note: this project config dir is Claude Code's user-global config (" << user_claude_config_dir()
              << ");\n"
              << "        writing absolute hook paths so hooks resolve from any directory.\n"
              << "        Use --global to make a user-wide install explicit.\n";
  }
  fs::path settings_path = fs::path(config_dir) / "settings.json";
  nlohmann::json data = read_json_file(settings_path);
  add_claude_hooks_selective(data, hooks_dir.string(), sel);
  write_json_file(settings_path, data);
  return settings_path;
}

static fs::path openclaw_write_skill(const std::string& config_dir) {
  fs::path p = fs::path(config_dir) / "skills" / "mnemon" / "SKILL.md";
  write_bytes(p, mnemon::embedded::openclaw_SKILL_md(), 0644);
  return p;
}

static fs::path openclaw_write_hook(const std::string& config_dir) {
  fs::path hook_dir = fs::path(config_dir) / "hooks" / "mnemon-prime";
  write_bytes(hook_dir / "HOOK.md", mnemon::embedded::openclaw_hooks_mnemon_prime_HOOK_md(), 0644);
  write_bytes(hook_dir / "handler.js", mnemon::embedded::openclaw_hooks_mnemon_prime_handler_js(), 0644);
  return hook_dir;
}

static fs::path openclaw_write_plugin(const std::string& config_dir, const char* ver) {
  fs::path plugin_dir = fs::path(config_dir) / "extensions" / "mnemon";
  fs::create_directories(plugin_dir);

  nlohmann::json manifest = nlohmann::json::parse(std::string(mnemon::embedded::openclaw_plugin_openclaw_plugin_json()));
  if (ver && std::string(ver) != "dev" && std::strlen(ver) > 0) {
    manifest["version"] = ver;
  }
  std::string manifest_str = manifest.dump(2);
  manifest_str.push_back('\n');

  write_bytes(plugin_dir / "package.json", mnemon::embedded::openclaw_plugin_package_json(), 0644);
  write_bytes(plugin_dir / "openclaw.plugin.json", manifest_str, 0644);
  write_bytes(plugin_dir / "index.js", mnemon::embedded::openclaw_plugin_index_js(), 0644);
  return plugin_dir;
}

static fs::path openclaw_register_plugin(const std::string& config_dir, const HookSelection& sel) {
  fs::path cfg_path = fs::path(config_dir) / "openclaw.json";
  nlohmann::json cfg;
  std::error_code ec;
  if (fs::exists(cfg_path, ec)) {
    std::ifstream in(cfg_path);
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    cfg = nlohmann::json::parse(raw);
  } else {
    cfg = nlohmann::json::object();
  }
  if (!cfg.contains("plugins") || !cfg["plugins"].is_object()) {
    cfg["plugins"] = nlohmann::json::object();
  }
  if (!cfg["plugins"].contains("entries") || !cfg["plugins"]["entries"].is_object()) {
    cfg["plugins"]["entries"] = nlohmann::json::object();
  }
  cfg["plugins"]["entries"]["mnemon"] = {{"enabled", true},
                                           {"config",
                                            {{"remind", sel.remind}, {"nudge", sel.nudge}, {"compact", sel.compact}}}};
  std::string out = cfg.dump(2);
  out.push_back('\n');
  fs::create_directories(cfg_path.parent_path());
  fs::path tmp = cfg_path;
  tmp += ".tmp";
  {
    std::ofstream o(tmp, std::ios::trunc);
    o << out;
  }
  fs::rename(tmp, cfg_path);
  chmod_path(cfg_path, 0600);
  return cfg_path;
}

static void init_default_store(const std::string& data_dir) {
  if (!paths::migrate_if_needed(data_dir, false)) {
    std::cerr << "  Warning: migration failed\n";
  }
  if (!paths::store_exists(data_dir, "default")) {
    std::string dir = paths::store_dir(data_dir, "default");
    auto db = Database::open_readwrite(dir);
    (void)db;
    std::cout << "  Initialized default store at " << dir << "\n";
  }
}

// --- eject ---

static bool eject_memory_block(const fs::path& file_path) {
  constexpr std::string_view kStart = "<!-- mnemon:start -->";
  constexpr std::string_view kEnd = "<!-- mnemon:end -->";
  std::error_code ec;
  if (!fs::exists(file_path, ec)) {
    return false;
  }
  std::ifstream in(file_path);
  std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  size_t start_idx = s.find(kStart);
  if (start_idx == std::string::npos) {
    return false;
  }
  // Search for end marker AFTER the start marker to avoid false matches
  size_t end_rel = s.find(kEnd, start_idx + kStart.size());
  if (end_rel == std::string::npos) {
    return false;
  }
  size_t end_idx = end_rel + kEnd.size();

  bool removed_leading = false;
  bool removed_trailing = false;
  if (start_idx > 0 && s[start_idx - 1] == '\n') {
    --start_idx;
    removed_leading = true;
  }
  if (end_idx < s.size() && s[end_idx] == '\n') {
    ++end_idx;
    removed_trailing = true;
  }
  std::string result = s.substr(0, start_idx) + s.substr(end_idx);
  if (removed_leading && removed_trailing && start_idx > 0 && end_idx < s.size()) {
    result = s.substr(0, start_idx) + "\n" + s.substr(end_idx);
  }
  // Collapse triple newlines (can appear when block was surrounded by blank lines)
  while (result.find("\n\n\n") != std::string::npos) {
    size_t p = result.find("\n\n\n");
    result.replace(p, 3, "\n\n");
  }
  // trim space like Go strings.TrimSpace
  size_t a = result.find_first_not_of(" \t\n\r");
  if (a == std::string::npos) {
    fs::remove(file_path, ec);
    return true;
  }
  size_t b = result.find_last_not_of(" \t\n\r");
  result = result.substr(a, b - a + 1);
  result.push_back('\n');
  // Write directly (file already exists; don't attempt to create parent dirs)
  std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("write " + file_path.string());
  }
  out.write(result.data(), static_cast<std::streamsize>(result.size()));
  chmod_path(file_path, 0644);
  return true;
}

static void eject_markdown(const std::string& file_path, const std::string& prompt, bool yes) {
  if (yes) {
    try {
      if (eject_memory_block(file_path)) {
        std::cout << "  Memory guidance removed from " << file_path << "\n";
      }
    } catch (const std::exception& e) {
      std::cerr << "  Warning: could not clean " << file_path << ": " << e.what() << "\n";
    }
  } else if (is_tty_in()) {
    if (confirm(prompt, true)) {
      try {
        if (eject_memory_block(file_path)) {
          std::cout << "  Memory guidance removed from " << file_path << "\n";
        }
      } catch (const std::exception& e) {
        std::cerr << "  Warning: could not clean " << file_path << ": " << e.what() << "\n";
      }
    }
  }
}

static void eject_local_markdown(bool yes) {
  eject_markdown("CLAUDE.md", "Remove memory guidance from ./CLAUDE.md?", yes);
  eject_markdown("AGENTS.md", "Remove memory guidance from ./AGENTS.md?", yes);
}

static int claude_eject(const std::string& config_dir, bool yes) {
  std::cout << "\nRemoving Claude Code integration (" << config_dir << ")...\n";
  int errs = 0;
  fs::path hooks_dir = fs::path(config_dir) / "hooks" / "mnemon";
  std::error_code ec;
  fs::remove_all(hooks_dir, ec);
  if (ec) {
    status_error("Hooks", ec.message());
    ++errs;
  } else {
    status_ok("Hooks", hooks_dir.string() + " removed");
  }
  remove_if_empty_dir(fs::path(config_dir) / "hooks");

  fs::path settings_path = fs::path(config_dir) / "settings.json";
  try {
    nlohmann::json data = read_json_file(settings_path);
    remove_claude_hooks(data);
    write_or_remove_json_file(settings_path, data);
    status_ok("Settings", settings_path.string() + " cleaned");
  } catch (const std::exception& e) {
    status_error("Settings", e.what());
    ++errs;
  }

  fs::path skill_dir = fs::path(config_dir) / "skills" / "mnemon";
  ec.clear();
  fs::remove_all(skill_dir, ec);
  if (ec) {
    status_error("Skill", ec.message());
    ++errs;
  } else {
    status_ok("Skill", skill_dir.string() + " removed");
  }
  remove_if_empty_dir(fs::path(config_dir) / "skills");
  remove_if_empty_dir(config_dir);

  eject_markdown("CLAUDE.md", "Remove memory guidance from ./CLAUDE.md?", yes);
  return errs;
}

// remove_openclaw_plugin_entry removes the "mnemon" entry from openclaw.json's
// plugins.entries. Deletes the file entirely if it becomes empty. Returns an
// error string on failure, empty on success (including file-not-found).
static std::string remove_openclaw_plugin_entry(const fs::path& cfg_path) {
  std::error_code ec;
  if (!fs::exists(cfg_path, ec)) {
    return "";
  }
  std::ifstream in(cfg_path);
  if (!in) {
    return "open " + cfg_path.string() + ": read error";
  }
  std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  nlohmann::json cfg;
  try {
    cfg = nlohmann::json::parse(raw);
  } catch (const std::exception& e) {
    return "parse openclaw.json: " + std::string(e.what());
  }

  if (!cfg.contains("plugins") || !cfg["plugins"].is_object()) {
    return "";
  }
  auto& plugins = cfg["plugins"];
  if (!plugins.contains("entries") || !plugins["entries"].is_object()) {
    return "";
  }
  auto& entries = plugins["entries"];
  if (!entries.contains("mnemon")) {
    return "";
  }
  entries.erase("mnemon");
  if (entries.empty()) {
    plugins.erase("entries");
  }
  if (plugins.empty()) {
    cfg.erase("plugins");
  }

  if (cfg.empty()) {
    ec.clear();
    fs::remove(cfg_path, ec);
    if (ec) {
      return "remove " + cfg_path.string() + ": " + ec.message();
    }
    return "";
  }

  std::string out = cfg.dump(2);
  out.push_back('\n');
  fs::path tmp = cfg_path;
  tmp += ".tmp";
  {
    std::ofstream o(tmp, std::ios::trunc);
    if (!o) {
      return "write " + tmp.string() + ": open error";
    }
    o << out;
  }
  ec.clear();
  fs::rename(tmp, cfg_path, ec);
  if (ec) {
    return "rename " + tmp.string() + ": " + ec.message();
  }
  chmod_path(cfg_path, 0600);
  return "";
}

static int openclaw_eject(const std::string& config_dir, bool yes) {
  std::cout << "\nRemoving OpenClaw integration (" << config_dir << ")...\n";
  int errs = 0;
  std::error_code ec;
  struct T {
    const char* label;
    fs::path path;
  };
  std::vector<T> targets = {{"Skill", fs::path(config_dir) / "skills" / "mnemon"},
                            {"Hook", fs::path(config_dir) / "hooks" / "mnemon-prime"},
                            {"Plugin", fs::path(config_dir) / "extensions" / "mnemon"}};
  for (const auto& t : targets) {
    ec.clear();
    fs::remove_all(t.path, ec);
    if (ec) {
      status_error(t.label, ec.message());
      ++errs;
    } else {
      status_ok(t.label, t.path.string() + " removed");
    }
  }
  remove_if_empty_dir(fs::path(config_dir) / "skills");
  remove_if_empty_dir(fs::path(config_dir) / "hooks");
  remove_if_empty_dir(fs::path(config_dir) / "extensions");

  fs::path cfg_path = fs::path(config_dir) / "openclaw.json";
  if (std::string err = remove_openclaw_plugin_entry(cfg_path); !err.empty()) {
    status_error("Config", err);
    ++errs;
  }

  remove_if_empty_dir(config_dir);
  eject_markdown("AGENTS.md", "Remove memory guidance from ./AGENTS.md?", yes);
  return errs;
}

// --- nanobot ---

static fs::path nanobot_write_skill(const std::string& config_dir) {
  fs::path skill_dir = fs::path(config_dir) / "skills" / "mnemon";
  fs::create_directories(skill_dir);
  fs::path p = skill_dir / "SKILL.md";
  write_bytes(p, mnemon::embedded::nanobot_SKILL_md(), 0644);
  return p;
}

static int nanobot_eject(const std::string& config_dir) {
  int errs = 0;
  std::cout << "\nRemoving Nanobot integration (" << config_dir << ")...\n";
  fs::path skill_dir = fs::path(config_dir) / "skills" / "mnemon";
  std::error_code ec;
  fs::remove_all(skill_dir, ec);
  if (ec) {
    status_error("Skill", "remove failed: " + ec.message());
    ++errs;
  } else {
    status_ok("Skill", skill_dir.string() + " removed");
  }
  remove_if_empty_dir((fs::path(config_dir) / "skills").string());
  remove_if_empty_dir(config_dir);
  return errs;
}

static bool install_nanobot(Environment env, bool global, bool setup_yes) {
  std::string config_dir = env.config_dir;
  if (!global && !setup_yes && is_tty_in()) {
    std::string local_dir = ".nanobot";
    std::string global_dir = (fs::path(home_dir()) / ".nanobot" / "workspace").string();
    size_t idx = select_one("Install scope", {
        "Global -- all projects (" + global_dir + "/)",
        "Local  -- this project only (" + local_dir + "/)",
    }, 0);
    config_dir = (idx == 1) ? local_dir : global_dir;
  }
  std::cout << "\nSetting up Nanobot (" << config_dir << ")...\n";
  std::cout << "\n[1/2] Skill\n";
  try {
    auto p = nanobot_write_skill(config_dir);
    status_ok("Skill", p.string());
  } catch (const std::exception& e) {
    status_error("Skill", e.what());
    return false;
  }
  std::cout << "\n[2/2] Prompts\n";
  std::string prompt_path;
  try {
    auto p = write_prompt_files();
    status_ok("Prompts", p.string());
    prompt_path = p.string();
  } catch (const std::exception& e) {
    status_error("Prompts", e.what());
    return false;
  }
  std::cout << "\nSetup complete!\n";
  std::cout << "  Skill   " << config_dir << "/skills/mnemon/SKILL.md\n";
  std::cout << "  Prompts " << prompt_path << "/ (guide.md, skill.md)\n";
  std::cout << "\nRestart Nanobot to activate the mnemon skill.\n";
  std::cout << "Edit " << prompt_path << "/guide.md to customize behavior.\n";
  std::cout << "Run 'mnemon setup --eject' to remove.\n";
  return true;
}

// --- hermes ---

static fs::path hermes_write_skill(const std::string& config_dir) {
  fs::path skill_dir = fs::path(config_dir) / "skills" / "mnemon";
  fs::create_directories(skill_dir);
  fs::path p = skill_dir / "SKILL.md";
  write_bytes(p, mnemon::embedded::hermes_SKILL_md(), 0644);
  return p;
}

static fs::path hermes_write_hook(const std::string& config_dir, const std::string& filename,
                                   std::string_view content) {
  fs::path hooks_dir = fs::path(config_dir) / "agent-hooks" / "mnemon";
  fs::create_directories(hooks_dir);
  fs::path p = hooks_dir / filename;
  write_bytes(p, content, 0755);
  return p;
}

// config.yaml is a real YAML document: read -> mutate -> re-marshal, mirroring the Go
// reference (internal/setup/hermes.go), not line-based string surgery.

// Read config.yaml into a YAML map. Missing or blank files yield an empty map, matching the
// Go reference's readYAMLFile.
static yaml::Value yaml_read_file(const fs::path& path) {
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    return yaml::Value::make_map();
  }
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("read " + path.string());
  }
  std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (raw.find_first_not_of(" \t\r\n") == std::string::npos) {
    return yaml::Value::make_map();
  }
  yaml::Value v = yaml::parse(raw);
  if (!v.is_map()) {
    return yaml::Value::make_map();
  }
  return v;
}

// Marshal + atomic write (temp file then rename), preserving the existing file mode and
// defaulting to 0600 — mirrors the Go reference's writeYAMLFile.
static void yaml_write_file(const fs::path& path, const yaml::Value& data) {
  std::string body = yaml::marshal(data);
  fs::create_directories(path.parent_path());
  mode_t mode = 0600;
  struct stat st {};
  if (::stat(path.string().c_str(), &st) == 0) {
    mode = st.st_mode & 0777;
  }
  fs::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      throw std::runtime_error("write tmp " + tmp.string());
    }
    out << body;
  }
  fs::rename(tmp, path);
  chmod_path(path, mode);
}

// Write the document, or remove the file when the resulting map is empty — mirrors the Go
// reference's writeOrRemoveYAMLFile.
static void yaml_write_or_remove(const fs::path& path, const yaml::Value& data) {
  if (!data.is_map() || data.map.empty()) {
    std::error_code ec;
    fs::remove(path, ec);
    return;
  }
  yaml_write_file(path, data);
}

// Drop every mnemon-owned hook entry (matched by the "mnemon" substring anywhere in the
// entry, like the Go reference's containsMnemon), pruning emptied events and the hooks block.
static void remove_hermes_hooks(yaml::Value& data) {
  auto it = data.map.find("hooks");
  if (it == data.map.end() || !it->second.is_map()) {
    return;
  }
  yaml::Value& hooks = it->second;
  static const char* events[] = {"on_session_start", "pre_llm_call", "post_llm_call", "on_session_finalize"};
  for (const char* ev : events) {
    auto eit = hooks.map.find(ev);
    if (eit == hooks.map.end() || !eit->second.is_seq()) {
      continue;
    }
    yaml::Value kept = yaml::Value::make_seq();
    for (auto& entry : eit->second.seq) {
      if (!yaml::contains_mnemon(entry)) {
        kept.seq.push_back(entry);
      }
    }
    if (kept.seq.empty()) {
      hooks.map.erase(eit);
    } else {
      eit->second = std::move(kept);
    }
  }
  if (hooks.map.empty()) {
    data.map.erase("hooks");
  }
}

// Remove any prior mnemon hooks, then append the selected ones — mirrors addHermesHooks.
static void add_hermes_hooks(yaml::Value& data, const fs::path& hooks_dir, const HookSelection& sel) {
  remove_hermes_hooks(data);
  auto hit = data.map.find("hooks");
  if (hit == data.map.end() || !hit->second.is_map()) {
    data.map["hooks"] = yaml::Value::make_map();
  }
  yaml::Value& hooks = data.map["hooks"];

  auto append = [&](const char* event, const char* file, long long timeout) {
    yaml::Value entry = yaml::Value::make_map();
    entry.map["command"] = yaml::Value::make_string((hooks_dir / file).string());
    entry.map["timeout"] = yaml::Value::make_int(timeout);
    yaml::Value& arr = hooks.map[event];
    if (!arr.is_seq()) {
      arr = yaml::Value::make_seq();
    }
    arr.seq.push_back(std::move(entry));
  };

  append("on_session_start", "prime.sh", 10);
  if (sel.remind) {
    append("pre_llm_call", "remind.sh", 10);
  }
  if (sel.nudge) {
    append("post_llm_call", "nudge.sh", 10);
  }
  if (sel.compact) {
    append("on_session_finalize", "compact.sh", 30);
  }
}

static fs::path hermes_register_hooks(const std::string& config_dir, const HookSelection& sel) {
  fs::path hooks_dir = fs::path(config_dir) / "agent-hooks" / "mnemon";
  fs::path abs_hooks_dir = fs::absolute(hooks_dir);
  fs::path cfg = fs::path(config_dir) / "config.yaml";

  yaml::Value data = yaml_read_file(cfg);
  add_hermes_hooks(data, abs_hooks_dir, sel);
  yaml_write_file(cfg, data);
  return cfg;
}

static int hermes_eject(const std::string& config_dir) {
  int errs = 0;
  std::cout << "\nRemoving Hermes Agent integration (" << config_dir << ")...\n";
  std::error_code ec;

  fs::path hooks_dir = fs::path(config_dir) / "agent-hooks" / "mnemon";
  fs::remove_all(hooks_dir, ec);
  if (ec) {
    status_error("Hooks", "remove failed: " + ec.message());
    ++errs;
  } else {
    status_ok("Hooks", hooks_dir.string() + " removed");
  }
  remove_if_empty_dir((fs::path(config_dir) / "agent-hooks").string());

  // Remove mnemon entries from config.yaml, preserving unrelated config. Missing files read
  // as an empty map and are removed (no-op) — mirrors the Go reference's HermesEject.
  fs::path cfg = fs::path(config_dir) / "config.yaml";
  try {
    yaml::Value data = yaml_read_file(cfg);
    remove_hermes_hooks(data);
    yaml_write_or_remove(cfg, data);
    status_ok("Config", cfg.string() + " cleaned");
  } catch (const std::exception& e) {
    status_error("Config", e.what());
    ++errs;
  }

  fs::path skill_dir = fs::path(config_dir) / "skills" / "mnemon";
  fs::remove_all(skill_dir, ec);
  if (ec) {
    status_error("Skill", "remove failed: " + ec.message());
    ++errs;
  } else {
    status_ok("Skill", skill_dir.string() + " removed");
  }
  remove_if_empty_dir((fs::path(config_dir) / "skills").string());

  fs::path state_dir = fs::path(config_dir) / "mnemon";
  fs::remove_all(state_dir, ec);
  status_ok("State", state_dir.string() + " removed");

  remove_if_empty_dir(config_dir);
  return errs;
}

static bool install_hermes(Environment env, bool setup_yes, const RunOptions& opt) {
  std::string config_dir = env.config_dir;
  std::cout << "\nSetting up Hermes Agent (" << config_dir << ")...\n";

  std::cout << "\n[1/4] Skill\n";
  try {
    auto p = hermes_write_skill(config_dir);
    status_ok("Skill", p.string());
  } catch (const std::exception& e) {
    status_error("Skill", e.what());
    return false;
  }

  std::cout << "\n[2/4] Prompts\n";
  std::string prompt_path;
  try {
    auto p = write_prompt_files();
    status_ok("Prompts", p.string());
    prompt_path = p.string();
  } catch (const std::exception& e) {
    status_error("Prompts", e.what());
    return false;
  }

  std::cout << "\n[3/4] Hooks\n";
  HookSelection sel = {true, true, false};
  if (!setup_yes && is_tty_in()) {
    sel = select_multi("Select hooks to enable", {
        "Remind  — recall relevant memories before each LLM call (recommended)",
        "Nudge   — queue remember guidance after each LLM response",
        "Compact — queue preservation guidance on session finalization",
    }, sel);
  }
  try {
    auto p = hermes_write_hook(config_dir, "prime.sh", mnemon::embedded::hermes_prime_sh());
    status_ok("Hook: prime", p.string());
  } catch (const std::exception& e) {
    status_error("Hook: prime", e.what());
    return false;
  }
  if (sel.remind) {
    try {
      auto p = hermes_write_hook(config_dir, "remind.sh", mnemon::embedded::hermes_remind_sh());
      status_ok("Hook: remind", p.string());
    } catch (const std::exception& e) {
      status_error("Hook: remind", e.what());
      return false;
    }
  }
  if (sel.nudge) {
    try {
      auto p = hermes_write_hook(config_dir, "nudge.sh", mnemon::embedded::hermes_nudge_sh());
      status_ok("Hook: nudge", p.string());
    } catch (const std::exception& e) {
      status_error("Hook: nudge", e.what());
      return false;
    }
  }
  if (sel.compact) {
    try {
      auto p = hermes_write_hook(config_dir, "compact.sh", mnemon::embedded::hermes_compact_sh());
      status_ok("Hook: compact", p.string());
    } catch (const std::exception& e) {
      status_error("Hook: compact", e.what());
      return false;
    }
  }

  std::cout << "\n[4/4] Config\n";
  try {
    auto p = hermes_register_hooks(config_dir, sel);
    status_updated("Config", p.string());
  } catch (const std::exception& e) {
    status_error("Config", e.what());
    return false;
  }

  std::vector<std::string> hook_names = {"prime"};
  if (sel.remind) hook_names.push_back("remind");
  if (sel.nudge)  hook_names.push_back("nudge");
  if (sel.compact) hook_names.push_back("compact");
  std::string hooks_str;
  for (size_t i = 0; i < hook_names.size(); ++i) {
    if (i > 0) hooks_str += ", ";
    hooks_str += hook_names[i];
  }

  std::cout << "\nSetup complete!\n";
  std::cout << "  Skill   " << config_dir << "/skills/mnemon/SKILL.md\n";
  std::cout << "  Hooks   " << config_dir << "/config.yaml (" << hooks_str << ")\n";
  std::cout << "  Prompts " << prompt_path << "/ (guide.md, skill.md)\n";
  std::cout << "\nStart a new Hermes session to activate.\n";
  std::cout << "Hermes may prompt once to approve the installed shell hooks.\n";
  std::cout << "Run 'mnemon setup --eject --target hermes' to remove.\n";
  return true;
}

// --- pi ---

static fs::path pi_write_skill(const std::string& config_dir) {
  fs::path skill_dir = fs::path(config_dir) / "skills" / "mnemon";
  fs::create_directories(skill_dir);
  fs::path p = skill_dir / "SKILL.md";
  write_bytes(p, mnemon::embedded::pi_SKILL_md(), 0644);
  return p;
}

static fs::path pi_write_extension(const std::string& config_dir) {
  fs::path ext_dir = fs::path(config_dir) / "extensions";
  fs::create_directories(ext_dir);
  fs::path p = ext_dir / "mnemon.ts";
  write_bytes(p, mnemon::embedded::pi_mnemon_ts(), 0644);
  return p;
}

static int pi_eject(const std::string& config_dir) {
  int errs = 0;
  std::cout << "\nRemoving Pi integration (" << config_dir << ")...\n";
  std::error_code ec;
  fs::path ext = fs::path(config_dir) / "extensions" / "mnemon.ts";
  fs::remove(ext, ec);
  if (ec) {
    status_error("Extension", "remove failed: " + ec.message());
    ++errs;
  } else {
    status_ok("Extension", ext.string() + " removed");
  }
  fs::path skill_dir = fs::path(config_dir) / "skills" / "mnemon";
  fs::remove_all(skill_dir, ec);
  if (ec) {
    status_error("Skill", "remove failed: " + ec.message());
    ++errs;
  } else {
    status_ok("Skill", skill_dir.string() + " removed");
  }
  remove_if_empty_dir((fs::path(config_dir) / "extensions").string());
  remove_if_empty_dir((fs::path(config_dir) / "skills").string());
  remove_if_empty_dir(config_dir);
  return errs;
}

static bool install_pi(Environment env, bool global, bool setup_yes) {
  std::string config_dir = env.config_dir;
  if (!global && !setup_yes && is_tty_in()) {
    std::string local_dir = ".pi";
    std::string global_dir = (fs::path(home_dir()) / ".pi" / "agent").string();
    size_t idx = select_one("Install scope", {
        "Local — this project only (" + local_dir + "/)",
        "Global — all projects (" + global_dir + "/)",
    }, 0);
    config_dir = (idx == 1) ? global_dir : local_dir;
  }
  std::cout << "\nSetting up Pi (" << config_dir << ")...\n";
  std::cout << "\n[1/3] Skill\n";
  try {
    auto p = pi_write_skill(config_dir);
    status_ok("Skill", p.string());
  } catch (const std::exception& e) {
    status_error("Skill", e.what());
    return false;
  }
  std::cout << "\n[2/3] Prompts\n";
  std::string prompt_path;
  try {
    auto p = write_prompt_files();
    status_ok("Prompts", p.string());
    prompt_path = p.string();
  } catch (const std::exception& e) {
    status_error("Prompts", e.what());
    return false;
  }
  std::cout << "\n[3/3] Extension\n";
  try {
    auto p = pi_write_extension(config_dir);
    status_ok("Extension", p.string());
  } catch (const std::exception& e) {
    status_error("Extension", e.what());
    return false;
  }
  std::cout << "\nSetup complete!\n";
  std::cout << "  Skill     " << config_dir << "/skills/mnemon/SKILL.md\n";
  std::cout << "  Extension " << config_dir
            << "/extensions/mnemon.ts (resources_discover, before_agent_start, agent_end, session_before_compact)\n";
  std::cout << "  Prompts   " << prompt_path << "/ (guide.md, skill.md)\n";
  std::cout << "\nStart a new Pi session or run /reload to activate.\n";
  std::cout << "Run 'mnemon setup --eject --target pi' to remove.\n";
  return true;
}

// --- codex ---

static void remove_codex_hooks(nlohmann::json& data) {
  if (!data.contains("hooks") || !data["hooks"].is_object()) {
    return;
  }
  auto& hooks = data["hooks"];
  static const char* keys[] = {"SessionStart", "UserPromptSubmit", "Stop"};
  for (const char* key : keys) {
    if (!hooks.contains(key)) {
      continue;
    }
    nlohmann::json filtered = filter_hook_array(hooks[key]);
    if (filtered.empty()) {
      hooks.erase(key);
    } else {
      hooks[key] = filtered;
    }
  }
  if (hooks.empty()) {
    data.erase("hooks");
  }
}

static void add_codex_hooks(nlohmann::json& data, const std::string& hooks_dir) {
  remove_codex_hooks(data);
  if (!data.contains("hooks") || !data["hooks"].is_object()) {
    data["hooks"] = nlohmann::json::object();
  }
  auto& hooks = data["hooks"];
  fs::path hd = hooks_dir;

  auto prime_entry = nlohmann::json::object();
  prime_entry["matcher"] = "startup|resume|clear";
  prime_entry["hooks"] = nlohmann::json::array(
      {{{"type", "command"}, {"command", (hd / "prime.sh").string()}, {"timeout", 30}, {"statusMessage", "Loading Mnemon context"}}});
  if (!hooks.contains("SessionStart")) {
    hooks["SessionStart"] = nlohmann::json::array();
  }
  hooks["SessionStart"].push_back(prime_entry);

  auto remind_entry = nlohmann::json::object();
  remind_entry["hooks"] = nlohmann::json::array(
      {{{"type", "command"}, {"command", (hd / "user_prompt.sh").string()}, {"timeout", 30}, {"statusMessage", "Checking Mnemon recall guidance"}}});
  if (!hooks.contains("UserPromptSubmit")) {
    hooks["UserPromptSubmit"] = nlohmann::json::array();
  }
  hooks["UserPromptSubmit"].push_back(remind_entry);

  auto stop_entry = nlohmann::json::object();
  stop_entry["hooks"] = nlohmann::json::array(
      {{{"type", "command"}, {"command", (hd / "stop.sh").string()}, {"timeout", 30}, {"statusMessage", "Checking Mnemon writeback guidance"}}});
  if (!hooks.contains("Stop")) {
    hooks["Stop"] = nlohmann::json::array();
  }
  hooks["Stop"].push_back(stop_entry);
}

static fs::path codex_write_skill(const std::string& config_dir) {
  fs::path skill_dir = fs::path(config_dir) / "skills" / "mnemon";
  fs::create_directories(skill_dir);
  fs::path p = skill_dir / "SKILL.md";
  write_bytes(p, mnemon::embedded::codex_SKILL_md(), 0644);
  return p;
}

static fs::path codex_write_hook(const std::string& config_dir, const std::string& filename, std::string_view content) {
  fs::path hook_path = fs::path(config_dir) / "hooks" / "mnemon" / filename;
  write_bytes(hook_path, content, 0755);
  return hook_path;
}

static fs::path codex_register_hooks(const std::string& config_dir) {
  fs::path hooks_dir = fs::path(config_dir) / "hooks" / "mnemon";
  fs::path abs_hooks_dir = fs::absolute(hooks_dir);
  fs::path hooks_path = fs::path(config_dir) / "hooks.json";
  nlohmann::json data = read_json_file(hooks_path);
  add_codex_hooks(data, abs_hooks_dir.string());
  write_json_file(hooks_path, data);
  return hooks_path;
}

static int codex_eject(const std::string& config_dir) {
  int errs = 0;
  std::cout << "\nRemoving Codex integration (" << config_dir << ")...\n";

  fs::path hooks_dir = fs::path(config_dir) / "hooks" / "mnemon";
  std::error_code ec;
  fs::remove_all(hooks_dir, ec);
  if (ec) {
    status_error("Hooks", ec.message());
    ++errs;
  } else {
    status_ok("Hooks", hooks_dir.string() + " removed");
  }
  remove_if_empty_dir(fs::path(config_dir) / "hooks");

  fs::path hooks_path = fs::path(config_dir) / "hooks.json";
  try {
    nlohmann::json data = read_json_file(hooks_path);
    remove_codex_hooks(data);
    write_or_remove_json_file(hooks_path, data);
    status_ok("Hooks config", hooks_path.string() + " cleaned");
  } catch (const std::exception& e) {
    status_error("Hooks config", e.what());
    ++errs;
  }

  fs::path skill_dir = fs::path(config_dir) / "skills" / "mnemon";
  ec.clear();
  fs::remove_all(skill_dir, ec);
  if (ec) {
    status_error("Skill", ec.message());
    ++errs;
  } else {
    status_ok("Skill", skill_dir.string() + " removed");
  }
  remove_if_empty_dir(fs::path(config_dir) / "skills");
  remove_if_empty_dir(config_dir);
  return errs;
}

static bool install_codex(Environment env, bool global, bool setup_yes) {
  std::string config_dir = env.config_dir;
  if (!global && !setup_yes && is_tty_in()) {
    std::string local_dir = ".codex";
    std::string global_dir = (fs::path(home_dir()) / ".codex").string();
    size_t idx = select_one("Install scope", {
        "Local — this project only (" + local_dir + "/)",
        "Global — all projects (" + global_dir + "/)",
    }, 0);
    config_dir = (idx == 1) ? global_dir : local_dir;
  }

  std::cout << "\nSetting up Codex (" << config_dir << ")...\n";

  std::cout << "\n[1/4] Skill\n";
  try {
    fs::path p = codex_write_skill(config_dir);
    status_ok("Skill", p.string());
  } catch (const std::exception& e) {
    status_error("Skill", e.what());
    return false;
  }

  std::cout << "\n[2/4] Prompts\n";
  std::string prompt_path;
  try {
    fs::path p = write_prompt_files();
    status_ok("Prompts", p.string());
    prompt_path = p.string();
  } catch (const std::exception& e) {
    status_error("Prompts", e.what());
    return false;
  }

  std::cout << "\n[3/4] Hooks\n";
  try {
    fs::path p = codex_write_hook(config_dir, "prime.sh", mnemon::embedded::codex_prime_sh());
    status_ok("Hook: prime", p.string());
  } catch (const std::exception& e) {
    status_error("Hook: prime", e.what());
    return false;
  }
  try {
    fs::path p = codex_write_hook(config_dir, "user_prompt.sh", mnemon::embedded::codex_user_prompt_sh());
    status_ok("Hook: remind", p.string());
  } catch (const std::exception& e) {
    status_error("Hook: remind", e.what());
    return false;
  }
  try {
    fs::path p = codex_write_hook(config_dir, "stop.sh", mnemon::embedded::codex_stop_sh());
    status_ok("Hook: stop", p.string());
  } catch (const std::exception& e) {
    status_error("Hook: stop", e.what());
    return false;
  }

  std::cout << "\n[4/4] Config\n";
  try {
    fs::path p = codex_register_hooks(config_dir);
    status_updated("Hooks config", p.string());
  } catch (const std::exception& e) {
    status_error("Hooks config", e.what());
    return false;
  }

  std::cout << "\nSetup complete!\n";
  std::cout << "  Skill   " << config_dir << "/skills/mnemon/SKILL.md\n";
  std::cout << "  Hooks   " << config_dir << "/hooks.json (SessionStart, UserPromptSubmit, Stop)\n";
  std::cout << "  Prompts " << prompt_path << "/ (guide.md, skill.md)\n\n";
  std::cout << "Start a new Codex session to activate.\n";
  std::cout << "Run 'mnemon setup --eject --target codex' to remove.\n";
  return true;
}

// --- cursor ---

static void remove_cursor_hooks(nlohmann::json& data) {
  if (!data.contains("hooks") || !data["hooks"].is_object()) {
    return;
  }
  auto& hooks = data["hooks"];
  static const char* keys[] = {"sessionStart", "stop", "preCompact"};
  for (const char* key : keys) {
    if (!hooks.contains(key) || !hooks[key].is_array()) {
      continue;
    }
    nlohmann::json filtered = filter_hook_array(hooks[key]);
    if (filtered.empty()) {
      hooks.erase(key);
    } else {
      hooks[key] = filtered;
    }
  }
  if (hooks.empty()) {
    data.erase("hooks");
    if (data.contains("version") && data.size() == 1) {
      data.erase("version");
    }
  }
}

static void add_cursor_hooks(nlohmann::json& data, const std::string& hooks_dir, const HookSelection& sel) {
  remove_cursor_hooks(data);
  if (!data.contains("version")) {
    data["version"] = 1;
  }
  if (!data.contains("hooks") || !data["hooks"].is_object()) {
    data["hooks"] = nlohmann::json::object();
  }
  auto& hooks = data["hooks"];
  fs::path hd = hooks_dir;

  auto session_entry = nlohmann::json::object();
  session_entry["command"] = (hd / "prime.sh").string();
  session_entry["timeout"] = 30;
  if (!hooks.contains("sessionStart")) {
    hooks["sessionStart"] = nlohmann::json::array();
  }
  hooks["sessionStart"].push_back(session_entry);

  if (sel.nudge) {
    auto stop_entry = nlohmann::json::object();
    stop_entry["command"] = (hd / "stop.sh").string();
    stop_entry["timeout"] = 30;
    stop_entry["loop_limit"] = 1;
    if (!hooks.contains("stop")) {
      hooks["stop"] = nlohmann::json::array();
    }
    hooks["stop"].push_back(stop_entry);
  }

  if (sel.compact) {
    auto compact_entry = nlohmann::json::object();
    compact_entry["command"] = (hd / "compact.sh").string();
    compact_entry["timeout"] = 30;
    if (!hooks.contains("preCompact")) {
      hooks["preCompact"] = nlohmann::json::array();
    }
    hooks["preCompact"].push_back(compact_entry);
  }
}

static fs::path cursor_write_skill(const std::string& config_dir) {
  fs::path skill_dir = fs::path(config_dir) / "skills" / "mnemon";
  fs::create_directories(skill_dir);
  fs::path p = skill_dir / "SKILL.md";
  write_bytes(p, mnemon::embedded::cursor_SKILL_md(), 0644);
  return p;
}

static fs::path cursor_write_hook(const std::string& config_dir, const std::string& filename, std::string_view content) {
  fs::path hook_path = fs::path(config_dir) / "hooks" / "mnemon" / filename;
  write_bytes(hook_path, content, 0755);
  return hook_path;
}

static fs::path cursor_register_hooks(const std::string& config_dir, const HookSelection& sel) {
  fs::path hooks_dir = fs::path(config_dir) / "hooks" / "mnemon";
  fs::path abs_hooks_dir = fs::absolute(hooks_dir);
  fs::path hooks_path = fs::path(config_dir) / "hooks.json";
  nlohmann::json data = read_json_file(hooks_path);
  add_cursor_hooks(data, abs_hooks_dir.string(), sel);
  write_json_file(hooks_path, data);
  return hooks_path;
}

static int cursor_eject(const std::string& config_dir) {
  int errs = 0;
  std::cout << "\nRemoving Cursor integration (" << config_dir << ")...\n";

  fs::path hooks_dir = fs::path(config_dir) / "hooks" / "mnemon";
  std::error_code ec;
  fs::remove_all(hooks_dir, ec);
  if (ec) {
    status_error("Hooks", ec.message());
    ++errs;
  } else {
    status_ok("Hooks", hooks_dir.string() + " removed");
  }
  remove_if_empty_dir(fs::path(config_dir) / "hooks");

  fs::path hooks_path = fs::path(config_dir) / "hooks.json";
  try {
    nlohmann::json data = read_json_file(hooks_path);
    remove_cursor_hooks(data);
    write_or_remove_json_file(hooks_path, data);
    status_ok("Hooks config", hooks_path.string() + " cleaned");
  } catch (const std::exception& e) {
    status_error("Hooks config", e.what());
    ++errs;
  }

  fs::path skill_dir = fs::path(config_dir) / "skills" / "mnemon";
  ec.clear();
  fs::remove_all(skill_dir, ec);
  if (ec) {
    status_error("Skill", ec.message());
    ++errs;
  } else {
    status_ok("Skill", skill_dir.string() + " removed");
  }
  remove_if_empty_dir(fs::path(config_dir) / "skills");
  remove_if_empty_dir(config_dir);
  return errs;
}

static HookSelection select_cursor_optional_hooks(bool setup_yes) {
  HookSelection sel{true, true, false};
  if (setup_yes || !is_tty_in()) {
    return sel;
  }
  while (true) {
    std::cout << "\n  " << c_bold() << "Select hooks to enable" << c_reset() << " " << c_dim()
              << "(toggle: 1-2, Enter=done)" << c_reset() << "\n";
    std::cout << "  1. " << (sel.nudge ? "[x]" : "[ ]")
              << " Nudge   — auto-submit a writeback reminder after responses (recommended)\n";
    std::cout << "  2. " << (sel.compact ? "[x]" : "[ ]") << " Compact — show a reminder before context compaction\n";
    std::cout << "› " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line) || line.empty()) {
      break;
    }
    try {
      int n = std::stoi(line);
      if (n == 1) {
        sel.nudge = !sel.nudge;
      } else if (n == 2) {
        sel.compact = !sel.compact;
      }
    } catch (...) {
    }
  }
  return sel;
}

static bool install_cursor(Environment env, bool global, bool setup_yes) {
  std::string config_dir = env.config_dir;
  if (!global && !setup_yes && is_tty_in()) {
    std::string local_dir = ".cursor";
    std::string global_dir = (fs::path(home_dir()) / ".cursor").string();
    size_t idx = select_one("Install scope", {
        "Local — this project only (" + local_dir + "/)",
        "Global — all projects (" + global_dir + "/)",
    }, 0);
    config_dir = (idx == 1) ? global_dir : local_dir;
  }

  std::cout << "\nSetting up Cursor (" << config_dir << ")...\n";

  std::cout << "\n[1/4] Skill\n";
  try {
    fs::path p = cursor_write_skill(config_dir);
    status_ok("Skill", p.string());
  } catch (const std::exception& e) {
    status_error("Skill", e.what());
    return false;
  }

  std::cout << "\n[2/4] Prompts\n";
  std::string prompt_path;
  try {
    fs::path p = write_prompt_files();
    status_ok("Prompts", p.string());
    prompt_path = p.string();
  } catch (const std::exception& e) {
    status_error("Prompts", e.what());
    return false;
  }

  std::cout << "\n[3/4] Hooks\n";
  HookSelection sel = select_cursor_optional_hooks(setup_yes);
  try {
    fs::path p = cursor_write_hook(config_dir, "prime.sh", mnemon::embedded::cursor_prime_sh());
    status_ok("Hook: prime", p.string());
  } catch (const std::exception& e) {
    status_error("Hook: prime", e.what());
    return false;
  }
  if (sel.nudge) {
    try {
      fs::path p = cursor_write_hook(config_dir, "stop.sh", mnemon::embedded::cursor_stop_sh());
      status_ok("Hook: nudge", p.string());
    } catch (const std::exception& e) {
      status_error("Hook: nudge", e.what());
      return false;
    }
  }
  if (sel.compact) {
    try {
      fs::path p = cursor_write_hook(config_dir, "compact.sh", mnemon::embedded::cursor_compact_sh());
      status_ok("Hook: compact", p.string());
    } catch (const std::exception& e) {
      status_error("Hook: compact", e.what());
      return false;
    }
  }

  std::cout << "\n[4/4] Config\n";
  try {
    fs::path p = cursor_register_hooks(config_dir, sel);
    status_updated("Hooks config", p.string());
  } catch (const std::exception& e) {
    status_error("Hooks config", e.what());
    return false;
  }

  std::string hook_names = "prime";
  if (sel.nudge) {
    hook_names += ", nudge";
  }
  if (sel.compact) {
    hook_names += ", compact";
  }

  std::cout << "\nSetup complete!\n";
  std::cout << "  Skill   " << config_dir << "/skills/mnemon/SKILL.md\n";
  std::cout << "  Hooks   " << config_dir << "/hooks.json (" << hook_names << ")\n";
  std::cout << "  Prompts " << prompt_path << "/ (guide.md, skill.md)\n\n";
  std::cout << "Start a new Cursor agent session to activate the mnemon skill.\n";
  std::cout << "Run 'mnemon setup --eject --target cursor' to remove.\n";
  return true;
}

// --- trae ---

static void remove_trae_hooks(nlohmann::json& data) {
  if (!data.contains("hooks") || !data["hooks"].is_object()) {
    return;
  }
  auto& hooks = data["hooks"];
  static const char* keys[] = {"SessionStart", "UserPromptSubmit", "Stop", "PreToolUse", "PostToolUse", "Notification"};
  for (const char* key : keys) {
    if (!hooks.contains(key) || !hooks[key].is_array()) {
      continue;
    }
    nlohmann::json filtered = filter_hook_array(hooks[key]);
    if (filtered.empty()) {
      hooks.erase(key);
    } else {
      hooks[key] = filtered;
    }
  }
  if (hooks.empty()) {
    data.erase("hooks");
    if (data.contains("version") && data.size() == 1) {
      data.erase("version");
    }
  }
}

static void add_trae_hook(nlohmann::json& hooks, const char* event, int loop_limit, const std::string& command) {
  auto entry = nlohmann::json::object();
  entry["hooks"] = nlohmann::json::array({{{"type", "command"}, {"command", command}, {"timeout", 30}}});
  if (loop_limit > 0) {
    entry["loop_limit"] = loop_limit;
  }
  if (!hooks.contains(event)) {
    hooks[event] = nlohmann::json::array();
  }
  hooks[event].push_back(entry);
}

static void add_trae_hooks(nlohmann::json& data, const std::string& hooks_dir) {
  remove_trae_hooks(data);
  if (!data.contains("version")) {
    data["version"] = 1;
  }
  if (!data.contains("hooks") || !data["hooks"].is_object()) {
    data["hooks"] = nlohmann::json::object();
  }
  auto& hooks = data["hooks"];
  fs::path hd = hooks_dir;
  add_trae_hook(hooks, "SessionStart", 0, (hd / "prime.sh").string());
  add_trae_hook(hooks, "UserPromptSubmit", 0, (hd / "user_prompt.sh").string());
  add_trae_hook(hooks, "Stop", 1, (hd / "stop.sh").string());
}

static fs::path trae_write_skill(const std::string& config_dir) {
  fs::path skill_dir = fs::path(config_dir) / "skills" / "mnemon";
  fs::create_directories(skill_dir);
  fs::path p = skill_dir / "SKILL.md";
  write_bytes(p, mnemon::embedded::trae_SKILL_md(), 0644);
  return p;
}

static fs::path trae_write_hook(const std::string& config_dir, const std::string& filename, std::string_view content) {
  fs::path hook_path = fs::path(config_dir) / "hooks" / "mnemon" / filename;
  write_bytes(hook_path, content, 0755);
  return hook_path;
}

static fs::path trae_register_hooks(const std::string& config_dir) {
  fs::path hooks_dir = fs::path(config_dir) / "hooks" / "mnemon";
  fs::path abs_hooks_dir = fs::absolute(hooks_dir);
  fs::path hooks_path = fs::path(config_dir) / "hooks.json";
  nlohmann::json data = read_json_file(hooks_path);
  add_trae_hooks(data, abs_hooks_dir.string());
  write_json_file(hooks_path, data);
  return hooks_path;
}

static int trae_eject(const std::string& config_dir) {
  int errs = 0;
  std::cout << "\nRemoving Trae integration (" << config_dir << ")...\n";

  fs::path hooks_dir = fs::path(config_dir) / "hooks" / "mnemon";
  std::error_code ec;
  fs::remove_all(hooks_dir, ec);
  if (ec) {
    status_error("Hooks", ec.message());
    ++errs;
  } else {
    status_ok("Hooks", hooks_dir.string() + " removed");
  }
  remove_if_empty_dir(fs::path(config_dir) / "hooks");

  fs::path hooks_path = fs::path(config_dir) / "hooks.json";
  try {
    nlohmann::json data = read_json_file(hooks_path);
    remove_trae_hooks(data);
    write_or_remove_json_file(hooks_path, data);
    status_ok("Hooks config", hooks_path.string() + " cleaned");
  } catch (const std::exception& e) {
    status_error("Hooks config", e.what());
    ++errs;
  }

  fs::path skill_dir = fs::path(config_dir) / "skills" / "mnemon";
  ec.clear();
  fs::remove_all(skill_dir, ec);
  if (ec) {
    status_error("Skill", ec.message());
    ++errs;
  } else {
    status_ok("Skill", skill_dir.string() + " removed");
  }
  remove_if_empty_dir(fs::path(config_dir) / "skills");
  remove_if_empty_dir(config_dir);
  return errs;
}

static bool install_trae(Environment env, bool global, bool setup_yes) {
  std::string config_dir = env.config_dir;
  if (!global && !setup_yes && is_tty_in()) {
    std::string local_dir = ".trae";
    std::string global_dir = (fs::path(home_dir()) / ".trae").string();
    size_t idx = select_one("Install scope", {
        "Local — this project only (" + local_dir + "/)",
        "Global — all projects (" + global_dir + "/)",
    }, 0);
    config_dir = (idx == 1) ? global_dir : local_dir;
  }

  std::cout << "\nSetting up Trae (" << config_dir << ")...\n";

  std::cout << "\n[1/3] Skill\n";
  try {
    fs::path p = trae_write_skill(config_dir);
    status_ok("Skill", p.string());
  } catch (const std::exception& e) {
    status_error("Skill", e.what());
    return false;
  }

  std::cout << "\n[2/3] Prompts\n";
  std::string prompt_path;
  try {
    fs::path p = write_prompt_files();
    status_ok("Prompts", p.string());
    prompt_path = p.string();
  } catch (const std::exception& e) {
    status_error("Prompts", e.what());
    return false;
  }

  std::cout << "\n[3/3] Hooks\n";
  struct HookFile {
    const char* label;
    const char* filename;
    std::string_view content;
  };
  HookFile hook_files[] = {
      {"Hook: prime", "prime.sh", mnemon::embedded::trae_prime_sh()},
      {"Hook: remind", "user_prompt.sh", mnemon::embedded::trae_user_prompt_sh()},
      {"Hook: nudge", "stop.sh", mnemon::embedded::trae_stop_sh()},
  };
  for (const auto& hf : hook_files) {
    try {
      fs::path p = trae_write_hook(config_dir, hf.filename, hf.content);
      status_ok(hf.label, p.string());
    } catch (const std::exception& e) {
      status_error(hf.label, e.what());
      return false;
    }
  }
  try {
    fs::path p = trae_register_hooks(config_dir);
    status_updated("Hooks config", p.string());
  } catch (const std::exception& e) {
    status_error("Hooks config", e.what());
    return false;
  }

  std::cout << "\nSetup complete!\n";
  std::cout << "  Skill   " << config_dir << "/skills/mnemon/SKILL.md\n";
  std::cout << "  Hooks   " << config_dir << "/hooks.json (SessionStart, UserPromptSubmit, Stop)\n";
  std::cout << "  Prompts " << prompt_path << "/ (guide.md, skill.md)\n\n";
  std::cout << "Restart Trae to activate the mnemon skill and hooks.\n";
  std::cout << "Run 'mnemon setup --eject --target trae' to remove.\n";
  return true;
}

// --- qoder / qoderwork ---

static void remove_qoder_hooks(nlohmann::json& data) {
  if (!data.contains("hooks") || !data["hooks"].is_object()) {
    return;
  }
  auto& hooks = data["hooks"];
  static const char* keys[] = {"SessionStart", "UserPromptSubmit", "Stop",
                               "PreToolUse",   "PostToolUse",      "PostToolUseFailure",
                               "Notification", "PermissionRequest", "PreCompact",
                               "SessionEnd",   "SubagentStart",     "SubagentStop"};
  for (const char* key : keys) {
    if (!hooks.contains(key) || !hooks[key].is_array()) {
      continue;
    }
    nlohmann::json filtered = filter_hook_array(hooks[key]);
    if (filtered.empty()) {
      hooks.erase(key);
    } else {
      hooks[key] = filtered;
    }
  }
  if (hooks.empty()) {
    data.erase("hooks");
  }
}

static void add_qoder_hook(nlohmann::json& hooks, const char* event, const std::string& command) {
  auto entry = nlohmann::json::object();
  entry["hooks"] = nlohmann::json::array({{{"type", "command"}, {"command", command}}});
  if (!hooks.contains(event)) {
    hooks[event] = nlohmann::json::array();
  }
  hooks[event].push_back(entry);
}

static void add_qoder_hooks(nlohmann::json& data, const std::string& hooks_dir) {
  remove_qoder_hooks(data);
  if (!data.contains("hooks") || !data["hooks"].is_object()) {
    data["hooks"] = nlohmann::json::object();
  }
  auto& hooks = data["hooks"];
  fs::path hd = hooks_dir;
  add_qoder_hook(hooks, "SessionStart", (hd / "prime.sh").string());
  add_qoder_hook(hooks, "UserPromptSubmit", (hd / "user_prompt.sh").string());
  add_qoder_hook(hooks, "Stop", (hd / "stop.sh").string());
}

static fs::path write_qoder_skill(const std::string& config_dir, std::string_view content) {
  fs::path skill_dir = fs::path(config_dir) / "skills" / "mnemon";
  fs::create_directories(skill_dir);
  fs::path p = skill_dir / "SKILL.md";
  write_bytes(p, content, 0644);
  return p;
}

static fs::path qoder_write_hook(const std::string& config_dir, const std::string& filename, std::string_view content) {
  fs::path hook_path = fs::path(config_dir) / "hooks" / "mnemon" / filename;
  write_bytes(hook_path, content, 0755);
  return hook_path;
}

static fs::path register_qoder_hooks(const std::string& config_dir) {
  fs::path hooks_dir = fs::path(config_dir) / "hooks" / "mnemon";
  fs::path abs_hooks_dir = fs::absolute(hooks_dir);
  fs::path settings_path = fs::path(config_dir) / "settings.json";
  nlohmann::json data = read_json_file(settings_path);
  add_qoder_hooks(data, abs_hooks_dir.string());
  write_json_file(settings_path, data);
  return settings_path;
}

static int eject_qoder(const std::string& display, const std::string& config_dir) {
  int errs = 0;
  std::cout << "\nRemoving " << display << " integration (" << config_dir << ")...\n";

  fs::path hooks_dir = fs::path(config_dir) / "hooks" / "mnemon";
  std::error_code ec;
  fs::remove_all(hooks_dir, ec);
  if (ec) {
    status_error("Hooks", ec.message());
    ++errs;
  } else {
    status_ok("Hooks", hooks_dir.string() + " removed");
  }
  remove_if_empty_dir(fs::path(config_dir) / "hooks");

  fs::path settings_path = fs::path(config_dir) / "settings.json";
  try {
    nlohmann::json data = read_json_file(settings_path);
    remove_qoder_hooks(data);
    write_or_remove_json_file(settings_path, data);
    status_ok("Settings", settings_path.string() + " cleaned");
  } catch (const std::exception& e) {
    status_error("Settings", e.what());
    ++errs;
  }

  fs::path skill_dir = fs::path(config_dir) / "skills" / "mnemon";
  ec.clear();
  fs::remove_all(skill_dir, ec);
  if (ec) {
    status_error("Skill", ec.message());
    ++errs;
  } else {
    status_ok("Skill", skill_dir.string() + " removed");
  }
  remove_if_empty_dir(fs::path(config_dir) / "skills");
  remove_if_empty_dir(config_dir);
  return errs;
}

static bool install_qoder_like(const std::string& config_dir, std::string_view skill_content,
                               const std::string& activation, const std::string& eject_hint) {
  std::cout << "\n[1/3] Skill\n";
  try {
    fs::path p = write_qoder_skill(config_dir, skill_content);
    status_ok("Skill", p.string());
  } catch (const std::exception& e) {
    status_error("Skill", e.what());
    return false;
  }

  std::cout << "\n[2/3] Prompts\n";
  std::string prompt_path;
  try {
    fs::path p = write_prompt_files();
    status_ok("Prompts", p.string());
    prompt_path = p.string();
  } catch (const std::exception& e) {
    status_error("Prompts", e.what());
    return false;
  }

  std::cout << "\n[3/3] Hooks\n";
  struct HookFile {
    const char* label;
    const char* filename;
    std::string_view content;
  };
  HookFile hook_files[] = {
      {"Hook: prime", "prime.sh", mnemon::embedded::qoder_prime_sh()},
      {"Hook: remind", "user_prompt.sh", mnemon::embedded::qoder_user_prompt_sh()},
      {"Hook: nudge", "stop.sh", mnemon::embedded::qoder_stop_sh()},
  };
  for (const auto& hf : hook_files) {
    try {
      fs::path p = qoder_write_hook(config_dir, hf.filename, hf.content);
      status_ok(hf.label, p.string());
    } catch (const std::exception& e) {
      status_error(hf.label, e.what());
      return false;
    }
  }
  try {
    fs::path p = register_qoder_hooks(config_dir);
    status_updated("Settings", p.string());
  } catch (const std::exception& e) {
    status_error("Settings", e.what());
    return false;
  }

  std::cout << "\nSetup complete!\n";
  std::cout << "  Skill    " << config_dir << "/skills/mnemon/SKILL.md\n";
  std::cout << "  Hooks    " << config_dir << "/settings.json (SessionStart, UserPromptSubmit, Stop)\n";
  std::cout << "  Prompts  " << prompt_path << "/ (guide.md, skill.md)\n\n";
  std::cout << activation << "\n";
  std::cout << eject_hint << "\n";
  return true;
}

static bool install_qoder(Environment env, bool global, bool setup_yes) {
  std::string config_dir = env.config_dir;
  if (!global && !setup_yes && is_tty_in()) {
    std::string local_dir = ".qoder";
    std::string global_dir = (fs::path(home_dir()) / ".qoder").string();
    size_t idx = select_one("Install scope", {
        "Local — this project only (" + local_dir + "/)",
        "Global — all projects (" + global_dir + "/)",
    }, 0);
    config_dir = (idx == 1) ? global_dir : local_dir;
  }

  std::cout << "\nSetting up Qoder (" << config_dir << ")...\n";
  return install_qoder_like(config_dir, mnemon::embedded::qoder_SKILL_md(),
                            "Restart Qoder IDE/CLI to activate the mnemon skill and hooks.",
                            "Run 'mnemon setup --eject --target qoder' to remove.");
}

static bool install_qoderwork(Environment env) {
  std::string config_dir = env.config_dir;
  std::cout << "\nSetting up QoderWork (" << config_dir << ")...\n";
  return install_qoder_like(config_dir, mnemon::embedded::qoderwork_SKILL_md(),
                            "Restart QoderWork to activate the mnemon skill and hooks.",
                            "Run 'mnemon setup --eject --target qoderwork' to remove.");
}

// --- codebuddy ---

static void remove_codebuddy_hooks(nlohmann::json& data) {
  if (!data.contains("hooks") || !data["hooks"].is_object()) {
    return;
  }
  auto& hooks = data["hooks"];
  static const char* keys[] = {"SessionStart", "UserPromptSubmit", "Stop",      "PreToolUse", "PostToolUse",
                               "Notification", "PreCompact",       "SessionEnd", "SubagentStop"};
  for (const char* key : keys) {
    if (!hooks.contains(key) || !hooks[key].is_array()) {
      continue;
    }
    nlohmann::json filtered = filter_hook_array(hooks[key]);
    if (filtered.empty()) {
      hooks.erase(key);
    } else {
      hooks[key] = filtered;
    }
  }
  if (hooks.empty()) {
    data.erase("hooks");
  }
}

static void add_codebuddy_hooks(nlohmann::json& data, const std::string& hooks_dir) {
  remove_codebuddy_hooks(data);
  if (!data.contains("hooks") || !data["hooks"].is_object()) {
    data["hooks"] = nlohmann::json::object();
  }
  auto& hooks = data["hooks"];
  fs::path hd = hooks_dir;
  // CodeBuddy hook entries share Qoder's {type, command} nested shape.
  add_qoder_hook(hooks, "SessionStart", (hd / "prime.sh").string());
  add_qoder_hook(hooks, "UserPromptSubmit", (hd / "user_prompt.sh").string());
  add_qoder_hook(hooks, "Stop", (hd / "stop.sh").string());
}

static fs::path codebuddy_write_skill(const std::string& config_dir) {
  fs::path skill_dir = fs::path(config_dir) / "skills" / "mnemon";
  fs::create_directories(skill_dir);
  fs::path p = skill_dir / "SKILL.md";
  write_bytes(p, mnemon::embedded::codebuddy_SKILL_md(), 0644);
  return p;
}

static fs::path codebuddy_write_hook(const std::string& config_dir, const std::string& filename,
                                     std::string_view content) {
  fs::path hook_path = fs::path(config_dir) / "hooks" / "mnemon" / filename;
  write_bytes(hook_path, content, 0755);
  return hook_path;
}

static fs::path codebuddy_register_hooks(const std::string& config_dir) {
  fs::path hooks_dir = fs::path(config_dir) / "hooks" / "mnemon";
  fs::path abs_hooks_dir = fs::absolute(hooks_dir);
  fs::path settings_path = fs::path(config_dir) / "settings.json";
  nlohmann::json data = read_json_file(settings_path);
  add_codebuddy_hooks(data, abs_hooks_dir.string());
  write_json_file(settings_path, data);
  return settings_path;
}

static int codebuddy_eject(const std::string& config_dir) {
  int errs = 0;
  std::cout << "\nRemoving CodeBuddy integration (" << config_dir << ")...\n";

  fs::path hooks_dir = fs::path(config_dir) / "hooks" / "mnemon";
  std::error_code ec;
  fs::remove_all(hooks_dir, ec);
  if (ec) {
    status_error("Hooks", ec.message());
    ++errs;
  } else {
    status_ok("Hooks", hooks_dir.string() + " removed");
  }
  remove_if_empty_dir(fs::path(config_dir) / "hooks");

  fs::path settings_path = fs::path(config_dir) / "settings.json";
  try {
    nlohmann::json data = read_json_file(settings_path);
    remove_codebuddy_hooks(data);
    write_or_remove_json_file(settings_path, data);
    status_ok("Settings", settings_path.string() + " cleaned");
  } catch (const std::exception& e) {
    status_error("Settings", e.what());
    ++errs;
  }

  fs::path skill_dir = fs::path(config_dir) / "skills" / "mnemon";
  ec.clear();
  fs::remove_all(skill_dir, ec);
  if (ec) {
    status_error("Skill", ec.message());
    ++errs;
  } else {
    status_ok("Skill", skill_dir.string() + " removed");
  }
  remove_if_empty_dir(fs::path(config_dir) / "skills");
  remove_if_empty_dir(config_dir);
  return errs;
}

static bool install_codebuddy(Environment env, bool global, bool setup_yes) {
  std::string config_dir = env.config_dir;
  if (!global && !setup_yes && is_tty_in()) {
    std::string local_dir = ".codebuddy";
    std::string global_dir = (fs::path(home_dir()) / ".codebuddy").string();
    size_t idx = select_one("Install scope", {
        "Local — this project only (" + local_dir + "/)",
        "Global — all projects (" + global_dir + "/)",
    }, 0);
    config_dir = (idx == 1) ? global_dir : local_dir;
  }

  std::cout << "\nSetting up CodeBuddy (" << config_dir << ")...\n";

  std::cout << "\n[1/3] Skill\n";
  try {
    fs::path p = codebuddy_write_skill(config_dir);
    status_ok("Skill", p.string());
  } catch (const std::exception& e) {
    status_error("Skill", e.what());
    return false;
  }

  std::cout << "\n[2/3] Prompts\n";
  std::string prompt_path;
  try {
    fs::path p = write_prompt_files();
    status_ok("Prompts", p.string());
    prompt_path = p.string();
  } catch (const std::exception& e) {
    status_error("Prompts", e.what());
    return false;
  }

  std::cout << "\n[3/3] Hooks\n";
  struct HookFile {
    const char* label;
    const char* filename;
    std::string_view content;
  };
  HookFile hook_files[] = {
      {"Hook: prime", "prime.sh", mnemon::embedded::codebuddy_prime_sh()},
      {"Hook: remind", "user_prompt.sh", mnemon::embedded::codebuddy_user_prompt_sh()},
      {"Hook: nudge", "stop.sh", mnemon::embedded::codebuddy_stop_sh()},
  };
  for (const auto& hf : hook_files) {
    try {
      fs::path p = codebuddy_write_hook(config_dir, hf.filename, hf.content);
      status_ok(hf.label, p.string());
    } catch (const std::exception& e) {
      status_error(hf.label, e.what());
      return false;
    }
  }
  try {
    fs::path p = codebuddy_register_hooks(config_dir);
    status_updated("Settings", p.string());
  } catch (const std::exception& e) {
    status_error("Settings", e.what());
    return false;
  }

  std::cout << "\nSetup complete!\n";
  std::cout << "  Skill    " << config_dir << "/skills/mnemon/SKILL.md\n";
  std::cout << "  Hooks    " << config_dir << "/settings.json (SessionStart, UserPromptSubmit, Stop)\n";
  std::cout << "  Prompts  " << prompt_path << "/ (guide.md, skill.md)\n\n";
  std::cout << "Restart CodeBuddy Code to activate the mnemon skill and hooks.\n";
  std::cout << "Run 'mnemon setup --eject --target codebuddy' to remove.\n";
  return true;
}

// --- workbuddy ---

static void remove_workbuddy_hooks(nlohmann::json& data) {
  if (!data.contains("hooks") || !data["hooks"].is_object()) {
    return;
  }
  auto& hooks = data["hooks"];
  static const char* keys[] = {"SessionStart", "UserPromptSubmit", "Stop",      "PreToolUse", "PostToolUse",
                               "Notification", "PreCompact",       "SessionEnd", "SubagentStop"};
  for (const char* key : keys) {
    if (!hooks.contains(key) || !hooks[key].is_array()) {
      continue;
    }
    nlohmann::json filtered = filter_hook_array(hooks[key]);
    if (filtered.empty()) {
      hooks.erase(key);
    } else {
      hooks[key] = filtered;
    }
  }
  if (hooks.empty()) {
    data.erase("hooks");
  }
}

static void add_workbuddy_hooks(nlohmann::json& data, const std::string& hooks_dir) {
  remove_workbuddy_hooks(data);
  if (!data.contains("hooks") || !data["hooks"].is_object()) {
    data["hooks"] = nlohmann::json::object();
  }
  auto& hooks = data["hooks"];
  fs::path hd = hooks_dir;
  // WorkBuddy hook entries share Qoder's {type, command} nested shape.
  add_qoder_hook(hooks, "SessionStart", (hd / "prime.sh").string());
  add_qoder_hook(hooks, "UserPromptSubmit", (hd / "user_prompt.sh").string());
  add_qoder_hook(hooks, "Stop", (hd / "stop.sh").string());
}

static fs::path workbuddy_write_skill(const std::string& config_dir) {
  fs::path skill_dir = fs::path(config_dir) / "skills" / "mnemon";
  fs::create_directories(skill_dir);
  fs::path p = skill_dir / "SKILL.md";
  write_bytes(p, mnemon::embedded::workbuddy_SKILL_md(), 0644);
  return p;
}

static fs::path workbuddy_write_hook(const std::string& config_dir, const std::string& filename,
                                     std::string_view content) {
  fs::path hook_path = fs::path(config_dir) / "hooks" / "mnemon" / filename;
  write_bytes(hook_path, content, 0755);
  return hook_path;
}

static fs::path workbuddy_register_hooks(const std::string& config_dir) {
  fs::path hooks_dir = fs::path(config_dir) / "hooks" / "mnemon";
  fs::path abs_hooks_dir = fs::absolute(hooks_dir);
  fs::path settings_path = fs::path(config_dir) / "settings.json";
  nlohmann::json data = read_json_file(settings_path);
  add_workbuddy_hooks(data, abs_hooks_dir.string());
  write_json_file(settings_path, data);
  return settings_path;
}

static int workbuddy_eject(const std::string& config_dir) {
  int errs = 0;
  std::cout << "\nRemoving WorkBuddy integration (" << config_dir << ")...\n";

  fs::path hooks_dir = fs::path(config_dir) / "hooks" / "mnemon";
  std::error_code ec;
  fs::remove_all(hooks_dir, ec);
  if (ec) {
    status_error("Hooks", ec.message());
    ++errs;
  } else {
    status_ok("Hooks", hooks_dir.string() + " removed");
  }
  remove_if_empty_dir(fs::path(config_dir) / "hooks");

  fs::path settings_path = fs::path(config_dir) / "settings.json";
  try {
    nlohmann::json data = read_json_file(settings_path);
    remove_workbuddy_hooks(data);
    write_or_remove_json_file(settings_path, data);
    status_ok("Settings", settings_path.string() + " cleaned");
  } catch (const std::exception& e) {
    status_error("Settings", e.what());
    ++errs;
  }

  fs::path skill_dir = fs::path(config_dir) / "skills" / "mnemon";
  ec.clear();
  fs::remove_all(skill_dir, ec);
  if (ec) {
    status_error("Skill", ec.message());
    ++errs;
  } else {
    status_ok("Skill", skill_dir.string() + " removed");
  }
  remove_if_empty_dir(fs::path(config_dir) / "skills");
  remove_if_empty_dir(config_dir);
  return errs;
}

static bool install_workbuddy(Environment env, bool global, bool setup_yes) {
  std::string config_dir = env.config_dir;
  if (!global && !setup_yes && is_tty_in()) {
    std::string local_dir = ".workbuddy";
    std::string global_dir = (fs::path(home_dir()) / ".workbuddy").string();
    size_t idx = select_one("Install scope", {
        "Local — this project only (" + local_dir + "/)",
        "Global — all projects (" + global_dir + "/)",
    }, 0);
    config_dir = (idx == 1) ? global_dir : local_dir;
  }

  std::cout << "\nSetting up WorkBuddy (" << config_dir << ")...\n";

  std::cout << "\n[1/3] Skill\n";
  try {
    fs::path p = workbuddy_write_skill(config_dir);
    status_ok("Skill", p.string());
  } catch (const std::exception& e) {
    status_error("Skill", e.what());
    return false;
  }

  std::cout << "\n[2/3] Prompts\n";
  std::string prompt_path;
  try {
    fs::path p = write_prompt_files();
    status_ok("Prompts", p.string());
    prompt_path = p.string();
  } catch (const std::exception& e) {
    status_error("Prompts", e.what());
    return false;
  }

  std::cout << "\n[3/3] Hooks\n";
  struct HookFile {
    const char* label;
    const char* filename;
    std::string_view content;
  };
  HookFile hook_files[] = {
      {"Hook: prime", "prime.sh", mnemon::embedded::workbuddy_prime_sh()},
      {"Hook: remind", "user_prompt.sh", mnemon::embedded::workbuddy_user_prompt_sh()},
      {"Hook: nudge", "stop.sh", mnemon::embedded::workbuddy_stop_sh()},
  };
  for (const auto& hf : hook_files) {
    try {
      fs::path p = workbuddy_write_hook(config_dir, hf.filename, hf.content);
      status_ok(hf.label, p.string());
    } catch (const std::exception& e) {
      status_error(hf.label, e.what());
      return false;
    }
  }
  try {
    fs::path p = workbuddy_register_hooks(config_dir);
    status_updated("Settings", p.string());
  } catch (const std::exception& e) {
    status_error("Settings", e.what());
    return false;
  }

  std::cout << "\nSetup complete!\n";
  std::cout << "  Skill    " << config_dir << "/skills/mnemon/SKILL.md\n";
  std::cout << "  Hooks    " << config_dir << "/settings.json (SessionStart, UserPromptSubmit, Stop)\n";
  std::cout << "  Prompts  " << prompt_path << "/ (guide.md, skill.md)\n\n";
  std::cout << "Restart WorkBuddy to activate the mnemon skill and hooks.\n";
  std::cout << "Run 'mnemon setup --eject --target workbuddy' to remove.\n";
  return true;
}

// --- kimi ---

// toml_quote mirrors Go's fmt %q for the ASCII paths/strings used in hook blocks.
static std::string toml_quote(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '\\' || c == '"') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

static std::string kimi_hook_block(const std::string& event, const std::string& command) {
  // matcher is always empty for mnemon's three lifecycle hooks.
  return "[[hooks]]\nevent = " + toml_quote(event) + "\ncommand = " + toml_quote(command) + "\ntimeout = 10";
}

static std::string collapse_excess_blank_lines(const std::string& s) {
  std::vector<std::string> lines;
  size_t pos = 0;
  while (true) {
    size_t nl = s.find('\n', pos);
    if (nl == std::string::npos) {
      lines.push_back(s.substr(pos));
      break;
    }
    lines.push_back(s.substr(pos, nl - pos));
    pos = nl + 1;
  }
  std::vector<std::string> out;
  int blank = 0;
  for (const auto& line : lines) {
    bool is_blank = line.find_first_not_of(" \t\r") == std::string::npos;
    if (is_blank) {
      ++blank;
      if (blank > 2) {
        continue;
      }
    } else {
      blank = 0;
    }
    out.push_back(line);
  }
  std::string joined;
  for (size_t i = 0; i < out.size(); ++i) {
    if (i) {
      joined.push_back('\n');
    }
    joined += out[i];
  }
  size_t end = joined.find_last_not_of('\n');
  joined = (end == std::string::npos) ? "" : joined.substr(0, end + 1);
  return joined + "\n";
}

static bool only_whitespace(const std::string& s) {
  return s.find_first_not_of(" \t\r\n") == std::string::npos;
}

static std::string remove_kimi_hooks(const std::string& config) {
  if (only_whitespace(config)) {
    return "";
  }
  // SplitAfter: keep the trailing '\n' on each line.
  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos < config.size()) {
    size_t nl = config.find('\n', pos);
    if (nl == std::string::npos) {
      lines.push_back(config.substr(pos));
      break;
    }
    lines.push_back(config.substr(pos, nl - pos + 1));
    pos = nl + 1;
  }

  auto trim_line = [](const std::string& l) {
    std::string t = l;
    if (!t.empty() && t.back() == '\n') {
      t.pop_back();
    }
    size_t b = t.find_first_not_of(" \t\r\n");
    size_t e = t.find_last_not_of(" \t\r\n");
    return (b == std::string::npos) ? std::string() : t.substr(b, e - b + 1);
  };

  std::string out;
  for (size_t i = 0; i < lines.size();) {
    if (trim_line(lines[i]) != "[[hooks]]") {
      out += lines[i];
      ++i;
      continue;
    }
    size_t start = i;
    ++i;
    for (; i < lines.size(); ++i) {
      std::string next = trim_line(lines[i]);
      if (!next.empty() && next[0] == '[') {
        break;
      }
    }
    std::string block;
    for (size_t j = start; j < i; ++j) {
      block += lines[j];
    }
    if (block.find("mnemon") != std::string::npos) {
      continue;
    }
    out += block;
  }
  return collapse_excess_blank_lines(out);
}

static std::string add_kimi_hooks(const std::string& config, const std::string& hooks_dir) {
  std::string cleaned = remove_kimi_hooks(config);
  size_t end = cleaned.find_last_not_of('\n');
  cleaned = (end == std::string::npos) ? "" : cleaned.substr(0, end + 1);

  fs::path hd = hooks_dir;
  std::string blocks = kimi_hook_block("SessionStart", (hd / "prime.sh").string()) + "\n\n" +
                       kimi_hook_block("UserPromptSubmit", (hd / "user_prompt.sh").string()) + "\n\n" +
                       kimi_hook_block("Stop", (hd / "stop.sh").string());
  if (cleaned.empty()) {
    return blocks + "\n";
  }
  return cleaned + "\n\n" + blocks + "\n";
}

static fs::path kimi_write_skill(const std::string& config_dir) {
  fs::path skill_dir = fs::path(config_dir) / "skills" / "mnemon";
  fs::create_directories(skill_dir);
  fs::path p = skill_dir / "SKILL.md";
  write_bytes(p, mnemon::embedded::kimi_SKILL_md(), 0644);
  return p;
}

static fs::path kimi_write_hook(const std::string& config_dir, const std::string& filename,
                                std::string_view content) {
  fs::path hook_path = fs::path(config_dir) / "hooks" / "mnemon" / filename;
  write_bytes(hook_path, content, 0755);
  return hook_path;
}

static fs::path kimi_register_hooks(const std::string& config_dir) {
  fs::path hooks_dir = fs::path(config_dir) / "hooks" / "mnemon";
  fs::path abs_hooks_dir = fs::absolute(hooks_dir);
  fs::path config_path = fs::path(config_dir) / "config.toml";

  std::string data;
  {
    std::ifstream in(config_path);
    if (in) {
      data.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
  }
  std::string updated = add_kimi_hooks(data, abs_hooks_dir.string());
  fs::create_directories(config_dir);
  write_bytes(config_path, updated, 0644);
  return config_path;
}

static int kimi_eject(const std::string& config_dir) {
  int errs = 0;
  std::cout << "\nRemoving Kimi Code integration (" << config_dir << ")...\n";

  fs::path hooks_dir = fs::path(config_dir) / "hooks" / "mnemon";
  std::error_code ec;
  fs::remove_all(hooks_dir, ec);
  if (ec) {
    status_error("Hooks", ec.message());
    ++errs;
  } else {
    status_ok("Hooks", hooks_dir.string() + " removed");
  }
  remove_if_empty_dir(fs::path(config_dir) / "hooks");

  fs::path config_path = fs::path(config_dir) / "config.toml";
  std::error_code exists_ec;
  if (fs::exists(config_path, exists_ec)) {
    std::ifstream in(config_path);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::string cleaned = remove_kimi_hooks(data);
    if (only_whitespace(cleaned)) {
      fs::remove(config_path, ec);
      if (ec) {
        status_error("Config", ec.message());
        ++errs;
      } else {
        status_ok("Config", config_path.string() + " removed");
      }
    } else {
      try {
        write_bytes(config_path, cleaned, 0644);
        status_ok("Config", config_path.string() + " cleaned");
      } catch (const std::exception& e) {
        status_error("Config", e.what());
        ++errs;
      }
    }
  }

  fs::path skill_dir = fs::path(config_dir) / "skills" / "mnemon";
  ec.clear();
  fs::remove_all(skill_dir, ec);
  if (ec) {
    status_error("Skill", ec.message());
    ++errs;
  } else {
    status_ok("Skill", skill_dir.string() + " removed");
  }
  remove_if_empty_dir(fs::path(config_dir) / "skills");
  remove_if_empty_dir(config_dir);
  return errs;
}

static bool install_kimi(Environment env) {
  std::string config_dir = env.config_dir;

  std::cout << "\nSetting up Kimi Code (" << config_dir << ")...\n";

  std::cout << "\n[1/3] Skill\n";
  try {
    fs::path p = kimi_write_skill(config_dir);
    status_ok("Skill", p.string());
  } catch (const std::exception& e) {
    status_error("Skill", e.what());
    return false;
  }

  std::cout << "\n[2/3] Prompts\n";
  std::string prompt_path;
  try {
    fs::path p = write_prompt_files();
    status_ok("Prompts", p.string());
    prompt_path = p.string();
  } catch (const std::exception& e) {
    status_error("Prompts", e.what());
    return false;
  }

  std::cout << "\n[3/3] Hooks\n";
  struct HookFile {
    const char* label;
    const char* filename;
    std::string_view content;
  };
  HookFile hook_files[] = {
      {"Hook: prime", "prime.sh", mnemon::embedded::kimi_prime_sh()},
      {"Hook: remind", "user_prompt.sh", mnemon::embedded::kimi_user_prompt_sh()},
      {"Hook: nudge", "stop.sh", mnemon::embedded::kimi_stop_sh()},
  };
  for (const auto& hf : hook_files) {
    try {
      fs::path p = kimi_write_hook(config_dir, hf.filename, hf.content);
      status_ok(hf.label, p.string());
    } catch (const std::exception& e) {
      status_error(hf.label, e.what());
      return false;
    }
  }
  try {
    fs::path p = kimi_register_hooks(config_dir);
    status_updated("Config", p.string());
  } catch (const std::exception& e) {
    status_error("Config", e.what());
    return false;
  }

  std::cout << "\nSetup complete!\n";
  std::cout << "  Skill    " << config_dir << "/skills/mnemon/SKILL.md\n";
  std::cout << "  Hooks    " << config_dir << "/config.toml (SessionStart, UserPromptSubmit, Stop)\n";
  std::cout << "  Prompts  " << prompt_path << "/ (guide.md, skill.md)\n\n";
  std::cout << "Restart Kimi Code to activate the mnemon skill and hooks.\n";
  std::cout << "Run 'mnemon setup --eject --target kimi' to remove.\n";
  return true;
}

// --- install flows ---

static HookSelection select_optional_hooks(bool setup_yes) {
  HookSelection sel{true, true, false};
  if (setup_yes || !is_tty_in()) {
    return sel;
  }
  std::vector<std::string> opts = {
      "Remind  — remind agent to recall & remember on each message (recommended)",
      "Nudge   — remind about memory on session end",
      "Compact — save key insights before context compaction",
  };
  return select_multi("Select hooks to enable", opts, sel);
}

static HookSelection select_openclaw_optional_hooks(bool setup_yes) {
  return select_optional_hooks(setup_yes);
}

static bool install_claude_code(Environment env, bool global, bool setup_yes, const RunOptions& opt) {
  std::string config_dir = env.config_dir;
  if (!global && !setup_yes && is_tty_in()) {
    fs::path global_dir = fs::path(home_dir()) / ".claude";
    std::vector<std::string> opts = {
        "Local — this project only (.claude/)",
        "Global — all projects (" + global_dir.string() + "/)",
    };
    size_t idx = select_one("Install scope", opts, 0);
    config_dir = (idx == 1) ? global_dir.string() : std::string(".claude");
  }

  std::cout << "\nSetting up Claude Code (" << config_dir << ")...\n";

  std::cout << "\n[1/3] Skill\n";
  try {
    fs::path p = claude_write_skill(config_dir);
    status_ok("Skill", p.string());
  } catch (const std::exception& e) {
    status_error("Skill", e.what());
    return false;
  }

  std::cout << "\n[2/3] Prompts\n";
  std::string prompt_path;
  try {
    fs::path p = write_prompt_files();
    status_ok("Prompts", p.string());
    prompt_path = p.string();
  } catch (const std::exception& e) {
    status_error("Prompts", e.what());
    return false;
  }

  try {
    fs::path p = claude_write_hook(config_dir, "prime.sh", mnemon::embedded::claude_prime_sh());
    status_ok("Hook: prime", p.string());
  } catch (const std::exception& e) {
    status_error("Hook: prime", e.what());
    return false;
  }

  std::cout << "\n[3/3] Optional hooks\n";
  HookSelection sel = select_optional_hooks(setup_yes);
  try {
    if (sel.remind) {
      fs::path p = claude_write_hook(config_dir, "user_prompt.sh", mnemon::embedded::claude_user_prompt_sh());
      status_ok("Hook: remind", p.string());
    }
    if (sel.nudge) {
      fs::path p = claude_write_hook(config_dir, "stop.sh", mnemon::embedded::claude_stop_sh());
      status_ok("Hook: nudge", p.string());
    }
    if (sel.compact) {
      fs::path p = claude_write_hook(config_dir, "compact.sh", mnemon::embedded::claude_compact_sh());
      status_ok("Hook: compact", p.string());
    }
  } catch (const std::exception& e) {
    status_error("Hook", e.what());
    return false;
  }

  try {
    fs::path p = claude_register_hooks(config_dir, sel);
    status_updated("Settings", p.string());
  } catch (const std::exception& e) {
    status_error("Settings", e.what());
    return false;
  }

  std::cout << "\nSetup complete!\n  Hooks   ";
  std::cout << "prime";
  if (sel.remind) {
    std::cout << ", remind";
  }
  if (sel.nudge) {
    std::cout << ", nudge";
  }
  if (sel.compact) {
    std::cout << ", compact";
  }
  std::cout << "\n  Prompts " << prompt_path << "/ (guide.md, skill.md)\n\n";
  std::cout << "Start a new Claude Code session to activate.\n";
  std::cout << "Edit " << prompt_path << "/guide.md to customize behavior.\n";
  std::cout << "Run 'mnemon setup --eject' to remove.\n";

  init_default_store(opt.data_dir);
  return true;
}

static bool install_openclaw(Environment env, bool global, bool setup_yes, const RunOptions& opt) {
  std::string config_dir = env.config_dir;
  if (!global && !setup_yes && is_tty_in()) {
    fs::path global_dir = fs::path(home_dir()) / ".openclaw";
    std::vector<std::string> opts = {
        "Global — all projects (" + global_dir.string() + "/)",
        "Local  — this project only (.openclaw/)",
    };
    size_t idx = select_one("Install scope", opts, 0);
    config_dir = (idx == 1) ? std::string(".openclaw") : global_dir.string();
  }

  std::cout << "\nSetting up OpenClaw (" << config_dir << ")...\n";

  std::cout << "\n[1/4] Skill\n";
  try {
    fs::path p = openclaw_write_skill(config_dir);
    status_ok("Skill", p.string());
  } catch (const std::exception& e) {
    status_error("Skill", e.what());
    return false;
  }

  std::cout << "\n[2/4] Prompts\n";
  std::string prompt_path;
  try {
    fs::path p = write_prompt_files();
    status_ok("Prompts", p.string());
    prompt_path = p.string();
  } catch (const std::exception& e) {
    status_error("Prompts", e.what());
    return false;
  }

  std::cout << "\n[3/4] Hook\n";
  try {
    fs::path p = openclaw_write_hook(config_dir);
    status_ok("Hook: prime", p.string());
  } catch (const std::exception& e) {
    status_error("Hook: prime", e.what());
    return false;
  }

  std::cout << "\n[4/4] Plugin\n";
  HookSelection sel = select_openclaw_optional_hooks(setup_yes);
  try {
    fs::path p = openclaw_write_plugin(config_dir, opt.version);
    status_ok("Plugin", p.string());
  } catch (const std::exception& e) {
    status_error("Plugin", e.what());
    return false;
  }
  try {
    fs::path p = openclaw_register_plugin(config_dir, sel);
    status_updated("Config", p.string());
  } catch (const std::exception& e) {
    status_error("Config", e.what());
    return false;
  }

  std::cout << "\nSetup complete!\n";
  std::cout << "  Skill   " << config_dir << "/skills/mnemon/SKILL.md\n";
  std::cout << "  Hook    " << config_dir << "/hooks/mnemon-prime/ (agent:bootstrap)\n";
  std::cout << "  Plugin  " << config_dir << "/extensions/mnemon/ (hooks: ";
  std::cout << "prime";
  if (sel.remind) {
    std::cout << ", remind";
  }
  if (sel.nudge) {
    std::cout << ", nudge";
  }
  if (sel.compact) {
    std::cout << ", compact";
  }
  std::cout << ")\n";
  std::cout << "  Prompts " << prompt_path << "/ (guide.md, skill.md)\n\n";
  std::cout << "Restart the OpenClaw gateway to activate.\n";
  std::cout << "Edit " << prompt_path << "/guide.md to customize behavior.\n";
  std::cout << "Run 'mnemon setup --eject' to remove.\n";

  init_default_store(opt.data_dir);
  return true;
}

static bool install_env(Environment* env, bool global, bool setup_yes, const RunOptions& opt) {
  if (env->name == "claude-code") {
    return install_claude_code(*env, global, setup_yes, opt);
  }
  if (env->name == "codex") {
    return install_codex(*env, global, setup_yes);
  }
  if (env->name == "cursor") {
    return install_cursor(*env, global, setup_yes);
  }
  if (env->name == "trae") {
    return install_trae(*env, global, setup_yes);
  }
  if (env->name == "qoder") {
    return install_qoder(*env, global, setup_yes);
  }
  if (env->name == "qoderwork") {
    return install_qoderwork(*env);
  }
  if (env->name == "codebuddy") {
    return install_codebuddy(*env, global, setup_yes);
  }
  if (env->name == "workbuddy") {
    return install_workbuddy(*env, global, setup_yes);
  }
  if (env->name == "kimi") {
    return install_kimi(*env);
  }
  if (env->name == "openclaw") {
    return install_openclaw(*env, global, setup_yes, opt);
  }
  if (env->name == "nanobot") {
    return install_nanobot(*env, global, setup_yes);
  }
  if (env->name == "pi") {
    return install_pi(*env, global, setup_yes);
  }
  if (env->name == "hermes") {
    return install_hermes(*env, setup_yes, opt);
  }
  return false;
}

static int eject_env(Environment* env, bool yes) {
  if (env->name == "claude-code") {
    return claude_eject(env->config_dir, yes);
  }
  if (env->name == "codex") {
    int errs = codex_eject(env->config_dir);
    eject_markdown("AGENTS.md", "Remove memory guidance from ./AGENTS.md?", yes);
    return errs;
  }
  if (env->name == "cursor") {
    return cursor_eject(env->config_dir);
  }
  if (env->name == "trae") {
    int errs = trae_eject(env->config_dir);
    eject_markdown("AGENTS.md", "Remove memory guidance from ./AGENTS.md?", yes);
    return errs;
  }
  if (env->name == "qoder") {
    int errs = eject_qoder("Qoder", env->config_dir);
    eject_markdown("AGENTS.md", "Remove memory guidance from ./AGENTS.md?", yes);
    return errs;
  }
  if (env->name == "qoderwork") {
    int errs = eject_qoder("QoderWork", env->config_dir);
    eject_markdown("AGENTS.md", "Remove memory guidance from ./AGENTS.md?", yes);
    return errs;
  }
  if (env->name == "codebuddy") {
    int errs = codebuddy_eject(env->config_dir);
    eject_markdown("AGENTS.md", "Remove memory guidance from ./AGENTS.md?", yes);
    return errs;
  }
  if (env->name == "workbuddy") {
    int errs = workbuddy_eject(env->config_dir);
    eject_markdown("AGENTS.md", "Remove memory guidance from ./AGENTS.md?", yes);
    return errs;
  }
  if (env->name == "kimi") {
    int errs = kimi_eject(env->config_dir);
    eject_markdown("AGENTS.md", "Remove memory guidance from ./AGENTS.md?", yes);
    return errs;
  }
  if (env->name == "openclaw") {
    return openclaw_eject(env->config_dir, yes);
  }
  if (env->name == "nanobot") {
    return nanobot_eject(env->config_dir);
  }
  if (env->name == "pi") {
    int errs = pi_eject(env->config_dir);
    eject_markdown("AGENTS.md", "Remove memory guidance from ./AGENTS.md?", yes);
    return errs;
  }
  if (env->name == "hermes") {
    int errs = hermes_eject(env->config_dir);
    eject_markdown("AGENTS.md", "Remove memory guidance from ./AGENTS.md?", yes);
    return errs;
  }
  return 0;
}

static void run_install_flow(const RunOptions& opt) {
  auto envs = detect_environments(opt.global);
  if (!opt.target.empty()) {
    for (auto& e : envs) {
      if (e.name == opt.target) {
        if (!install_env(&e, opt.global, opt.yes, opt)) {
          throw std::runtime_error("setup install failed");
        }
        return;
      }
    }
    throw std::runtime_error("unknown target \"" + opt.target + "\"");
  }

  std::cout << "Detecting LLM CLI environments...\n\n";
  std::vector<Environment*> detected;
  for (auto& e : envs) {
    detection_line(e.detected, e.display, e.version, e.config_dir);
    if (e.detected) {
      detected.push_back(&e);
    }
  }
  if (detected.empty()) {
    std::cout << "\nNo supported LLM CLI environments detected.\n";
    std::cout << "Install Claude Code, Codex, OpenClaw, Nanobot, Pi, or Hermes Agent, then run 'mnemon setup' again.\n";
    return;
  }

  std::vector<Environment*> selected;
  if (opt.yes) {
    selected = detected;
  } else if (is_tty_in()) {
    std::vector<std::string> options;
    for (auto* e : detected) {
      options.push_back(e->display);
    }
    size_t idx = select_one("Select environment", options, 0);
    selected.push_back(detected[idx]);
  } else {
    selected = detected;
  }

  if (selected.empty()) {
    std::cout << "\nNo environments selected.\n";
    return;
  }

  int err_count = 0;
  for (auto* e : selected) {
    if (!install_env(e, opt.global, opt.yes, opt)) {
      ++err_count;
    }
  }
  if (err_count > 0) {
    throw std::runtime_error(std::to_string(err_count) + " error(s) during setup");
  }
}

static void run_eject_flow(const RunOptions& opt) {
  auto envs = detect_environments(opt.global);
  if (!opt.target.empty()) {
    for (auto& e : envs) {
      if (e.name == opt.target) {
        int errs = eject_env(&e, opt.yes);
        if (errs > 0) {
          throw std::runtime_error("eject completed with errors");
        }
        return;
      }
    }
    throw std::runtime_error("unknown target \"" + opt.target + "\"");
  }

  std::cout << "Detecting LLM CLI environments...\n\n";
  std::vector<Environment*> installed;
  for (auto& e : envs) {
    detection_line(e.detected, e.display, e.version, e.config_dir);
    if (e.detected) {
      installed.push_back(&e);
    }
  }
  if (installed.empty()) {
    std::cout << "\nNo environments detected.\n";
    eject_local_markdown(opt.yes);
    return;
  }

  std::vector<Environment*> selected;
  if (opt.yes) {
    selected = installed;
  } else if (is_tty_in()) {
    std::vector<std::string> options;
    for (auto* e : installed) {
      options.push_back(e->display);
    }
    size_t idx = select_one("Select environment to remove", options, 0);
    selected.push_back(installed[idx]);
  } else {
    selected = installed;
  }

  if (selected.empty()) {
    std::cout << "\nNo environments selected.\n";
    return;
  }

  int err_count = 0;
  for (auto* e : selected) {
    if (eject_env(e, opt.yes) > 0) {
      ++err_count;
    }
  }
  std::cout << "\nDone! All selected integrations removed.\n";
  if (err_count > 0) {
    throw std::runtime_error(std::to_string(err_count) + " error(s) during eject");
  }
}

} // namespace

void run(const RunOptions& opt) {
  if (!opt.target.empty() && opt.target != "claude-code" && opt.target != "codex" && opt.target != "cursor" && opt.target != "trae" && opt.target != "qoder" && opt.target != "qoderwork" && opt.target != "codebuddy" && opt.target != "workbuddy" && opt.target != "kimi" && opt.target != "openclaw" && opt.target != "nanobot" && opt.target != "pi" && opt.target != "hermes") {
    throw std::runtime_error("invalid target \"" + opt.target + "\" (must be claude-code, codex, cursor, trae, qoder, qoderwork, codebuddy, workbuddy, kimi, openclaw, nanobot, pi, or hermes)");
  }
  if (opt.eject) {
    run_eject_flow(opt);
  } else {
    run_install_flow(opt);
  }
}

} // namespace mnemon::setup
