cmake_minimum_required(VERSION 3.24)
project(mnemon VERSION 0.1.3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

include(FetchContent)

# --- Embedded setup assets (vendored under setup_assets/) ---
set(ASSET_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/setup_assets")
set(EMBED_GEN_HPP "${CMAKE_CURRENT_BINARY_DIR}/embedded_assets.hpp")
set(EMBED_GEN_CPP "${CMAKE_CURRENT_BINARY_DIR}/embedded_assets.cpp")
set(ASSET_DEPS
  "${ASSET_ROOT}/claude/user_prompt.sh"
  "${ASSET_ROOT}/claude/stop.sh"
  "${ASSET_ROOT}/claude/prime.sh"
  "${ASSET_ROOT}/claude/compact.sh"
  "${ASSET_ROOT}/claude/SKILL.md"
  "${ASSET_ROOT}/claude/guide.md"
  "${ASSET_ROOT}/openclaw/SKILL.md"
  "${ASSET_ROOT}/openclaw/hooks/mnemon-prime/HOOK.md"
  "${ASSET_ROOT}/openclaw/hooks/mnemon-prime/handler.js"
  "${ASSET_ROOT}/openclaw/plugin/package.json"
  "${ASSET_ROOT}/openclaw/plugin/openclaw.plugin.json"
  "${ASSET_ROOT}/openclaw/plugin/index.js"
  "${ASSET_ROOT}/nanoclaw/SKILL.md"
  "${ASSET_ROOT}/nanoclaw/container-skill.md"
)
add_custom_command(
  OUTPUT ${EMBED_GEN_HPP} ${EMBED_GEN_CPP}
  COMMAND ${Python3_EXECUTABLE}
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_embedded_assets.py"
          "${ASSET_ROOT}" "${EMBED_GEN_HPP}" "${EMBED_GEN_CPP}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_embedded_assets.py" ${ASSET_DEPS}
  VERBATIM
)

# --- SQLite amalgamation ---
set(SQLITE_VERSION 3530100)
set(SQLITE_ZIP "${CMAKE_BINARY_DIR}/sqlite-amalgamation-${SQLITE_VERSION}.zip")
set(SQLITE_SRC "${CMAKE_BINARY_DIR}/sqlite-amalgamation-${SQLITE_VERSION}")
if(NOT EXISTS "${SQLITE_SRC}/sqlite3.c")
  file(DOWNLOAD
    "https://sqlite.org/2026/sqlite-amalgamation-${SQLITE_VERSION}.zip"
    "${SQLITE_ZIP}"
    SHOW_PROGRESS
    TLS_VERIFY ON
  )
  file(ARCHIVE_EXTRACT INPUT "${SQLITE_ZIP}" DESTINATION "${CMAKE_BINARY_DIR}")
endif()
add_library(sqlite3 STATIC "${SQLITE_SRC}/sqlite3.c")
target_include_directories(sqlite3 PUBLIC "${SQLITE_SRC}")
if(MSVC)
  target_compile_definitions(sqlite3 PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(sqlite3 PRIVATE -Wno-unused-but-set-variable)
endif()

FetchContent_Declare(CLI11
  GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
  GIT_TAG v2.6.2
)
FetchContent_MakeAvailable(CLI11)

FetchContent_Declare(json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0
)
FetchContent_MakeAvailable(json)

FetchContent_Declare(httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.44.0
)
FetchContent_MakeAvailable(httplib)

FetchContent_Declare(Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.14.0
)
FetchContent_MakeAvailable(Catch2)

add_executable(mnemon
  src/main.cpp
  src/paths.cpp
  src/db.cpp
  src/model.cpp
  src/time_util.cpp
  src/model_json.cpp
  src/tokenize.cpp
  src/keyword.cpp
  src/diff.cpp
  src/intent.cpp
  src/recall.cpp
  src/vector_math.cpp
  src/ollama.cpp
  src/uuid.cpp
  src/graph_extract.cpp
  src/graph_temporal.cpp
  src/graph_entity.cpp
  src/graph_causal.cpp
  src/graph_semantic.cpp
  src/graph_engine.cpp
  src/graph_bfs.cpp
  src/setup.cpp
  src/viz.cpp
  src/commands.cpp
  ${EMBED_GEN_CPP}
)
target_include_directories(mnemon PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(mnemon PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(mnemon PRIVATE "${httplib_SOURCE_DIR}")
target_compile_options(mnemon PRIVATE "-DMNEMON_VERSION_STR=\"${PROJECT_VERSION}\"")
target_link_libraries(mnemon PRIVATE CLI11::CLI11 nlohmann_json::nlohmann_json sqlite3 Threads::Threads)
if(APPLE)
  target_link_libraries(mnemon PRIVATE "-framework Accelerate")
endif()

option(BUILD_TESTING "Build unit tests" ON)
if(BUILD_TESTING)
  enable_testing()
  add_executable(cpp_tests tests/smoke_test.cpp src/vector_math.cpp)
  target_include_directories(cpp_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(cpp_tests PRIVATE Catch2::Catch2WithMain)
  if(APPLE)
    target_link_libraries(cpp_tests PRIVATE "-framework Accelerate")
  endif()
  add_test(NAME cpp_smoke COMMAND cpp_tests)
endif()
