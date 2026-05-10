#pragma once

#include <string>

namespace mnemon::setup {

struct RunOptions {
  std::string data_dir;
  const char* version = "dev";
  std::string target; // "" | "claude-code" | "openclaw"
  bool eject = false;
  bool yes = false;
  bool global = false;
};

/** Full Claude Code / OpenClaw setup or eject (matches Go cmd/setup + internal/setup). */
void run(const RunOptions& opt);

} // namespace mnemon::setup
