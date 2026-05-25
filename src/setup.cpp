// Interactive and `--json` setup: detect host app, merge hook configs, install/eject embedded assets (go:embed parity).
#include "setup.hpp"

#include "db.hpp"
#include "embedded_assets.hpp"
#include "paths.hpp"

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

static std::vector<Environment> detect_environments(bool global) {
  return {detect_claude(global), detect_openclaw(global)};
}

// --- install pieces ---

static fs::path write_prompt_files() {
  fs::path prompt_dir = fs::path(home_dir()) / ".mnemon" / "prompt";
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

static fs::path claude_register_hooks(const std::string& config_dir, const HookSelection& sel) {
  fs::path hooks_dir = fs::path(config_dir) / "hooks" / "mnemon";
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
  try {
    fs::path p = write_prompt_files();
    status_ok("Prompts", p.string());
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
  std::cout << "\n  Prompts ~/.mnemon/prompt/ (guide.md, skill.md)\n\n";
  std::cout << "Start a new Claude Code session to activate.\n";
  std::cout << "Edit ~/.mnemon/prompt/guide.md to customize behavior.\n";
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
  try {
    fs::path p = write_prompt_files();
    status_ok("Prompts", p.string());
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
  std::cout << "  Prompts ~/.mnemon/prompt/ (guide.md, skill.md)\n\n";
  std::cout << "Restart the OpenClaw gateway to activate.\n";
  std::cout << "Edit ~/.mnemon/prompt/guide.md to customize behavior.\n";
  std::cout << "Run 'mnemon setup --eject' to remove.\n";

  init_default_store(opt.data_dir);
  return true;
}

static bool install_env(Environment* env, bool global, bool setup_yes, const RunOptions& opt) {
  if (env->name == "claude-code") {
    return install_claude_code(*env, global, setup_yes, opt);
  }
  if (env->name == "openclaw") {
    return install_openclaw(*env, global, setup_yes, opt);
  }
  return false;
}

static int eject_env(Environment* env, bool yes) {
  if (env->name == "claude-code") {
    return claude_eject(env->config_dir, yes);
  }
  if (env->name == "openclaw") {
    return openclaw_eject(env->config_dir, yes);
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
    std::cout << "Install Claude Code or OpenClaw, then run 'mnemon setup' again.\n";
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
  if (!opt.target.empty() && opt.target != "claude-code" && opt.target != "openclaw") {
    throw std::runtime_error("invalid target \"" + opt.target + "\" (must be claude-code or openclaw)");
  }
  if (opt.eject) {
    run_eject_flow(opt);
  } else {
    run_install_flow(opt);
  }
}

} // namespace mnemon::setup
