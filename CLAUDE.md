# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

`mnemon-cpp` is a **C++23 port of the Go `mnemon` binary** — a single-binary memory daemon CLI for LLM agents (Claude Code, OpenClaw, etc.) backed by SQLite. The host LLM is the supervisor; this binary does deterministic storage, graph indexing, retrieval, and lifecycle decay — no embedded model, no API keys, no network calls except optional localhost Ollama for embeddings.

**This port is a drop-in replacement, not a rewrite.** The Go binary is the reference; the C++ binary must be byte-compatible with it on every observable surface:

- CLI shape (commands, flags, positional args, exit codes, stdout JSON keys/values)
- SQLite schema and row contents (existing Go-written DBs must open and round-trip cleanly)
- On-disk layout under `MNEMON_DATA_DIR`, including the legacy `~/.mnemon/mnemon.db` migration
- Bytes written by `mnemon setup` (hooks, skills, plugin manifests, `settings.json` blocks)

**`mnemon-spec.md` is the authoritative behavioral contract.** Treat it as source of truth before reading code. `scripts/e2e_test.sh` is the conformance test — if it passes, the drop-in contract holds.

## Build, test, run

CMake is the source of truth. `Makefile` is convenience wrappers.

```bash
make build                 # cmake configure + build → ./build/mnemon
make unit                  # ctest in build/ (currently just a Catch2 smoke test)
make test                  # full E2E (rebuilds via CMake, then runs scripts/e2e_test.sh; needs jq)
make clean                 # rm -rf build .testdata

# Skip rebuild when iterating on the harness:
MNEMON_TEST_BINARY="$PWD/build/mnemon" bash scripts/e2e_test.sh

# Single E2E section: there is no flag — comment out the other `banner` blocks
# in scripts/e2e_test.sh, or copy the milestone's commands and run them by hand
# against $MNEMON_TEST_BINARY with --data-dir pointed at a scratch dir.
```

First configure downloads the SQLite amalgamation into `build/` and `FetchContent`-pulls CLI11, nlohmann/json, cpp-httplib, and Catch2. CI runs the same flow on Ubuntu with gcc-13 (`.github/workflows/ci.yml`).

Useful runtime env vars (all read by the binary, not the build):

- `MNEMON_DATA_DIR` — base data dir (default `$HOME/.mnemon`)
- `MNEMON_STORE` — named store override (also: `--store`, or the `active` file)
- `MNEMON_EMBED_ENDPOINT` / `MNEMON_EMBED_MODEL` / `MNEMON_EMBED_DIMENSIONS` — Ollama config

## Architecture (the parts you can't see by `ls`-ing src/)

### Five layers, strict direction of dependency

```
Integration   setup_assets/ (hooks, skills, guides) → embedded into binary, copied to disk by `setup`
CLI           src/main.cpp → src/commands.cpp (CLI11 wiring; ALL command handlers + JSON output)
Engine        recall.cpp, intent.cpp, diff.cpp, graph_*.cpp, keyword.cpp, vector_math.cpp, tokenize.cpp
Storage       db.cpp (SQLite WAL, schema, transactions, oplog), paths.cpp, model.cpp, time_util.cpp
External      ollama.cpp (HTTP via cpp-httplib, localhost-only by default)
```

**Module boundaries are enforced by convention, not by the build:**

- Engine code must not reach into CLI parsing.
- Storage code must not reach into engine algorithms.
- **The Ollama client must never be called while holding a SQL transaction** (`Database::in_transaction`). Embedding calls happen *before* the transaction in `remember`, and `semantic_candidates` / `causal_candidates` are computed *outside* it for the JSON response. See spec §2.2, §7.3, §7.6.

### `src/commands.cpp` is the contract

All CLI parsing, validation, JSON shaping, and command dispatch live there in one large `run_mnemon(argc, argv)` function with a CLI11 subcommand per verb (`remember`, `recall`, `search`, `link`, `related`, `forget`, `gc`, `embed`, `status`, `log`, `store {list,create,set,remove}`, `setup`, `viz`). When changing command behavior, the JSON keys and validation messages are part of the drop-in contract — match the Go binary's wording (e.g. `"invalid category \"<x>\"; valid: preference, decision, fact, insight, context, general"`).

### Data model & SQLite (spec §4–§5)

Schema is created idempotently on every writable open in `Database::migrate()`. Includes additive `ALTER TABLE`s and a one-time *narrative-edges cleanup* that rebuilds the `edges` table if it predates the four-type CHECK. Edge types are exactly four: `temporal | semantic | causal | entity`. Categories are exactly six: `preference | decision | fact | insight | context | general`. Insights are soft-deleted (`deleted_at`); auto-prune cap is `kMaxInsights = 1000`.

Embeddings are stored as little-endian IEEE-754 binary64 blobs, length `N*8`. Vectors of mismatched dimension are not validated — cosine similarity simply returns 0.

### Embedded setup assets (`setup_assets/`)

Files under `setup_assets/{claude,openclaw,nanoclaw}` are **vendored verbatim from the upstream Go monorepo** (`internal/setup/assets`) and compiled into the binary at configure time:

- `tools/gen_embedded_assets.py` reads the files listed in `ASSET_FILES` (must match the Go `go:embed` list in `setup_assets/assets.go`) and emits `build/embedded_assets.{hpp,cpp}` containing `unsigned char` arrays + `string_view` accessors in `mnemon::embedded::`.
- `CMakeLists.txt` declares the same file list under `ASSET_DEPS` so `cmake --build` regenerates when any asset changes.
- Both lists must stay in sync. Adding/removing an asset requires editing **three** places: `setup_assets/assets.go` (parity reference), `tools/gen_embedded_assets.py` `ASSET_FILES`, and `CMakeLists.txt` `ASSET_DEPS`.

Assets must be **byte-identical** to upstream so `mnemon setup` writes the same files as the Go binary. To refresh after upstream changes:

```bash
bash scripts/sync_setup_assets_from_monorepo.sh
# Default: shallow-clones https://github.com/mnemon-dev/mnemon into .cache/
# Or:    MNEMON_UPSTREAM_ASSETS=/path/to/mnemon/internal/setup/assets bash scripts/sync_setup_assets_from_monorepo.sh
```

Then rebuild so CMake regenerates the embedded files.

### Stores and the `active` file

A "store" is a named subdirectory `data/<name>/mnemon.db` under `MNEMON_DATA_DIR`. `--store` flag > `MNEMON_STORE` env > contents of `<base>/active` (default `default`). Store name regex is `^[a-zA-Z0-9][a-zA-Z0-9_-]*$`. `paths::migrate_if_needed()` handles the legacy `<base>/mnemon.db` → `<base>/data/default/mnemon.db` move and runs on every writable open; `--readonly` skips it.

## Conventions worth knowing

- **JSON parity over idiomatic C++.** When in doubt about output shape, run the Go binary on the same input and diff. The `e2e_test.sh` `assert_jq` calls encode the contract for most commands.
- **CLI parse errors throw `CLI::ValidationError`**; runtime/db errors throw `std::runtime_error`. Both are caught at the bottom of `run_mnemon` — `ValidationError` exits via `app.exit()` (CLI11's standard nonzero), other exceptions print to stderr and `return 1`.
- **`db->log_op(...)` after every mutating command** — the `log` subcommand is part of the user-visible contract (E2E asserts on it).
- The unit test target (`tests/smoke_test.cpp`) is currently a single Catch2 placeholder. Real coverage lives in `scripts/e2e_test.sh`. If you add a Catch2 test, wire it into the existing `cpp_tests` executable in `CMakeLists.txt` rather than creating a new target.
- `.testdata/` is created and wiped by `e2e_test.sh` on each run; never check it in (already gitignored).

## Staying in sync with upstream Go binary

The upstream Go reference (`https://github.com/mnemon-dev/mnemon`) is the source of truth for the parity contract. New upstream commits are ported by the `/upstream-sync` skill (`.claude/skills/upstream-sync/SKILL.md`). The skill processes new commits in one autonomous batch, opens one stacked PR per commit that needs porting, and stops without merging — the human merges in order.

State files (committed at repo root):

- `.upstream-sync` — `key=value` tracker recording the last upstream SHA evaluated. Bumped by `scripts/upstream_sync.sh advance <sha>` inside each port-PR. Re-seed with `scripts/upstream_sync.sh init --force` only if upstream history was rewritten.
- `.upstream-tag-pending` — `<tag>=<upstream-sha>` sidecar listing upstream tags whose port-PR hasn't been merged yet. Cleared by `scripts/upstream_release_tags.sh` after merges.

Each port-PR's commit and body carry an `Upstream-Commit: <full-sha>` trailer; this is load-bearing for tag mirroring. Don't reformat or drop it.

Helper tests run via `make test-helper`.
