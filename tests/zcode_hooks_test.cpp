// ZCode hook argv/filename shapes are platform-specific (upstream 0fcb3b69).
// CI runs POSIX only, so cover both GOOS branches with the goos parameter directly.
#include "zcode_hooks.hpp"

#include <catch2/catch_test_macros.hpp>

using mnemon::setup::zcode_hook_filename;
using mnemon::setup::zcode_process_hook;

TEST_CASE("zcode_hook_filename picks extension by goos") {
  CHECK(zcode_hook_filename("prime", "posix") == "prime.sh");
  CHECK(zcode_hook_filename("user_prompt", "linux") == "user_prompt.sh");
  CHECK(zcode_hook_filename("stop", "windows") == "stop.ps1");
}

TEST_CASE("zcode_process_hook POSIX shape: bash <script>") {
  auto h = zcode_process_hook("/hooks/mnemon/prime.sh", "Loading Mnemon context", "linux");
  CHECK(h["type"] == "process");
  CHECK(h["command"] == "bash");
  CHECK(h["args"] == nlohmann::json::array({"/hooks/mnemon/prime.sh"}));
  CHECK(h["enabled"] == true);
  CHECK(h["timeoutMs"] == 30000);
  CHECK(h["statusMessage"] == "Loading Mnemon context");
}

TEST_CASE("zcode_process_hook Windows shape: powershell.exe -File <script>") {
  auto h = zcode_process_hook("C\\\\hooks\\\\mnemon\\\\prime.ps1", "Loading Mnemon context", "windows");
  CHECK(h["command"] == "powershell.exe");
  CHECK(h["args"] == nlohmann::json::array({"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                                            "-File", "C\\\\hooks\\\\mnemon\\\\prime.ps1"}));
  CHECK(h["type"] == "process");
  CHECK(h["timeoutMs"] == 30000);
}
