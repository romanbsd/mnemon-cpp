#!/usr/bin/env python3
"""Generate embedded_assets.hpp / embedded_assets.cpp from setup_assets/ (byte-for-byte)."""
import pathlib
import sys

# Must match setup_assets/assets.go go:embed list (kept for parity with the Go binary).
ASSET_FILES = [
    "claude/user_prompt.sh",
    "claude/stop.sh",
    "claude/prime.sh",
    "claude/compact.sh",
    "claude/SKILL.md",
    "claude/guide.md",
    "openclaw/SKILL.md",
    "openclaw/hooks/mnemon-prime/HOOK.md",
    "openclaw/hooks/mnemon-prime/handler.js",
    "openclaw/plugin/package.json",
    "openclaw/plugin/openclaw.plugin.json",
    "openclaw/plugin/index.js",
    "nanoclaw/SKILL.md",
    "nanoclaw/container-skill.md",
    "nanobot/SKILL.md",
    "pi/SKILL.md",
    "pi/mnemon.ts",
    "hermes/SKILL.md",
    "hermes/prime.sh",
    "hermes/remind.sh",
    "hermes/nudge.sh",
    "hermes/compact.sh",
    "codex/SKILL.md",
    "codex/prime.sh",
    "codex/stop.sh",
    "codex/user_prompt.sh",
    "cursor/SKILL.md",
    "cursor/prime.sh",
    "cursor/stop.sh",
    "cursor/compact.sh",
    "trae/SKILL.md",
    "trae/prime.sh",
    "trae/user_prompt.sh",
    "trae/stop.sh",
]


def symbol_array(rel: str) -> str:
    return "k_" + rel.replace("/", "_").replace(".", "_").replace("-", "_")


def cxx_array(name: str, data: bytes) -> str:
    lines = [f"alignas(1) static const unsigned char {name}[] = {{"]
    for i in range(0, len(data), 16):
        chunk = data[i : i + 16]
        hx = ", ".join(f"0x{b:02x}" for b in chunk)
        lines.append(f"  {hx},")
    lines.append("};")
    return "\n".join(lines)


def accessor_fn(cxx_name: str, arr: str) -> str:
    return (
        f"std::string_view {cxx_name}() {{\n"
        f"  return std::string_view(reinterpret_cast<const char*>({arr}), sizeof({arr}));\n"
        f"}}"
    )


def main() -> None:
    if len(sys.argv) != 4:
        print("usage: gen_embedded_assets.py <assets_root> <out.hpp> <out.cpp>", file=sys.stderr)
        sys.exit(1)
    root = pathlib.Path(sys.argv[1])
    out_hpp = pathlib.Path(sys.argv[2])
    out_cpp = pathlib.Path(sys.argv[3])

    arrays: list[str] = []
    accessors_cpp: list[str] = []
    decls_hpp: list[str] = []

    for rel in ASSET_FILES:
        path = root / rel
        data = path.read_bytes()
        arr = symbol_array(rel)
        # C++ function name: claude_user_prompt_sh from claude/user_prompt.sh
        fn = arr[2:] if arr.startswith("k_") else arr
        arrays.append(cxx_array(arr, data))
        accessors_cpp.append(accessor_fn(fn, arr))
        decls_hpp.append(f"std::string_view {fn}();")

    hpp = "\n".join(
        [
            "#pragma once",
            "#include <string_view>",
            "",
            "namespace mnemon::embedded {",
            "",
            *("  " + d for d in decls_hpp),
            "",
            "}  // namespace mnemon::embedded",
            "",
        ]
    )

    cpp = "\n".join(
        [
            '#include "embedded_assets.hpp"',
            "",
            "namespace mnemon::embedded {",
            "namespace {",
            "",
            *arrays,
            "",
            "}  // namespace",
            "",
            *accessors_cpp,
            "",
            "}  // namespace mnemon::embedded",
            "",
        ]
    )

    out_hpp.write_text(hpp, encoding="utf-8")
    out_cpp.write_text(cpp, encoding="utf-8")


if __name__ == "__main__":
    main()
