#pragma once

#include <string>
#include <vector>

namespace mnemon::paths {

std::string default_data_dir();
bool valid_store_name(const std::string& name);
std::string store_dir(const std::string& base, const std::string& name);
std::string active_file(const std::string& base);
std::string read_active(const std::string& base);
bool write_active(const std::string& base, const std::string& name);
std::vector<std::string> list_stores(const std::string& base);
bool store_exists(const std::string& base, const std::string& name);
/** Legacy ~/.mnemon/mnemon.db → data/default/. Prints migration line to stderr. */
bool migrate_if_needed(const std::string& base, bool readonly_mode);

} // namespace mnemon::paths
