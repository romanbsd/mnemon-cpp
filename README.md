# mnemon (C++23)

Standalone CLI and engine port. Behavior is defined by [mnemon-spec.md](mnemon-spec.md).

**Layout:** This directory **is** the repository root (`mnemon-cpp`): CMake, `src/`, vendored `setup_assets/`, `scripts/e2e_test.sh`, and CI all live here.

## Toolchain

- **CMake** 3.24+
- **C++23** compiler (Apple Clang / GCC 13+ / Clang 17+)
- **Python** 3.8+ (configure-time only: generates embedded bytes from `setup_assets/`)
- **jq** (for the E2E harness only)

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/mnemon --version
```

Or: `make build`

SQLite amalgamation is downloaded into `build/`; **FetchContent** supplies CLI11, nlohmann/json, cpp-httplib, and Catch2.

## Tests

```bash
make unit          # ctest in build/
make test          # full E2E (builds via CMake unless MNEMON_TEST_BINARY is set)
```

Or manually:

```bash
cd build && ctest --output-on-failure
bash scripts/e2e_test.sh
MNEMON_TEST_BINARY="$PWD/build/mnemon" bash scripts/e2e_test.sh   # skip rebuild
```

## Embedded setup assets

Files under `setup_assets/` are compiled into the binary (same bytes as the reference Go `go:embed` tree). They are **vendored in-repo** (not a submodule): small, rarely change, and must stay byte-identical for drop-in `setup` behavior.

After upstream hook/skill changes in the reference **mnemon** repo, refresh vendored bytes. Default assumption: **mnemon** is cloned as a sibling (`../mnemon`, same parent directory as this repo):

```bash
bash scripts/sync_setup_assets_from_monorepo.sh
```

Or point at any checkout: `MNEMON_UPSTREAM_ASSETS=/path/to/mnemon/internal/setup/assets bash scripts/sync_setup_assets_from_monorepo.sh`

Then rebuild so CMake regenerates `embedded_assets.*`.

## Test data / submodules

**No git submodule** for tests: `scripts/e2e_test.sh` creates an isolated `.testdata/` directory and deletes it at the start of each run. Nothing to fetch beyond `jq` and a normal build.

## Dependencies (third-party)

| Library             | Use         | License (typical) |
|---------------------|------------|-------------------|
| SQLite amalgamation | Storage    | Public domain     |
| CLI11               | CLI        | BSD-3-Clause      |
| nlohmann/json       | JSON I/O   | MIT               |
| cpp-httplib         | Ollama HTTP| MIT               |
| Catch2              | Unit tests | BSL-1.0           |

See [LICENSE](LICENSE).
