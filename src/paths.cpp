#include "paths.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace mnemon::paths {

static const char* kDefaultStore = "default";

// Resolution order matches Go: env override, then ~/.mnemon, then a temp fallback.
std::string default_data_dir() {
  if (const char* env = std::getenv("MNEMON_DATA_DIR"); env && *env) {
    return env;
  }
  if (const char* home = std::getenv("HOME"); home && *home) {
    return (fs::path(home) / ".mnemon").string();
  }
  return "/tmp/.mnemon";
}

bool valid_store_name(const std::string& name) {
  static const std::regex re(R"(^[a-zA-Z0-9][a-zA-Z0-9_-]*$)");
  return std::regex_match(name, re);
}

std::string store_dir(const std::string& base, const std::string& name) {
  return (fs::path(base) / "data" / name).string();
}

std::string active_file(const std::string& base) {
  return (fs::path(base) / "active").string();
}

std::string read_active(const std::string& base) {
  std::ifstream in(active_file(base));
  if (!in) {
    return kDefaultStore;
  }
  std::string line;
  std::getline(in, line);
  // trim
  size_t a = line.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) {
    return kDefaultStore;
  }
  size_t b = line.find_last_not_of(" \t\r\n");
  line = line.substr(a, b - a + 1);
  if (line.empty()) {
    return kDefaultStore;
  }
  return line;
}

bool write_active(const std::string& base, const std::string& name) {
  std::error_code ec;
  fs::create_directories(base, ec);
  if (ec) {
    return false;
  }
  std::ofstream out(active_file(base), std::ios::trunc);
  if (!out) {
    return false;
  }
  out << name << '\n';
  return true;
}

std::vector<std::string> list_stores(const std::string& base) {
  std::vector<std::string> names;
  fs::path data_dir = fs::path(base) / "data";
  std::error_code ec;
  if (!fs::exists(data_dir, ec)) {
    return names;
  }
  for (const auto& e : fs::directory_iterator(data_dir, ec)) {
    if (e.is_directory()) {
      names.push_back(e.path().filename().string());
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

bool store_exists(const std::string& base, const std::string& name) {
  std::error_code ec;
  return fs::is_directory(store_dir(base, name), ec);
}

// Legacy layout: single ~/.mnemon/mnemon.db → ~/.mnemon/data/default/mnemon.db (+ WAL sidecars).
// Skip when readonly so we never mutate disk from a read-only open.
bool migrate_if_needed(const std::string& base, bool readonly_mode) {
  if (readonly_mode) {
    return true;
  }
  fs::path old_db = fs::path(base) / "mnemon.db";
  fs::path new_dir = fs::path(store_dir(base, kDefaultStore));
  fs::path new_db = new_dir / "mnemon.db";
  std::error_code ec;
  if (!fs::exists(old_db, ec)) {
    return true;
  }
  if (fs::exists(new_db, ec)) {
    return true;
  }
  fs::create_directories(new_dir, ec);
  if (ec) {
    return false;
  }
  fs::rename(old_db, new_db, ec);
  if (ec) {
    return false;
  }
  fs::path old_wal = fs::path(base) / "mnemon.db-wal";
  fs::path old_shm = fs::path(base) / "mnemon.db-shm";
  if (fs::exists(old_wal, ec)) {
    fs::rename(old_wal, fs::path(new_db.string() + "-wal"), ec);
  }
  if (fs::exists(old_shm, ec)) {
    fs::rename(old_shm, fs::path(new_db.string() + "-shm"), ec);
  }
  std::cerr << "mnemon: migrated database to " << new_db.string() << "\n";
  return true;
}

} // namespace mnemon::paths
