# mnemon (C++23)

Standalone CLI and engine port of [Mnemon GO](https://github.com/mnemon-dev/mnemon)
Useful for memory and disk space constrained systems.
Behavior is defined by [mnemon-spec.md](mnemon-spec.md).

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

## Local Nomic v1.5 embeddings

Mnemon supports two local embedding APIs:

- `ollama` (default): `GET /api/tags` and `POST /api/embed`
- `llama.cpp`: `GET /health` and `POST /v1/embeddings`

Select the API with `MNEMON_EMBED_API`. The model server runs separately from the short-lived `mnemon` process,
so the model can remain loaded for a burst of commands and unload after an inactivity timeout.

### llama.cpp with idle model unloading

Install llama.cpp with Homebrew:

```bash
brew install llama.cpp
```

Start a local embedding-only server. On the first run, `-hf` downloads the Q8 Nomic v1.5 GGUF into llama.cpp's
cache; subsequent starts use the cached file.

```bash
llama-server \
  --host 127.0.0.1 \
  --port 11435 \
  --embedding \
  --pooling mean \
  -hf nomic-ai/nomic-embed-text-v1.5-GGUF:Q8_0 \
  --alias nomic-embed-text \
  --sleep-idle-seconds 600
```

Leave the process running. After 600 seconds without inference requests, llama-server enters its sleeping state
and unloads the model. The HTTP server remains alive, and Mnemon's next embedding request reloads the model
automatically. To keep the server running after the terminal closes:

```bash
mkdir -p "$HOME/.mnemon"
nohup llama-server \
  --host 127.0.0.1 \
  --port 11435 \
  --embedding \
  --pooling mean \
  -hf nomic-ai/nomic-embed-text-v1.5-GGUF:Q8_0 \
  --alias nomic-embed-text \
  --sleep-idle-seconds 600 \
  >"$HOME/.mnemon/llama-server.log" 2>&1 &
```

Configure every environment that invokes Mnemon, including IDEs and agent hooks:

```bash
export MNEMON_EMBED_API=llama.cpp
export MNEMON_EMBED_ENDPOINT=http://127.0.0.1:11435
export MNEMON_EMBED_MODEL=nomic-embed-text
unset MNEMON_EMBED_DIMENSIONS
```

Mnemon automatically sends `search_document: ` for stored memories and `search_query: ` for recall queries, as
required by Nomic v1.5. Leaving `MNEMON_EMBED_DIMENSIONS` unset uses the native 768 dimensions. A positive value
requests a smaller Matryoshka vector:

```bash
export MNEMON_EMBED_DIMENSIONS=256
```

Some llama-server versions accept `dimensions` but still return the native 768 values. Mnemon handles this by
keeping the leading requested dimensions and normalizing the shortened vector; it rejects a response that is
shorter than requested.

Verify connectivity without writing to the database:

```bash
./build/mnemon embed --status
```

The JSON response must contain:

```json
{
  "embedding_available": true,
  "ollama_available": true,
  "model": "nomic-embed-text"
}
```

`ollama_available` is retained as a legacy compatibility field; it reflects the selected embedding service even
when `MNEMON_EMBED_API=llama.cpp`.

Backfill active memories that do not yet have an embedding:

```bash
./build/mnemon embed --all
```

Changing dimensions after embeddings have already been stored requires regenerating all embeddings so vectors
remain comparable. Stopping llama-server disables embeddings, but Mnemon still supports keyword and graph recall.

### Ollama

Ollama remains the default, so `MNEMON_EMBED_API` may be omitted. Install [Ollama](https://ollama.com/download),
download Nomic v1.5, and run the daemon with a ten-minute model keep-alive:

```bash
ollama pull nomic-embed-text:v1.5
OLLAMA_KEEP_ALIVE=10m ollama serve
```

In the shell that runs Mnemon:

```bash
export MNEMON_EMBED_API=ollama
export MNEMON_EMBED_ENDPOINT=http://127.0.0.1:11434
export MNEMON_EMBED_MODEL=nomic-embed-text:v1.5
```

Use `ollama ps` to inspect loaded models. `OLLAMA_KEEP_ALIVE` accepts durations such as `5m`, `30m`, or `24h`.

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
