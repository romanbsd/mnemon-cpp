# C++ mnemon — convenience targets (CMake is the source of truth).
.PHONY: build test unit test-helper clean help

CMAKE_BUILD_TYPE ?= Release

help: ## Show targets
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-10s\033[0m %s\n", $$1, $$2}'

build: ## Configure and compile (build/mnemon)
	cmake -S . -B build -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE)
	cmake --build build -j

unit: build ## ctest from build/
	cd build && ctest --output-on-failure

test: build ## Full E2E harness (requires jq)
	bash scripts/e2e_test.sh

test-helper: ## Run upstream-sync helper tests (no build, no network)
	bash tests/upstream_sync_helper_test.sh
	@if [ -f tests/upstream_release_tags_test.sh ]; then bash tests/upstream_release_tags_test.sh; fi

clean: ## Remove build tree and local E2E data
	rm -rf build .testdata
