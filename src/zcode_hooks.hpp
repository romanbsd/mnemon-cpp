#pragma once

// Platform-varying pieces of ZCode's cli/config.json hook registration.
// Split out so the Windows (powershell) vs POSIX (bash) argv shapes are
// unit-testable without dragging the rest of setup.cpp into the test binary.
#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace mnemon::setup {

// Host GOOS as ZCode hook selection cares about it: "windows" on Windows,
// "posix" everywhere else (only the ==\"windows\" distinction matters).
std::string_view zcode_host_goos();

// Hook script filename for a base name under the given GOOS
// ("windows" -> "<base>.ps1", otherwise "<base>.sh").
std::string zcode_hook_filename(std::string_view base, std::string_view goos);

// One ZCode process-hook object. On windows: powershell.exe with
// -NoProfile -NonInteractive -ExecutionPolicy Bypass -File <script>;
// otherwise: bash <script>.
nlohmann::json zcode_process_hook(std::string_view script_path, std::string_view status, std::string_view goos);

} // namespace mnemon::setup
