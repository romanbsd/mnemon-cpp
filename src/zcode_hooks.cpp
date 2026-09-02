#include "zcode_hooks.hpp"

namespace mnemon::setup {

std::string_view zcode_host_goos() {
#ifdef _WIN32
  return "windows";
#else
  return "posix";
#endif
}

std::string zcode_hook_filename(std::string_view base, std::string_view goos) {
  std::string out(base);
  out += (goos == "windows") ? ".ps1" : ".sh";
  return out;
}

nlohmann::json zcode_process_hook(std::string_view script_path, std::string_view status, std::string_view goos) {
  std::string command = "bash";
  nlohmann::json args = nlohmann::json::array({std::string(script_path)});
  if (goos == "windows") {
    command = "powershell.exe";
    args = nlohmann::json::array(
        {"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", std::string(script_path)});
  }
  return nlohmann::json{{"type", "process"},
                        {"command", command},
                        {"args", args},
                        {"enabled", true},
                        {"timeoutMs", 30000},
                        {"statusMessage", std::string(status)}};
}

} // namespace mnemon::setup
