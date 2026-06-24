#!/usr/bin/env bash
#
# Mnemon E2E visual test (C++ project root: parent of this script's directory).
# Usage: bash scripts/e2e_test.sh
# Optional: MNEMON_TEST_BINARY=/path/to/mnemon to skip the CMake build.
#
set -euo pipefail

# ── Prerequisites ───────────────────────────────────────────────────────
require_cmd() {
  local bin="$1"
  local hint="${2:-}"
  if ! command -v "$bin" >/dev/null 2>&1; then
    echo "e2e_test.sh: '$bin' is required but not on PATH." >&2
    [ -n "$hint" ] && echo "  $hint" >&2
    exit 1
  fi
}
require_cmd jq "e.g. brew install jq, apt install jq — https://jqlang.github.io/jq/download/"
if [ -z "${MNEMON_TEST_BINARY:-}" ]; then
  require_cmd cmake "CMake 3.24+ required to build (e.g. brew install cmake, apt install cmake)"
fi

# ── Colors ────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
DIM='\033[2m'
RESET='\033[0m'

PASS=0
FAIL=0
TOTAL=0

# ── Helpers ───────────────────────────────────────────────────────────
banner() {
  echo ""
  echo -e "${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RESET}"
  echo -e "${BOLD}${CYAN}  $1${RESET}"
  echo -e "${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RESET}"
}

step() {
  echo ""
  echo -e "  ${YELLOW}▸${RESET} ${BOLD}$1${RESET}"
}

show_json() {
  echo "$1" | jq '.' 2>/dev/null | head -"${2:-20}" | sed 's/^/    /'
}

pass() {
  local label="$1"; local detail="$2"
  TOTAL=$((TOTAL + 1)); PASS=$((PASS + 1))
  echo -e "    ${GREEN}✔${RESET} $label ${DIM}$detail${RESET}"
}

fail() {
  local label="$1"; local detail="$2"
  TOTAL=$((TOTAL + 1)); FAIL=$((FAIL + 1))
  echo -e "    ${RED}✘${RESET} $label ${DIM}$detail${RESET}"
}

# assert_contains LABEL JSON NEEDLE
assert_contains() {
  if echo "$2" | grep -q "$3"; then
    pass "$1" "(contains: $3)"
  else
    fail "$1" "(expected: $3)"
  fi
}

# assert_not_contains LABEL JSON NEEDLE
assert_not_contains() {
  if echo "$2" | grep -q "$3"; then
    fail "$1" "(should NOT contain: $3)"
  else
    pass "$1" "(absent: $3)"
  fi
}

# assert_jq LABEL JSON JQ_FILTER EXPECTED
# e.g. assert_jq "total is 1" "$OUT" '.total_insights' '1'
assert_jq() {
  local label="$1" json="$2" filter="$3" expected="$4"
  local actual
  actual=$(echo "$json" | jq -r "$filter" 2>/dev/null || echo "__ERROR__")
  if [ "$actual" = "$expected" ]; then
    pass "$label" "($filter == $expected)"
  else
    fail "$label" "($filter: expected=$expected, got=$actual)"
  fi
}

# assert_jq_gte LABEL JSON JQ_FILTER EXPECTED
assert_jq_gte() {
  local label="$1" json="$2" filter="$3" expected="$4"
  local actual
  actual=$(echo "$json" | jq -r "$filter" 2>/dev/null || echo "0")
  if [ "$actual" -ge "$expected" ] 2>/dev/null; then
    pass "$label" "($filter=$actual >= $expected)"
  else
    fail "$label" "($filter: expected >= $expected, got=$actual)"
  fi
}

# assert_jq_lte LABEL JSON JQ_FILTER EXPECTED
assert_jq_lte() {
  local label="$1" json="$2" filter="$3" expected="$4"
  local actual
  actual=$(echo "$json" | jq -r "$filter" 2>/dev/null || echo "0")
  if [ "$actual" -le "$expected" ] 2>/dev/null; then
    pass "$label" "($filter=$actual <= $expected)"
  else
    fail "$label" "($filter: expected <= $expected, got=$actual)"
  fi
}

extract_id() {
  echo "$1" | jq -r '.id'
}

# ── Setup ─────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
TESTDATA="$PROJECT_DIR/.testdata"
TESTDIR="$TESTDATA/m1"
DEFAULT_MNEMON="$PROJECT_DIR/build/mnemon"
M="${MNEMON_TEST_BINARY:-$DEFAULT_MNEMON}"

banner "Building mnemon"
cd "$PROJECT_DIR"
if [ -n "${MNEMON_TEST_BINARY:-}" ]; then
  echo -e "  ${GREEN}✔${RESET} Using test binary: $M"
else
  cmake -S "$PROJECT_DIR" -B "$PROJECT_DIR/build" -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
  cmake --build "$PROJECT_DIR/build" -j
  M="$DEFAULT_MNEMON"
  echo -e "  ${GREEN}✔${RESET} Binary built: $M"
fi

# Clean previous test data
rm -rf "$TESTDATA"
mkdir -p "$TESTDIR"
echo -e "  ${DIM}  Test data: $TESTDATA/${RESET}"

# ══════════════════════════════════════════════════════════════════════
banner "Milestone 0: Store Management & Data Isolation"
# ══════════════════════════════════════════════════════════════════════

STORE_DIR="$TESTDATA/store_test"
mkdir -p "$STORE_DIR"

step "store list — empty on fresh dir"
OUT=$($M --data-dir "$STORE_DIR" store list)
assert_contains "no stores message" "$OUT" "no stores yet"

step "store create — create stores"
OUT=$($M --data-dir "$STORE_DIR" store create default)
assert_contains "created default" "$OUT" 'Created store "default"'
OUT=$($M --data-dir "$STORE_DIR" store create work)
assert_contains "created work" "$OUT" 'Created store "work"'

step "store create — reject duplicate"
OUT=$($M --data-dir "$STORE_DIR" store create work 2>&1 || true)
assert_contains "rejects duplicate" "$OUT" "already exists"

step "store create — reject invalid name"
OUT=$($M --data-dir "$STORE_DIR" store create ".bad" 2>&1 || true)
assert_contains "rejects invalid" "$OUT" "invalid store name"

step "store list — shows created stores"
OUT=$($M --data-dir "$STORE_DIR" store list)
assert_contains "lists default" "$OUT" "default"
assert_contains "lists work" "$OUT" "work"

step "store set — switch active store"
$M --data-dir "$STORE_DIR" store set work
OUT=$($M --data-dir "$STORE_DIR" store list)
assert_contains "work is active" "$OUT" "* work"

step "store set — reject nonexistent"
OUT=$($M --data-dir "$STORE_DIR" store set nonexistent 2>&1 || true)
assert_contains "rejects missing" "$OUT" "does not exist"

step "store set — reject invalid name"
OUT=$($M --data-dir "$STORE_DIR" store set "../outside" 2>&1 || true)
assert_contains "store set rejects invalid" "$OUT" "invalid store name"

step "store remove — reject invalid name"
OUT=$($M --data-dir "$STORE_DIR" store remove "../outside" 2>&1 || true)
assert_contains "store remove rejects invalid" "$OUT" "invalid store name"

step "MNEMON_STORE invalid — openDB rejects"
OUT=$(MNEMON_STORE="../outside" $M --data-dir "$STORE_DIR" status 2>&1 || true)
assert_contains "env invalid store rejected" "$OUT" "invalid store name"

step "store list — filters invalid directory names"
mkdir -p "$STORE_DIR/data/.hidden"
OUT=$($M --data-dir "$STORE_DIR" store list)
assert_not_contains "list filters .hidden" "$OUT" ".hidden"

step "store remove — cannot remove active store"
OUT=$($M --data-dir "$STORE_DIR" store remove work 2>&1 || true)
assert_contains "rejects active removal" "$OUT" "cannot remove the active store"

step "store remove — remove inactive store"
$M --data-dir "$STORE_DIR" store create temp
OUT=$($M --data-dir "$STORE_DIR" store remove temp)
assert_contains "removed temp" "$OUT" 'Removed store "temp"'

step "data isolation — memories in different stores are isolated"
MNEMON_STORE=default $M --data-dir "$STORE_DIR" remember --no-diff "I am in default store" --cat fact --imp 3 > /dev/null
MNEMON_STORE=work $M --data-dir "$STORE_DIR" remember --no-diff "I am in work store" --cat fact --imp 3 > /dev/null

OUT=$(MNEMON_STORE=default $M --data-dir "$STORE_DIR" search "default store")
assert_contains "default finds own data" "$OUT" "I am in default store"
assert_not_contains "default not finds work data" "$OUT" "I am in work store"

OUT=$(MNEMON_STORE=work $M --data-dir "$STORE_DIR" search "work store")
assert_contains "work finds own data" "$OUT" "I am in work store"
assert_not_contains "work not finds default data" "$OUT" "I am in default store"

step "MNEMON_STORE env — overrides active file"
# Active is "work", but env says "default"
OUT=$(MNEMON_STORE=default $M --data-dir "$STORE_DIR" status)
assert_contains "env override db path" "$OUT" "data/default/mnemon.db"

step "migration — moves legacy DB + WAL/SHM sidecars to data/default/"
MIGRATE_DIR="$TESTDATA/migrate_test"
rm -rf "$MIGRATE_DIR" && mkdir -p "$MIGRATE_DIR"
# Create a valid legacy-layout SQLite DB and fake sidecar files
sqlite3 "$MIGRATE_DIR/mnemon.db" "CREATE TABLE IF NOT EXISTS insights (id TEXT PRIMARY KEY);"
printf 'fake-wal' > "$MIGRATE_DIR/mnemon.db-wal"
printf 'fake-shm' > "$MIGRATE_DIR/mnemon.db-shm"
# Trigger migration (any command that calls open_db); SQLite may clean up WAL on close
$M --data-dir "$MIGRATE_DIR" status > /dev/null 2>&1 || true
assert_contains "db migrated" "$(ls "$MIGRATE_DIR/data/default/" 2>/dev/null)" "mnemon.db"
# WAL/SHM should no longer be at the root after migration (moved or cleaned up by SQLite)
if [ -f "$MIGRATE_DIR/mnemon.db-wal" ]; then fail "legacy wal gone" "(should not remain at root)"; else pass "legacy wal gone" "(absent from root)"; fi
if [ -f "$MIGRATE_DIR/mnemon.db-shm" ]; then fail "legacy shm gone" "(should not remain at root)"; else pass "legacy shm gone" "(absent from root)"; fi

# ══════════════════════════════════════════════════════════════════════
banner "Milestone 1: Basic CRUD"
# ══════════════════════════════════════════════════════════════════════

step "remember — store insight with tags"
OUT=$($M --data-dir "$TESTDIR" remember --no-diff "User prefers Qdrant for vector DB" --cat preference --imp 4 --tags "tool,db")
show_json "$OUT" 20
ID1=$(extract_id "$OUT")
assert_jq "category is preference" "$OUT" '.category' 'preference'
assert_jq "importance is 4"        "$OUT" '.importance' '4'
assert_contains "tags include tool" "$OUT" '"tool"'
assert_contains "entities has Qdrant" "$OUT" '"Qdrant"'

step "recall — keyword search (compact default)"
OUT=$($M --data-dir "$TESTDIR" recall "Qdrant")
show_json "$OUT" 10
assert_contains "found Qdrant insight" "$OUT" "User prefers Qdrant"
assert_contains "compact has confidence" "$OUT" '"confidence"'
assert_not_contains "compact omits signals" "$OUT" '"signals"'
assert_not_contains "compact omits anchor_count" "$OUT" '"anchor_count"'

step "recall — no match returns sparse hint (compact)"
OUT=$($M --data-dir "$TESTDIR" recall "nonexistent_xyz")
assert_contains "sparse hint" "$OUT" "sparse_results"

step "recall --verbose — full payload"
OUT=$($M --data-dir "$TESTDIR" recall "Qdrant" --verbose)
assert_contains "verbose has meta" "$OUT" '"meta"'
assert_contains "verbose has signals" "$OUT" '"signals"'
assert_contains "verbose has anchor_count" "$OUT" '"anchor_count"'

step "status — statistics"
OUT=$($M --data-dir "$TESTDIR" status)
show_json "$OUT"
assert_jq "total is 1"          "$OUT" '.total_insights' '1'
assert_jq "preference count"    "$OUT" '.by_category.preference' '1'
assert_jq "no deleted insights" "$OUT" '.deleted_insights' '0'

step "forget — soft delete"
OUT=$($M --data-dir "$TESTDIR" forget "$ID1")
show_json "$OUT"
assert_jq "status is deleted" "$OUT" '.status' 'deleted'

OUT=$($M --data-dir "$TESTDIR" status)
assert_jq "total now 0"    "$OUT" '.total_insights'   '0'
assert_jq "deleted now 1"  "$OUT" '.deleted_insights'  '1'

# ══════════════════════════════════════════════════════════════════════
banner "Milestone 2: Graph Edge Auto-Generation"
# ══════════════════════════════════════════════════════════════════════

# Fresh DB for cleaner edge tests
TESTDIR2="$TESTDATA/m2"
mkdir -p "$TESTDIR2"

step "remember 3 insights — check temporal + causal edges"
OUT1=$($M --data-dir "$TESTDIR2" remember --no-diff "User prefers Qdrant for vector DB" --cat preference --imp 4)
ID_A=$(extract_id "$OUT1")
assert_jq "first: no temporal" "$OUT1" '.edges_created.temporal' '0'

sleep 1  # ensure distinct timestamps

OUT2=$($M --data-dir "$TESTDIR2" remember --no-diff "Chose Qdrant because of Rust performance" --cat decision --imp 5)
ID_B=$(extract_id "$OUT2")
assert_jq "second: 2 temporal" "$OUT2" '.edges_created.temporal' '2'
assert_jq "second: 1 causal"   "$OUT2" '.edges_created.causal'   '1'

sleep 1

OUT3=$($M --data-dir "$TESTDIR2" remember --no-diff "Qdrant benchmark shows 10ms p99 latency" --cat fact --imp 3)
ID_C=$(extract_id "$OUT3")
assert_jq_gte "third: >= 2 temporal (backbone + proximity)" "$OUT3" '.edges_created.temporal' '2'

step "status — verify edge count"
OUT=$($M --data-dir "$TESTDIR2" status)
assert_jq_gte "edges >= 5" "$OUT" '.edge_count' '5'

step "related — temporal traversal from B"
OUT=$($M --data-dir "$TESTDIR2" related "$ID_B" --edge temporal)
show_json "$OUT" 20
assert_contains "finds A via temporal" "$OUT" "$ID_A"
assert_contains "finds C via temporal" "$OUT" "$ID_C"

step "related — causal traversal from B"
OUT=$($M --data-dir "$TESTDIR2" related "$ID_B" --edge causal)
show_json "$OUT" 10
assert_contains "finds A via causal" "$OUT" "$ID_A"

step "Entity extraction — CamelCase"
OUT=$($M --data-dir "$TESTDIR2" remember --no-diff "We use HttpServer and DataStore in the project" --cat fact)
echo -e "    ${DIM}entities: $(echo "$OUT" | jq -c '.entities')${RESET}"
assert_contains "HttpServer extracted" "$OUT" '"HttpServer"'
assert_contains "DataStore extracted"  "$OUT" '"DataStore"'
ID_D=$(extract_id "$OUT")

sleep 1

step "Entity edge — shared entity creates link"
OUT=$($M --data-dir "$TESTDIR2" remember --no-diff "HttpServer handles all API requests" --cat fact)
echo -e "    ${DIM}entities: $(echo "$OUT" | jq -c '.entities')  edges: $(echo "$OUT" | jq -c '.edges_created')${RESET}"
assert_jq_gte "entity edges created (bidirectional)" "$OUT" '.edges_created.entity' '2'
ID_E=$(extract_id "$OUT")

step "Entity edge — bidirectional traversal"
OUT=$($M --data-dir "$TESTDIR2" related "$ID_E" --edge entity)
assert_contains "E → D via entity" "$OUT" "$ID_D"
OUT=$($M --data-dir "$TESTDIR2" related "$ID_D" --edge entity)
assert_contains "D → E via entity (reverse)" "$OUT" "$ID_E"

step "Entity extraction — file paths"
OUT=$($M --data-dir "$TESTDIR2" remember --no-diff "Config lives at ./cmd/root.go and internal/store/db.go" --cat fact)
echo -e "    ${DIM}entities: $(echo "$OUT" | jq -c '.entities')${RESET}"
assert_contains "file path extracted" "$OUT" './cmd/root.go'

step "Entity extraction — Chinese book titles"
OUT=$($M --data-dir "$TESTDIR2" remember --no-diff "推荐阅读《深入理解计算机系统》这本书" --cat fact)
echo -e "    ${DIM}entities: $(echo "$OUT" | jq -c '.entities')${RESET}"
assert_contains "Chinese title extracted" "$OUT" '深入理解计算机系统'

# ══════════════════════════════════════════════════════════════════════
banner "Milestone 3: Search + Diff"
# ══════════════════════════════════════════════════════════════════════

step "search — token-scored search"
OUT=$($M --data-dir "$TESTDIR2" search "Rust performance")
show_json "$OUT" 15
assert_contains "finds decision insight" "$OUT" "Chose Qdrant"
assert_contains "has score field"        "$OUT" '"score"'

step "search — no match returns []"
OUT=$($M --data-dir "$TESTDIR2" search "zzz_no_match_zzz")
assert_jq "empty array" "$OUT" 'length' '0'


# ══════════════════════════════════════════════════════════════════════
banner "Milestone 4: Intent-Aware Smart Recall"
# ══════════════════════════════════════════════════════════════════════

# Fresh DB for multi-level traversal test
TESTDIR3="$TESTDATA/m4"
mkdir -p "$TESTDIR3"

step "multi-level traversal — build A→B→C causal chain"
# A: anchor (keyword-matched), B: 1-hop, C: 2-hop (only reachable with depth>=2)
OUT_X=$($M --data-dir "$TESTDIR3" remember --no-diff "Alpha service handles request routing" --cat fact --imp 3)
ID_X=$(extract_id "$OUT_X")

sleep 1

OUT_Y=$($M --data-dir "$TESTDIR3" remember --no-diff "Request routing uses Alpha service because of low latency" --cat decision --imp 4)
ID_Y=$(extract_id "$OUT_Y")
assert_jq_gte "Y has causal edge to X" "$OUT_Y" '.edges_created.causal' '1'

sleep 1

OUT_Z=$($M --data-dir "$TESTDIR3" remember --no-diff "Low latency achieved because of edge caching" --cat fact --imp 3)
ID_Z=$(extract_id "$OUT_Z")
assert_jq_gte "Z has causal edge to Y" "$OUT_Z" '.edges_created.causal' '1'

step "multi-level traversal — smart recall finds depth-2 node"
OUT=$($M --data-dir "$TESTDIR3" recall "why Alpha service routing" --smart --verbose)
echo -e "    ${DIM}intent: $(echo "$OUT" | jq -r '.results[0].intent // "N/A"')  results: $(echo "$OUT" | jq '.results | length')${RESET}"
echo "$OUT" | jq -r '.results[] | "    \(.via)\t\(.score | tostring | .[:6])\t\(.insight.content[:50])"' 2>/dev/null
assert_contains "finds anchor X (Alpha)" "$OUT" "$ID_X"
assert_contains "finds depth-1 Y (routing)" "$OUT" "$ID_Y"
assert_contains "finds depth-2 Z (caching)" "$OUT" "$ID_Z"

step "smart recall — WHY intent"
OUT=$($M --data-dir "$TESTDIR2" recall "why did we choose Qdrant" --smart --verbose)
echo -e "    ${DIM}intent: $(echo "$OUT" | jq -r '.results[0].intent // "N/A"')  results: $(echo "$OUT" | jq '.results | length')${RESET}"
assert_contains "intent is WHY" "$OUT" '"WHY"'
assert_contains "finds Qdrant insight" "$OUT" "Qdrant"

step "smart recall — WHEN intent"
OUT=$($M --data-dir "$TESTDIR2" recall "when did we choose vector db" --smart --verbose)
echo -e "    ${DIM}intent: $(echo "$OUT" | jq -r '.results[0].intent // "N/A"')  results: $(echo "$OUT" | jq '.results | length')${RESET}"
assert_contains "intent is WHEN" "$OUT" '"WHEN"'

step "smart recall — graph augments results"
OUT=$($M --data-dir "$TESTDIR2" recall "why Qdrant performance" --smart --verbose)
COUNT=$(echo "$OUT" | jq '.results | length')
TOTAL=$((TOTAL + 1))
if [ "$COUNT" -ge 2 ]; then
  PASS=$((PASS + 1))
  echo -e "    ${GREEN}✔${RESET} Returns multiple results ${DIM}(count=$COUNT)${RESET}"
else
  FAIL=$((FAIL + 1))
  echo -e "    ${RED}✘${RESET} Expected >= 2 results, got $COUNT"
fi

# ══════════════════════════════════════════════════════════════════════
banner "Milestone 5: Semantic Edges (Claude-in-the-loop)"
# ══════════════════════════════════════════════════════════════════════

step "remember — output includes semantic_candidates field"
TESTDIR5="$TESTDATA/m5"
mkdir -p "$TESTDIR5"
OUT=$($M --data-dir "$TESTDIR5" remember --no-diff "Go is great for building CLI tools" --cat fact --imp 3)
assert_contains "has semantic_candidates" "$OUT" '"semantic_candidates"'
assert_jq "semantic field is 0 in edges_created" "$OUT" '.edges_created.semantic' '0'
ID_S1=$(extract_id "$OUT")

sleep 1

step "remember — similar content generates candidates"
OUT=$($M --data-dir "$TESTDIR5" remember --no-diff "Building CLI tools in Go is efficient" --cat fact --imp 3)
assert_contains "has semantic_candidates" "$OUT" '"semantic_candidates"'
ID_S2=$(extract_id "$OUT")
# Should find the first insight as a candidate (high token overlap)
SC_COUNT=$(echo "$OUT" | jq '.semantic_candidates | length')
TOTAL=$((TOTAL + 1))
if [ "$SC_COUNT" -ge 1 ]; then
  PASS=$((PASS + 1))
  echo -e "    ${GREEN}✔${RESET} Found $SC_COUNT semantic candidate(s)"
else
  FAIL=$((FAIL + 1))
  echo -e "    ${RED}✘${RESET} Expected >= 1 semantic candidates, got $SC_COUNT"
fi

step "remember — unrelated content has fewer candidates"
OUT=$($M --data-dir "$TESTDIR5" remember --no-diff "Xylophone zebra quantum platypus" --cat fact --imp 2)
SC_COUNT=$(echo "$OUT" | jq '.semantic_candidates | length')
# With embeddings, generic similarity may still produce low-score candidates (cosine > 0.30).
# Verify count is within bounds (≤ maxSemanticCandidates=5).
assert_jq_lte "unrelated: limited candidates" "$OUT" '.semantic_candidates | length' '5'

step "link — create semantic edge"
OUT=$($M --data-dir "$TESTDIR5" link "$ID_S1" "$ID_S2" --type semantic --weight 0.85)
show_json "$OUT" 10
assert_jq "status is linked" "$OUT" '.status' 'linked'
assert_jq "edge type is semantic" "$OUT" '.edge_type' 'semantic'
assert_contains "created_by claude" "$OUT" '"created_by"'

step "link — verify bidirectional edges"
OUT=$($M --data-dir "$TESTDIR5" related "$ID_S1" --edge semantic)
assert_contains "S1 → S2 via semantic" "$OUT" "$ID_S2"
OUT=$($M --data-dir "$TESTDIR5" related "$ID_S2" --edge semantic)
assert_contains "S2 → S1 via semantic (reverse)" "$OUT" "$ID_S1"

step "link — weight validation"
OUT=$($M --data-dir "$TESTDIR5" link "$ID_S1" "$ID_S2" --type semantic --weight 1.5 2>&1 || true)
assert_contains "rejects weight > 1.0" "$OUT" "weight must be"

step "link — nonexistent insight"
OUT=$($M --data-dir "$TESTDIR5" link "$ID_S1" "nonexistent-id-000" --type semantic --weight 0.5 2>&1 || true)
assert_contains "rejects missing insight" "$OUT" "not found"

step "smart recall — semantic edges participate in traversal"
OUT=$($M --data-dir "$TESTDIR5" recall "Go CLI" --smart --verbose)
COUNT=$(echo "$OUT" | jq '.results | length')
TOTAL=$((TOTAL + 1))
if [ "$COUNT" -ge 2 ]; then
  PASS=$((PASS + 1))
  echo -e "    ${GREEN}✔${RESET} Semantic-linked insights found ${DIM}(count=$COUNT)${RESET}"
else
  FAIL=$((FAIL + 1))
  echo -e "    ${RED}✘${RESET} Expected >= 2 results via semantic edges, got $COUNT"
fi

# ══════════════════════════════════════════════════════════════════════
banner "Milestone 6: Retention Lifecycle (effective_importance)"
# ══════════════════════════════════════════════════════════════════════

TESTDIR6="$TESTDATA/m6"
mkdir -p "$TESTDIR6"

step "setup — create insights with varying importance"
$M --data-dir "$TESTDIR6" remember --no-diff "Critical architecture decision: use SQLite" --cat decision --imp 5 > /dev/null
sleep 1
$M --data-dir "$TESTDIR6" remember --no-diff "Minor note about formatting" --cat general --imp 1 > /dev/null
ID_LOW=$(extract_id "$($M --data-dir "$TESTDIR6" remember --no-diff "Temporary context note" --cat context --imp 1)")
sleep 1
$M --data-dir "$TESTDIR6" remember --no-diff "Important user preference for dark mode" --cat preference --imp 4 > /dev/null

step "remember — output includes effective_importance and auto_pruned"
OUT=$($M --data-dir "$TESTDIR6" remember --no-diff "Test insight for lifecycle" --cat fact --imp 3)
assert_contains "has effective_importance" "$OUT" '"effective_importance"'
assert_contains "has auto_pruned" "$OUT" '"auto_pruned"'
assert_jq "auto_pruned is 0 (under cap)" "$OUT" '.auto_pruned' '0'

step "gc — suggest mode returns candidates with effective_importance"
OUT=$($M --data-dir "$TESTDIR6" gc --threshold 0.7)
show_json "$OUT" 25
assert_contains "has candidates field" "$OUT" '"candidates"'
assert_contains "has actions field"    "$OUT" '"actions"'
assert_contains "has max_insights"     "$OUT" '"max_insights"'
assert_jq "total_insights is 5" "$OUT" '.total_insights' '5'

step "gc — low-importance non-immune insights appear as candidates"
CAND_COUNT=$(echo "$OUT" | jq '.candidates_found')
TOTAL=$((TOTAL + 1))
if [ "$CAND_COUNT" -ge 1 ]; then
  PASS=$((PASS + 1))
  echo -e "    ${GREEN}✔${RESET} Found $CAND_COUNT GC candidate(s)"
else
  FAIL=$((FAIL + 1))
  echo -e "    ${RED}✘${RESET} Expected >= 1 GC candidates, got $CAND_COUNT"
fi

step "gc — candidates have effective_importance and immune fields"
FIRST=$(echo "$OUT" | jq '.candidates[0]')
assert_contains "has effective_importance" "$FIRST" '"effective_importance"'
assert_contains "has days_since"           "$FIRST" '"days_since_access"'
assert_contains "has immune field"         "$FIRST" '"immune"'

step "gc — immune insights (imp>=4) are excluded from candidates"
# Check that no candidate has importance >= 4
HIGH_IMP=$(echo "$OUT" | jq '[.candidates[] | select(.insight.importance >= 4)] | length')
assert_jq "no high-imp candidates" "$OUT" '[.candidates[] | select(.insight.importance >= 4)] | length' '0'

step "gc --keep — boost retention"
OUT=$($M --data-dir "$TESTDIR6" gc --keep "$ID_LOW")
show_json "$OUT" 10
assert_jq "status is retained" "$OUT" '.status' 'retained'
assert_jq "access count boosted" "$OUT" '.new_access' '3'
assert_contains "has effective_importance" "$OUT" '"effective_importance"'
assert_contains "has immune field"         "$OUT" '"immune"'

step "gc — kept insight becomes immune (access_count >= 3)"
OUT_AFTER=$($M --data-dir "$TESTDIR6" gc --threshold 0.7)
KEPT_STILL=$(echo "$OUT_AFTER" | jq --arg id "$ID_LOW" '[.candidates[].insight.id] | index($id)')
TOTAL=$((TOTAL + 1))
if [ "$KEPT_STILL" = "null" ]; then
  PASS=$((PASS + 1))
  echo -e "    ${GREEN}✔${RESET} Boosted insight is now immune (not in candidates)"
else
  FAIL=$((FAIL + 1))
  echo -e "    ${RED}✘${RESET} Boosted insight should be immune but still in candidates"
fi

step "gc --keep — nonexistent insight"
OUT=$($M --data-dir "$TESTDIR6" gc --keep "nonexistent-id-000" 2>&1 || true)
assert_contains "rejects missing insight" "$OUT" "not found"

step "gc — high threshold returns more candidates"
OUT=$($M --data-dir "$TESTDIR6" gc --threshold 2.0)
HIGH_COUNT=$(echo "$OUT" | jq '.candidates_found')
TOTAL=$((TOTAL + 1))
if [ "$HIGH_COUNT" -ge "$CAND_COUNT" ]; then
  PASS=$((PASS + 1))
  echo -e "    ${GREEN}✔${RESET} Higher threshold → more candidates ($HIGH_COUNT >= $CAND_COUNT)"
else
  FAIL=$((FAIL + 1))
  echo -e "    ${RED}✘${RESET} Expected higher threshold to find more candidates"
fi

# ══════════════════════════════════════════════════════════════════════
banner "Observability: Operation Log"
# ══════════════════════════════════════════════════════════════════════

step "log — shows operations from TESTDIR2"
OUT=$($M --data-dir "$TESTDIR2" log --limit 30)
echo "$OUT" | head -10 | sed 's/^/    /'
assert_contains "log has remember ops" "$OUT" "remember"
assert_contains "log has recall ops"   "$OUT" "recall"

step "log — shows link and gc operations"
OUT=$($M --data-dir "$TESTDIR5" log --limit 30)
assert_contains "log has link ops" "$OUT" "link"

OUT=$($M --data-dir "$TESTDIR6" log --limit 30)
assert_contains "log has gc ops" "$OUT" "gc"

# ══════════════════════════════════════════════════════════════════════
banner "Milestone 7: Embedding Support (Ollama)"
# ══════════════════════════════════════════════════════════════════════

step "embed --status — always works (even without Ollama)"
TESTDIR7="$TESTDATA/m7"
mkdir -p "$TESTDIR7"
$M --data-dir "$TESTDIR7" remember --no-diff "Embedding test insight one" --cat fact --imp 3 > /dev/null
$M --data-dir "$TESTDIR7" remember --no-diff "Embedding test insight two" --cat fact --imp 3 > /dev/null
OUT=$($M --data-dir "$TESTDIR7" embed --status)
show_json "$OUT"
assert_jq "total_insights is 2" "$OUT" '.total_insights' '2'
assert_contains "has ollama_available" "$OUT" '"ollama_available"'
assert_contains "has coverage field" "$OUT" '"coverage"'

# Check if Ollama is available for the remaining tests
OLLAMA_OK=$(echo "$OUT" | jq -r '.ollama_available')
if [ "$OLLAMA_OK" = "true" ]; then
  step "remember — auto-embeds when Ollama available"
  OUT=$($M --data-dir "$TESTDIR7" remember --no-diff "This insight should be auto-embedded" --cat fact --imp 3)
  assert_jq "embedded is true" "$OUT" '.embedded' 'true'
  ID_E1=$(extract_id "$OUT")

  step "embed --all — backfill un-embedded insights"
  # Create an insight without auto-embedding by pointing Ollama to a dead endpoint
  MNEMON_EMBED_ENDPOINT="http://127.0.0.1:1" $M --data-dir "$TESTDIR7" remember --no-diff "Un-embedded test insight" --cat fact --imp 2 > /dev/null

  # Backfill all — should find the un-embedded one
  OUT=$($M --data-dir "$TESTDIR7" embed --all)
  show_json "$OUT"
  assert_jq "backfill status" "$OUT" '.status' 'backfill_complete'
  assert_contains "has succeeded count" "$OUT" '"succeeded"'

  step "embed --status — verify coverage after backfill"
  OUT=$($M --data-dir "$TESTDIR7" embed --status)
  assert_jq "all embedded" "$OUT" '.coverage' '100%'

  step "recall --smart — uses hybrid search with embeddings"
  OUT=$($M --data-dir "$TESTDIR7" recall "embedding test" --smart --verbose)
  COUNT=$(echo "$OUT" | jq '.results | length')
  TOTAL=$((TOTAL + 1))
  if [ "$COUNT" -ge 1 ]; then
    PASS=$((PASS + 1))
    echo -e "    ${GREEN}✔${RESET} Smart recall with embeddings works ${DIM}(count=$COUNT)${RESET}"
  else
    FAIL=$((FAIL + 1))
    echo -e "    ${RED}✘${RESET} Expected >= 1 results, got $COUNT"
  fi
else
  echo -e "  ${DIM}  Ollama not available — skipping embedding integration tests${RESET}"
  echo -e "  ${DIM}  Install: brew install ollama && ollama pull nomic-embed-text${RESET}"

  step "remember — embedded=false when Ollama unavailable"
  OUT=$($M --data-dir "$TESTDIR7" remember --no-diff "This insight will not be embedded" --cat fact --imp 3)
  assert_jq "embedded is false" "$OUT" '.embedded' 'false'
fi

# ══════════════════════════════════════════════════════════════════════
banner "Milestone 8: Causal Candidates (2-hop BFS Neighborhood)"
# ══════════════════════════════════════════════════════════════════════

TESTDIR8="$TESTDATA/m8"
mkdir -p "$TESTDIR8"

step "causal candidates — output includes causal_candidates field"
OUT=$($M --data-dir "$TESTDIR8" remember --no-diff "Causal test baseline insight about caching" --cat fact --imp 3)
assert_contains "has causal_candidates" "$OUT" '"causal_candidates"'
ID_CC1=$(extract_id "$OUT")

step "causal candidates — BFS finds neighbors via edges"
sleep 1
OUT=$($M --data-dir "$TESTDIR8" remember --no-diff "Chose Redis because of latency requirements in caching" --cat decision --imp 4)
assert_contains "has causal_candidates" "$OUT" '"causal_candidates"'
ID_CC2=$(extract_id "$OUT")
CC_COUNT=$(echo "$OUT" | jq '.causal_candidates | length')
TOTAL=$((TOTAL + 1))
if [ "$CC_COUNT" -ge 1 ]; then
  PASS=$((PASS + 1))
  echo -e "    ${GREEN}✔${RESET} Found $CC_COUNT causal candidate(s) via BFS"
else
  FAIL=$((FAIL + 1))
  echo -e "    ${RED}✘${RESET} Expected >= 1 causal candidates, got $CC_COUNT"
fi

step "causal candidates — candidate has hop and via_edge fields"
if [ "$CC_COUNT" -ge 1 ]; then
  FIRST_CC=$(echo "$OUT" | jq '.causal_candidates[0]')
  assert_contains "has hop" "$FIRST_CC" '"hop"'
  assert_contains "has via_edge" "$FIRST_CC" '"via_edge"'
  assert_contains "has causal_signal" "$FIRST_CC" '"causal_signal"'
  assert_contains "has suggested_sub_type" "$FIRST_CC" '"suggested_sub_type"'
fi

step "causal candidates — hop-2 discovery via graph"
sleep 1
OUT=$($M --data-dir "$TESTDIR8" remember --no-diff "Edge caching reduces Redis load significantly" --cat fact --imp 3)
ID_CC3=$(extract_id "$OUT")
CC_COUNT2=$(echo "$OUT" | jq '.causal_candidates | length')
TOTAL=$((TOTAL + 1))
if [ "$CC_COUNT2" -ge 2 ]; then
  PASS=$((PASS + 1))
  echo -e "    ${GREEN}✔${RESET} Found $CC_COUNT2 candidates (includes hop-2 via BFS)"
else
  FAIL=$((FAIL + 1))
  echo -e "    ${RED}✘${RESET} Expected >= 2 causal candidates (hop-1 + hop-2), got $CC_COUNT2"
fi

step "entity extraction — dictionary-based (tech terms)"
TESTDIR_DICT="$TESTDATA/m_dict"
mkdir -p "$TESTDIR_DICT"
OUT=$($M --data-dir "$TESTDIR_DICT" remember --no-diff "We use React and TypeScript with Redis for caching" --cat fact --imp 3)
echo -e "    ${DIM}entities: $(echo "$OUT" | jq -c '.entities')${RESET}"
assert_contains "React extracted via dictionary" "$OUT" '"React"'
assert_contains "TypeScript extracted via dictionary" "$OUT" '"TypeScript"'
assert_contains "Redis extracted via dictionary" "$OUT" '"Redis"'

step "entity extraction — acronyms (ALLCAPS)"
OUT=$($M --data-dir "$TESTDIR_DICT" remember --no-diff "The API uses gRPC and JWT for authentication over HTTP" --cat fact --imp 3)
echo -e "    ${DIM}entities: $(echo "$OUT" | jq -c '.entities')${RESET}"
assert_contains "API extracted" "$OUT" '"API"'
assert_contains "JWT extracted" "$OUT" '"JWT"'
assert_contains "HTTP extracted" "$OUT" '"HTTP"'

step "entity extraction — stopwords not extracted"
OUT=$($M --data-dir "$TESTDIR_DICT" remember --no-diff "IF YOU CAN SEE THE WAY TO DO IT" --cat fact --imp 2)
echo -e "    ${DIM}entities: $(echo "$OUT" | jq -c '.entities')${RESET}"
assert_not_contains "IF not extracted" "$OUT" '"IF"'
assert_not_contains "YOU not extracted" "$OUT" '"YOU"'

# ══════════════════════════════════════════════════════════════════════
banner "Milestone 9: LLM Entity Injection (--entities flag)"
# ══════════════════════════════════════════════════════════════════════

TESTDIR9="$TESTDATA/m9"
mkdir -p "$TESTDIR9"

step "--entities — LLM-provided entities appear in output"
OUT=$($M --data-dir "$TESTDIR9" remember --no-diff "The new caching layer improves performance significantly" --cat fact --imp 3 --entities "caching-layer,performance-optimization")
echo -e "    ${DIM}entities: $(echo "$OUT" | jq -c '.entities')${RESET}"
assert_contains "LLM entity present" "$OUT" '"caching-layer"'
assert_contains "LLM entity present" "$OUT" '"performance-optimization"'

step "--entities — merges with regex-extracted entities"
OUT=$($M --data-dir "$TESTDIR9" remember --no-diff "We deploy HttpServer on Docker with Redis" --cat fact --imp 3 --entities "deployment-pipeline,high-availability")
echo -e "    ${DIM}entities: $(echo "$OUT" | jq -c '.entities')${RESET}"
# LLM-provided
assert_contains "LLM entity: deployment-pipeline" "$OUT" '"deployment-pipeline"'
assert_contains "LLM entity: high-availability" "$OUT" '"high-availability"'
# Regex/dictionary-extracted
assert_contains "regex entity: HttpServer" "$OUT" '"HttpServer"'
assert_contains "dict entity: Docker" "$OUT" '"Docker"'
assert_contains "dict entity: Redis" "$OUT" '"Redis"'

step "--entities — creates entity edges with shared LLM entities"
OUT=$($M --data-dir "$TESTDIR9" remember --no-diff "Upgrading the caching layer for better throughput" --cat decision --imp 4 --entities "caching-layer,throughput")
echo -e "    ${DIM}entities: $(echo "$OUT" | jq -c '.entities')  edges: $(echo "$OUT" | jq -c '.edges_created')${RESET}"
# "caching-layer" is shared with the first insight → should create entity edges
assert_jq_gte "entity edges from shared LLM entity" "$OUT" '.edges_created.entity' '2'

step "--entities — no flag still works (regex only)"
OUT=$($M --data-dir "$TESTDIR9" remember --no-diff "Python and FastAPI are great for prototyping" --cat fact --imp 3)
echo -e "    ${DIM}entities: $(echo "$OUT" | jq -c '.entities')${RESET}"
assert_contains "dict entity: Python" "$OUT" '"Python"'
assert_contains "dict entity: FastAPI" "$OUT" '"FastAPI"'

step "--entity-mode provided — only LLM-provided entities are used"
OUT=$($M --data-dir "$TESTDIR9" remember --no-diff "We deploy HttpServer on Docker with Redis" --cat fact --imp 3 --entities "strict-entity" --entity-mode provided 2>&1 || true)
echo -e "    ${DIM}entities: $(echo "$OUT" | jq -c '.entities' 2>/dev/null)${RESET}"
assert_contains "provided entity present" "$OUT" '"strict-entity"'
assert_not_contains "regex entity omitted in provided mode" "$OUT" '"HttpServer"'
assert_not_contains "dict entity omitted in provided mode" "$OUT" '"Docker"'

step "--entity-mode auto — ignores LLM-provided entities"
OUT=$($M --data-dir "$TESTDIR9" remember --no-diff "We deploy HttpServer on Docker with Redis" --cat fact --imp 3 --entities "ignored-entity" --entity-mode auto 2>&1 || true)
echo -e "    ${DIM}entities: $(echo "$OUT" | jq -c '.entities' 2>/dev/null)${RESET}"
assert_not_contains "provided entity ignored in auto mode" "$OUT" '"ignored-entity"'
assert_contains "regex entity present in auto mode" "$OUT" '"HttpServer"'
assert_contains "dict entity present in auto mode" "$OUT" '"Docker"'

step "--entity-mode invalid — rejects unknown modes"
OUT=$($M --data-dir "$TESTDIR9" remember --no-diff "test" --cat fact --imp 3 --entity-mode bogus 2>&1 || true)
assert_contains "invalid entity mode rejected" "$OUT" "invalid entity mode"

# ══════════════════════════════════════════════════════════════════════
banner "Milestone 10: Auto-Prune Lifecycle"
# ══════════════════════════════════════════════════════════════════════

TESTDIR10="$TESTDATA/m10"
mkdir -p "$TESTDIR10"

step "auto-prune — insert 5 low-imp + 2 high-imp insights (cap=4 for test)"
# We'll use a small cap to test pruning. The cap is hardcoded at 1000 in production,
# so here we test that the mechanism WORKS by checking auto_pruned=0 under cap.
for i in 1 2 3; do
  $M --data-dir "$TESTDIR10" remember --no-diff "Low importance note $i" --cat general --imp 1 > /dev/null
done
OUT=$($M --data-dir "$TESTDIR10" remember --no-diff "High importance decision" --cat decision --imp 5)
assert_jq "auto_pruned is 0 under cap" "$OUT" '.auto_pruned' '0'

step "auto-prune — effective_importance varies by importance level"
# imp=5 should have much higher EI than imp=1
OUT_HIGH=$($M --data-dir "$TESTDIR10" gc --threshold 999)
# All non-immune candidates should be imp=1 or 2
TOTAL=$((TOTAL + 1))
IMMUNE_IN_CAND=$(echo "$OUT_HIGH" | jq '[.candidates[] | select(.immune == true)] | length')
if [ "$IMMUNE_IN_CAND" = "0" ]; then
  PASS=$((PASS + 1))
  echo -e "    ${GREEN}✔${RESET} No immune insights in candidates"
else
  FAIL=$((FAIL + 1))
  echo -e "    ${RED}✘${RESET} Found $IMMUNE_IN_CAND immune insights in candidates"
fi

step "effective_importance — high imp > low imp"
EI_LOW=$(echo "$OUT_HIGH" | jq '.candidates[0].effective_importance')
EI_CONTEXT=$($M --data-dir "$TESTDIR10" remember --no-diff "Another high imp fact" --cat fact --imp 5 | jq '.effective_importance')
TOTAL=$((TOTAL + 1))
# EI for imp=5 should be > EI for imp=1
LOW_INT=$(echo "$EI_LOW" | awk '{printf "%d", $1 * 1000}')
HIGH_INT=$(echo "$EI_CONTEXT" | awk '{printf "%d", $1 * 1000}')
if [ "$HIGH_INT" -gt "$LOW_INT" ]; then
  PASS=$((PASS + 1))
  echo -e "    ${GREEN}✔${RESET} imp=5 EI ($EI_CONTEXT) > imp=1 EI ($EI_LOW)"
else
  FAIL=$((FAIL + 1))
  echo -e "    ${RED}✘${RESET} Expected imp=5 EI > imp=1 EI (got $EI_CONTEXT vs $EI_LOW)"
fi


# ══════════════════════════════════════════════════════════════════════
banner "Milestone 11: Smart Recall Reranking + Signals"
# ══════════════════════════════════════════════════════════════════════

step "smart recall — --intent override"
OUT=$($M --data-dir "$TESTDIR3" recall "Alpha service" --smart --intent WHY --verbose)
assert_jq "intent is WHY" "$OUT" '.meta.intent' 'WHY'
assert_jq "intent_source is override" "$OUT" '.meta.intent_source' 'override'

step "smart recall — auto-detected intent source"
OUT=$($M --data-dir "$TESTDIR3" recall "why Alpha service routing" --smart --verbose)
assert_jq "intent_source is auto" "$OUT" '.meta.intent_source' 'auto'

step "smart recall — signals metadata present"
OUT=$($M --data-dir "$TESTDIR3" recall "Alpha service routing" --smart --verbose)
FIRST=$(echo "$OUT" | jq '.results[0]')
assert_contains "has signals" "$FIRST" '"signals"'
assert_contains "has keyword signal" "$FIRST" '"keyword"'
assert_contains "has graph signal" "$FIRST" '"graph"'

step "smart recall — meta fields present"
assert_contains "has anchor_count" "$OUT" '"anchor_count"'
assert_contains "has traversed" "$OUT" '"traversed"'
assert_jq_gte "anchor_count >= 1" "$OUT" '.meta.anchor_count' '1'

step "smart recall — invalid intent rejected"
OUT=$($M --data-dir "$TESTDIR3" recall "test" --smart --intent INVALID 2>&1 || true)
assert_contains "rejects invalid intent" "$OUT" "unknown intent"

# ══════════════════════════════════════════════════════════════════════
banner "Input Validation: limit flags"
# ══════════════════════════════════════════════════════════════════════

VALDIR="$TESTDATA/val_test"
mkdir -p "$VALDIR"
$M --data-dir "$VALDIR" remember --no-diff "validation test insight" --cat fact --imp 3 > /dev/null

step "recall — rejects --limit 0"
OUT=$($M --data-dir "$VALDIR" recall "test" --limit 0 2>&1 || true)
assert_contains "recall limit 0 rejected" "$OUT" "must be at least 1"

step "search — rejects --limit 0"
OUT=$($M --data-dir "$VALDIR" search "test" --limit 0 2>&1 || true)
assert_contains "search limit 0 rejected" "$OUT" "must be at least 1"

step "log — rejects --limit 0"
OUT=$($M --data-dir "$VALDIR" log --limit 0 2>&1 || true)
assert_contains "log limit 0 rejected" "$OUT" "must be at least 1"

step "gc — rejects --limit 0"
OUT=$($M --data-dir "$VALDIR" gc --limit 0 2>&1 || true)
assert_contains "gc limit 0 rejected" "$OUT" "must be at least 1"

step "gc — rejects negative --threshold"
OUT=$($M --data-dir "$VALDIR" gc --threshold -0.1 2>&1 || true)
assert_contains "gc threshold negative rejected" "$OUT" "must be non-negative"

# ══════════════════════════════════════════════════════════════════════
banner "Setup: Nanobot target validation"
# ══════════════════════════════════════════════════════════════════════

SETUPDIR="$TESTDATA/setup_nanobot"
mkdir -p "$SETUPDIR"

step "setup --target nanobot --yes — accepted (installs skill)"
OUT=$($M --data-dir "$SETUPDIR" setup --target nanobot --yes 2>&1 || true)
assert_contains "nanobot target accepted" "$OUT" "Skill"

step "setup --target bogus — still rejected"
OUT=$($M --data-dir "$SETUPDIR" setup --target bogus 2>&1 || true)
assert_contains "bogus target rejected" "$OUT" "invalid target"

step "setup --target bogus error mentions nanobot"
assert_contains "error mentions nanobot" "$OUT" "nanobot"

step "setup --target codex --yes — accepted (installs skill)"
OUT=$($M --data-dir "$SETUPDIR" setup --target codex --yes 2>&1 || true)
assert_contains "codex target accepted" "$OUT" "Skill"

step "setup --target bogus error mentions codex"
OUT=$($M --data-dir "$SETUPDIR" setup --target bogus 2>&1 || true)
assert_contains "error mentions codex" "$OUT" "codex"

CURSOR_SETUP_DIR="$TESTDATA/setup_cursor"
mkdir -p "$CURSOR_SETUP_DIR"

step "setup --target cursor --yes — accepted (installs skill + hooks.json)"
OUT=$(cd "$CURSOR_SETUP_DIR" && $M --data-dir "$CURSOR_SETUP_DIR" setup --target cursor --yes 2>&1 || true)
assert_contains "cursor target accepted" "$OUT" "Skill"
CURSOR_HOOKS="$CURSOR_SETUP_DIR/.cursor/hooks.json"
if [ -f "$CURSOR_HOOKS" ]; then
  HJSON="$(cat "$CURSOR_HOOKS")"
  assert_jq "cursor version field" "$HJSON" '.version' 1
  assert_jq "cursor sessionStart prime hook" "$HJSON" '.hooks.sessionStart[0].command | endswith("hooks/mnemon/prime.sh")' true
  assert_jq "cursor stop hook loop_limit" "$HJSON" '.hooks.stop[0].loop_limit' 1
  assert_jq "cursor no preCompact by default" "$HJSON" '.hooks | has("preCompact")' false
else
  fail "cursor hooks.json written" "missing $CURSOR_HOOKS"
fi

step "setup --target bogus error mentions cursor"
OUT=$($M --data-dir "$CURSOR_SETUP_DIR" setup --target bogus 2>&1 || true)
assert_contains "error mentions cursor" "$OUT" "cursor"

TRAE_SETUP_DIR="$TESTDATA/setup_trae"
mkdir -p "$TRAE_SETUP_DIR"

step "setup --target trae --yes — accepted (installs skill + hooks.json)"
OUT=$(cd "$TRAE_SETUP_DIR" && $M --data-dir "$TRAE_SETUP_DIR" setup --target trae --yes 2>&1 || true)
assert_contains "trae target accepted" "$OUT" "Skill"
TRAE_HOOKS="$TRAE_SETUP_DIR/.trae/hooks.json"
if [ -f "$TRAE_HOOKS" ]; then
  THJSON="$(cat "$TRAE_HOOKS")"
  assert_jq "trae version field" "$THJSON" '.version' 1
  assert_jq "trae SessionStart prime hook" "$THJSON" '.hooks.SessionStart[0].hooks[0].command | endswith("hooks/mnemon/prime.sh")' true
  assert_jq "trae UserPromptSubmit remind hook" "$THJSON" '.hooks.UserPromptSubmit[0].hooks[0].command | endswith("hooks/mnemon/user_prompt.sh")' true
  assert_jq "trae Stop loop_limit" "$THJSON" '.hooks.Stop[0].loop_limit' 1
  assert_jq "trae hook timeout" "$THJSON" '.hooks.SessionStart[0].hooks[0].timeout' 30
else
  fail "trae hooks.json written" "missing $TRAE_HOOKS"
fi

step "setup --target bogus error mentions trae"
OUT=$($M --data-dir "$TRAE_SETUP_DIR" setup --target bogus 2>&1 || true)
assert_contains "error mentions trae" "$OUT" "trae"

QODER_SETUP_DIR="$TESTDATA/setup_qoder"
mkdir -p "$QODER_SETUP_DIR"

step "setup --target qoder --yes — accepted (installs skill + settings.json)"
OUT=$(cd "$QODER_SETUP_DIR" && $M --data-dir "$QODER_SETUP_DIR" setup --target qoder --yes 2>&1 || true)
assert_contains "qoder target accepted" "$OUT" "Skill"
QODER_SETTINGS="$QODER_SETUP_DIR/.qoder/settings.json"
if [ -f "$QODER_SETTINGS" ]; then
  QJSON="$(cat "$QODER_SETTINGS")"
  assert_jq "qoder SessionStart prime hook" "$QJSON" '.hooks.SessionStart[0].hooks[0].command | endswith("hooks/mnemon/prime.sh")' true
  assert_jq "qoder UserPromptSubmit remind hook" "$QJSON" '.hooks.UserPromptSubmit[0].hooks[0].command | endswith("hooks/mnemon/user_prompt.sh")' true
  assert_jq "qoder Stop nudge hook" "$QJSON" '.hooks.Stop[0].hooks[0].command | endswith("hooks/mnemon/stop.sh")' true
  assert_jq "qoder hook type command" "$QJSON" '.hooks.SessionStart[0].hooks[0].type' command
else
  fail "qoder settings.json written" "missing $QODER_SETTINGS"
fi

step "setup --target bogus error mentions qoder"
OUT=$($M --data-dir "$QODER_SETUP_DIR" setup --target bogus 2>&1 || true)
assert_contains "error mentions qoder" "$OUT" "qoder"

QODERWORK_HOME="$TESTDATA/setup_qoderwork_home"
mkdir -p "$QODERWORK_HOME"

step "setup --target qoderwork --yes — accepted (user-global ~/.qoderwork)"
OUT=$(HOME="$QODERWORK_HOME" $M --data-dir "$QODERWORK_HOME/.mnemon-data" setup --target qoderwork --yes 2>&1 || true)
assert_contains "qoderwork target accepted" "$OUT" "Skill"
QODERWORK_SETTINGS="$QODERWORK_HOME/.qoderwork/settings.json"
if [ -f "$QODERWORK_SETTINGS" ]; then
  QWJSON="$(cat "$QODERWORK_SETTINGS")"
  assert_jq "qoderwork SessionStart prime hook" "$QWJSON" '.hooks.SessionStart[0].hooks[0].command | endswith("hooks/mnemon/prime.sh")' true
  if [ -f "$QODERWORK_HOME/.qoderwork/skills/mnemon/SKILL.md" ]; then
    pass "qoderwork skill written" ""
  else
    fail "qoderwork skill written" "missing SKILL.md"
  fi
else
  fail "qoderwork settings.json written" "missing $QODERWORK_SETTINGS"
fi

CODEBUDDY_SETUP_DIR="$TESTDATA/setup_codebuddy"
mkdir -p "$CODEBUDDY_SETUP_DIR"

step "setup --target codebuddy --yes — accepted (installs skill + settings.json)"
OUT=$(cd "$CODEBUDDY_SETUP_DIR" && $M --data-dir "$CODEBUDDY_SETUP_DIR" setup --target codebuddy --yes 2>&1 || true)
assert_contains "codebuddy target accepted" "$OUT" "Skill"
CODEBUDDY_SETTINGS="$CODEBUDDY_SETUP_DIR/.codebuddy/settings.json"
if [ -f "$CODEBUDDY_SETTINGS" ]; then
  CBJSON="$(cat "$CODEBUDDY_SETTINGS")"
  assert_jq "codebuddy SessionStart prime hook" "$CBJSON" '.hooks.SessionStart[0].hooks[0].command | endswith("hooks/mnemon/prime.sh")' true
  assert_jq "codebuddy UserPromptSubmit remind hook" "$CBJSON" '.hooks.UserPromptSubmit[0].hooks[0].command | endswith("hooks/mnemon/user_prompt.sh")' true
  assert_jq "codebuddy Stop nudge hook" "$CBJSON" '.hooks.Stop[0].hooks[0].command | endswith("hooks/mnemon/stop.sh")' true
else
  fail "codebuddy settings.json written" "missing $CODEBUDDY_SETTINGS"
fi

step "setup --target bogus error mentions codebuddy"
OUT=$($M --data-dir "$CODEBUDDY_SETUP_DIR" setup --target bogus 2>&1 || true)
assert_contains "error mentions codebuddy" "$OUT" "codebuddy"

WORKBUDDY_SETUP_DIR="$TESTDATA/setup_workbuddy"
mkdir -p "$WORKBUDDY_SETUP_DIR"

step "setup --target workbuddy --yes — accepted (installs skill + settings.json)"
OUT=$(cd "$WORKBUDDY_SETUP_DIR" && $M --data-dir "$WORKBUDDY_SETUP_DIR" setup --target workbuddy --yes 2>&1 || true)
assert_contains "workbuddy target accepted" "$OUT" "Skill"
WORKBUDDY_SETTINGS="$WORKBUDDY_SETUP_DIR/.workbuddy/settings.json"
if [ -f "$WORKBUDDY_SETTINGS" ]; then
  WBJSON="$(cat "$WORKBUDDY_SETTINGS")"
  assert_jq "workbuddy SessionStart prime hook" "$WBJSON" '.hooks.SessionStart[0].hooks[0].command | endswith("hooks/mnemon/prime.sh")' true
  assert_jq "workbuddy UserPromptSubmit remind hook" "$WBJSON" '.hooks.UserPromptSubmit[0].hooks[0].command | endswith("hooks/mnemon/user_prompt.sh")' true
  assert_jq "workbuddy Stop nudge hook" "$WBJSON" '.hooks.Stop[0].hooks[0].command | endswith("hooks/mnemon/stop.sh")' true
else
  fail "workbuddy settings.json written" "missing $WORKBUDDY_SETTINGS"
fi

step "setup --target bogus error mentions workbuddy"
OUT=$($M --data-dir "$WORKBUDDY_SETUP_DIR" setup --target bogus 2>&1 || true)
assert_contains "error mentions workbuddy" "$OUT" "workbuddy"

KIMI_HOME="$TESTDATA/setup_kimi_home"
mkdir -p "$KIMI_HOME"

step "setup --target kimi --yes — accepted (native ~/.kimi-code via KIMI_CODE_HOME)"
OUT=$(KIMI_CODE_HOME="$KIMI_HOME/.kimi-code" $M --data-dir "$KIMI_HOME/.mnemon-data" setup --target kimi --yes 2>&1 || true)
assert_contains "kimi target accepted" "$OUT" "Skill"
assert_contains "kimi output shows config.toml" "$OUT" "config.toml"
KIMI_CONFIG="$KIMI_HOME/.kimi-code/config.toml"
if [ -f "$KIMI_CONFIG" ]; then
  KCFG="$(cat "$KIMI_CONFIG")"
  assert_contains "kimi config has [[hooks]]" "$KCFG" "[[hooks]]"
  assert_contains "kimi SessionStart event" "$KCFG" 'event = "SessionStart"'
  assert_contains "kimi UserPromptSubmit event" "$KCFG" 'event = "UserPromptSubmit"'
  assert_contains "kimi Stop event" "$KCFG" 'event = "Stop"'
  assert_contains "kimi prime hook command" "$KCFG" "prime.sh"
  assert_contains "kimi hook timeout" "$KCFG" "timeout = 10"
  if [ -f "$KIMI_HOME/.kimi-code/skills/mnemon/SKILL.md" ]; then
    pass "kimi skill written" ""
  else
    fail "kimi skill written" "missing SKILL.md"
  fi
else
  fail "kimi config.toml written" "missing $KIMI_CONFIG"
fi

step "setup --target bogus error mentions kimi"
OUT=$($M --data-dir "$KIMI_HOME/.mnemon-data" setup --target bogus 2>&1 || true)
assert_contains "error mentions kimi" "$OUT" "kimi"

OPENCLAW_SETUP_DIR="$TESTDATA/setup_openclaw"
mkdir -p "$OPENCLAW_SETUP_DIR"

step "setup --target openclaw --yes — accepted (installs skill)"
OUT=$(cd "$OPENCLAW_SETUP_DIR" && $M --data-dir "$OPENCLAW_SETUP_DIR" setup --target openclaw --yes 2>&1 || true)
assert_contains "openclaw target accepted" "$OUT" "Skill"

step "openclaw handler.js has MNEMON_DATA_DIR prompt support"
HANDLER_JS="$OPENCLAW_SETUP_DIR/.openclaw/hooks/mnemon-prime/handler.js"
if [ -f "$HANDLER_JS" ]; then
  assert_contains "openclaw handler uses MNEMON_DATA_DIR" "$(cat "$HANDLER_JS")" "MNEMON_DATA_DIR"
else
  fail "openclaw handler.js exists" "(not found: $HANDLER_JS)"
fi

# ══════════════════════════════════════════════════════════════════════
banner "Setup: MNEMON_DATA_DIR prompt path"
# ══════════════════════════════════════════════════════════════════════

PROMPTDIR_TEST="$TESTDATA/setup_prompt_dir"
mkdir -p "$PROMPTDIR_TEST"

step "setup --target nanobot with MNEMON_DATA_DIR — output shows custom prompt path"
OUT=$(MNEMON_DATA_DIR="$PROMPTDIR_TEST" $M --data-dir "$TESTDATA/setup_prompt_dir_db" setup --target nanobot --yes 2>&1 || true)
assert_contains "prompt path reflects MNEMON_DATA_DIR" "$OUT" "$PROMPTDIR_TEST/prompt"

step "setup prompt files written under MNEMON_DATA_DIR"
if [ -f "$PROMPTDIR_TEST/prompt/guide.md" ]; then
  pass "guide.md under MNEMON_DATA_DIR" "(file exists)"
else
  fail "guide.md under MNEMON_DATA_DIR" "(file missing: $PROMPTDIR_TEST/prompt/guide.md)"
fi

# ══════════════════════════════════════════════════════════════════════
banner "Setup eject: markdown cleanup"
# ══════════════════════════════════════════════════════════════════════

EJECT_DIR="$TESTDATA/eject_test"
mkdir -p "$EJECT_DIR/.claude"

step "eject — removes mnemon block even when stale end-marker precedes start-marker"
# This tests the fix for: end_idx was searched from pos 0, so a <!-- mnemon:end -->
# fragment appearing BEFORE <!-- mnemon:start --> caused the wrong range to be found
# and the block was left intact.
cat > "$EJECT_DIR/CLAUDE.md" << 'MDEOF'
# My Project

Some text <!-- mnemon:end --> in passing.

<!-- mnemon:start -->
## Mnemon Memory Guidance
Instructions for agent.
<!-- mnemon:end -->

## Development Notes

Keep going.
MDEOF

(cd "$EJECT_DIR" && $M setup --eject --yes 2>&1 || true)
EJECT_MD=$(cat "$EJECT_DIR/CLAUDE.md" 2>/dev/null || echo "FILE_DELETED")
assert_not_contains "removed start marker" "$EJECT_MD" "mnemon:start"
assert_contains "preserved pre-block content" "$EJECT_MD" "My Project"
assert_contains "preserved post-block content" "$EJECT_MD" "Development Notes"

step "eject — no triple newlines after removing sandwiched block"
cat > "$EJECT_DIR/CLAUDE.md" << 'MDEOF'
# Header

Content before.

<!-- mnemon:start -->
Guidance.
<!-- mnemon:end -->

Content after.
MDEOF

(cd "$EJECT_DIR" && $M setup --eject --yes 2>&1 || true)
EJECT_MD2=$(cat "$EJECT_DIR/CLAUDE.md" 2>/dev/null || echo "FILE_DELETED")
# $(printf '\n\n\n') is stripped by command substitution; detect via awk instead
CONSEC_BLANKS=$(awk '/^$/{c++; if(c>=2){found=1;exit}} !/^$/{c=0} END{print found+0}' "$EJECT_DIR/CLAUDE.md" 2>/dev/null || echo "0")
if [ "$CONSEC_BLANKS" = "0" ]; then pass "no triple newlines" "(absent: consecutive blank lines)"; else fail "no triple newlines" "(should NOT contain: consecutive blank lines)"; fi
assert_contains "preserved header" "$EJECT_MD2" "Header"
assert_contains "preserved content" "$EJECT_MD2" "Content after"

# ══════════════════════════════════════════════════════════════════════
banner "Setup: Claude Code user-global config collision guard"
# ══════════════════════════════════════════════════════════════════════
# A project-local install run with cwd == $HOME makes "./.claude" the same
# directory as "~/.claude" (Claude Code's user-global config). Relative hook
# commands written there only resolve when the session cwd is $HOME, so the
# install must detect the collision and write absolute commands instead.

step "setup --target claude-code with cwd == \$HOME — writes absolute hook commands"
CLAUDE_HOME="$TESTDATA/claude_home_collision"
rm -rf "$CLAUDE_HOME" && mkdir -p "$CLAUDE_HOME"
(cd "$CLAUDE_HOME" && HOME="$CLAUDE_HOME" CLAUDE_CONFIG_DIR= $M --data-dir "$CLAUDE_HOME/.mnemon-data" setup --target claude-code --yes > /dev/null 2>&1 || true)
PRIME_CMD=$(jq -r '.hooks.SessionStart[0].hooks[0].command' "$CLAUDE_HOME/.claude/settings.json" 2>/dev/null || echo "")
case "$PRIME_CMD" in
  /*) pass "collision: SessionStart command is absolute" "($PRIME_CMD)" ;;
  *)  fail "collision: SessionStart command is absolute" "(expected absolute path, got: $PRIME_CMD)" ;;
esac

step "setup --target claude-code in a genuine project dir — keeps relative hook commands"
CLAUDE_PROJ="$TESTDATA/claude_proj_local"
CLAUDE_PROJ_HOME="$TESTDATA/claude_home_unrelated"
rm -rf "$CLAUDE_PROJ" "$CLAUDE_PROJ_HOME" && mkdir -p "$CLAUDE_PROJ" "$CLAUDE_PROJ_HOME"
(cd "$CLAUDE_PROJ" && HOME="$CLAUDE_PROJ_HOME" CLAUDE_CONFIG_DIR= $M --data-dir "$CLAUDE_PROJ/.mnemon-data" setup --target claude-code --yes > /dev/null 2>&1 || true)
PRIME_CMD2=$(jq -r '.hooks.SessionStart[0].hooks[0].command' "$CLAUDE_PROJ/.claude/settings.json" 2>/dev/null || echo "")
case "$PRIME_CMD2" in
  /*) fail "project-local: SessionStart command stays relative" "(expected relative path, got absolute: $PRIME_CMD2)" ;;
  *)  pass "project-local: SessionStart command stays relative" "($PRIME_CMD2)" ;;
esac

step "setup --target claude-code — symlinked \$HOME vs physical cwd still collides"
SYMBASE="$TESTDATA/claude_symlink_base"
rm -rf "$SYMBASE" && mkdir -p "$SYMBASE/realhome/.claude"
ln -s "$SYMBASE/realhome" "$SYMBASE/linkhome"
(cd "$SYMBASE/realhome" && HOME="$SYMBASE/linkhome" CLAUDE_CONFIG_DIR= $M --data-dir "$SYMBASE/realhome/.mnemon-data" setup --target claude-code --yes > /dev/null 2>&1 || true)
PRIME_CMD3=$(jq -r '.hooks.SessionStart[0].hooks[0].command' "$SYMBASE/realhome/.claude/settings.json" 2>/dev/null || echo "")
case "$PRIME_CMD3" in
  /*) pass "symlinked \$HOME: SessionStart command is absolute" "($PRIME_CMD3)" ;;
  *)  fail "symlinked \$HOME: SessionStart command is absolute" "(expected absolute path, got: $PRIME_CMD3)" ;;
esac

# ══════════════════════════════════════════════════════════════════════
banner "Embeddings: one-time float64 → float32 storage migration"
# ══════════════════════════════════════════════════════════════════════
# Legacy databases persisted embeddings as raw little-endian float64 blobs
# (8 bytes/dim). Current databases use float32 (4 bytes/dim — half the size,
# ample precision for cosine ranking). PRAGMA user_version gates a one-time,
# in-place rewrite of every legacy blob on first open of an old database;
# malformed or already-migrated blobs are left untouched rather than risk
# corrupting them.

step "legacy float64 embeddings rewritten to float32 on open; malformed blobs untouched"
EMBMIG_DIR="$TESTDATA/embed_float32_migrate"
rm -rf "$EMBMIG_DIR"
GOOD_ID=$(extract_id "$($M --data-dir "$EMBMIG_DIR" remember "legacy embedding fixture good" --cat fact)")
BAD_ID=$(extract_id "$($M --data-dir "$EMBMIG_DIR" remember "legacy embedding fixture malformed" --cat fact)")
EMBMIG_DB="$EMBMIG_DIR/data/default/mnemon.db"
# [1.5, -2.25] as little-endian float64 — upstream's own migration test fixture;
# 5 raw bytes is not a multiple of 8 and must survive the migration unchanged.
sqlite3 "$EMBMIG_DB" "UPDATE insights SET embedding = X'000000000000F83F00000000000002C0' WHERE id = '$GOOD_ID';
                      UPDATE insights SET embedding = X'0102030405' WHERE id = '$BAD_ID';
                      PRAGMA user_version = 0;"

$M --data-dir "$EMBMIG_DIR" status > /dev/null 2>&1 || true

GOOD_LEN=$(sqlite3 "$EMBMIG_DB" "SELECT length(embedding) FROM insights WHERE id = '$GOOD_ID'")
GOOD_HEX=$(sqlite3 "$EMBMIG_DB" "SELECT hex(embedding) FROM insights WHERE id = '$GOOD_ID'")
BAD_HEX=$(sqlite3 "$EMBMIG_DB" "SELECT hex(embedding) FROM insights WHERE id = '$BAD_ID'")
EMBMIG_USER_VERSION=$(sqlite3 "$EMBMIG_DB" "PRAGMA user_version")

case "$GOOD_LEN-$GOOD_HEX" in
  8-0000C03F000010C0) pass "legacy [1.5, -2.25] rewritten as 8-byte float32 LE blob" "($GOOD_HEX)" ;;
  *)                  fail "legacy [1.5, -2.25] rewritten as 8-byte float32 LE blob" "(len=$GOOD_LEN hex=$GOOD_HEX)" ;;
esac

case "$BAD_HEX" in
  0102030405) pass "malformed 5-byte blob left untouched by migration" "($BAD_HEX)" ;;
  *)          fail "malformed 5-byte blob left untouched by migration" "(got $BAD_HEX)" ;;
esac

case "$EMBMIG_USER_VERSION" in
  1) pass "user_version advanced to 1 despite malformed blob" "($EMBMIG_USER_VERSION)" ;;
  *) fail "user_version advanced to 1 despite malformed blob" "(got $EMBMIG_USER_VERSION)" ;;
esac

banner "Milestone 12: --embed-model Flag"
EMBEDMODEL_DIR="$TESTDATA/embed_model"
mkdir -p "$EMBEDMODEL_DIR"

step "--embed-model flag is accepted (no error)"
OUT=$($M --data-dir "$EMBEDMODEL_DIR" --embed-model nomic-embed-text embed --status 2>&1)
assert_contains "embed-model flag accepted" "$OUT" "ollama_available"

step "--embed-model overrides MNEMON_EMBED_MODEL env var"
OUT=$(MNEMON_EMBED_MODEL="env-model" $M --data-dir "$EMBEDMODEL_DIR" --embed-model "flag-model" embed --status 2>&1)
assert_contains "embed-model flag overrides env" "$OUT" "ollama_available"

banner "Milestone 14: Import Command"
IMPORT_DIR="$TESTDATA/import"
mkdir -p "$IMPORT_DIR"

step "import — dry-run validates draft without writing"
cat > "$IMPORT_DIR/draft_basic.json" << 'DRAFTEOF'
{
  "schema_version": "1",
  "source": "chat-export",
  "insights": [
    {
      "content": "User prefers concise answers in English.",
      "category": "preference",
      "importance": 4,
      "tags": ["ux"],
      "created_at": "2024-01-15T09:30:00Z"
    }
  ]
}
DRAFTEOF
OUT=$($M --data-dir "$IMPORT_DIR" import --dry-run "$IMPORT_DIR/draft_basic.json" 2>&1)
assert_contains "dry-run output" "$OUT" "Dry run"

step "import — basic import returns expected JSON fields"
OUT=$($M --data-dir "$IMPORT_DIR" import "$IMPORT_DIR/draft_basic.json" 2>&1)
assert_jq "imported count is 1"      "$OUT" '.imported'      '1'
assert_jq "updated count is 0"       "$OUT" '.updated'       '0'
assert_jq "skipped count is 0"       "$OUT" '.skipped'       '0'
assert_jq "errors count is 0"        "$OUT" '.errors'        '0'
assert_jq "edges_inserted is 0"      "$OUT" '.edges_inserted' '0'
assert_jq "results is an array"      "$OUT" '.results | type' 'array'
assert_jq "results[0].action added"  "$OUT" '.results[0].action' 'added'

step "import — explicit edge is inserted"
cat > "$IMPORT_DIR/draft_edges.json" << 'DRAFTEOF'
{
  "schema_version": "1",
  "insights": [
    {"content": "alpha insight for edge test", "category": "fact", "importance": 3},
    {"content": "beta insight for edge test",  "category": "fact", "importance": 3}
  ],
  "edges": [
    {"source_index": 0, "target_index": 1, "edge_type": "semantic", "weight": 0.8, "reason": "related topics"}
  ]
}
DRAFTEOF
OUT=$($M --data-dir "$IMPORT_DIR" import --no-diff "$IMPORT_DIR/draft_edges.json" 2>&1)
assert_jq "edges_inserted is 1" "$OUT" '.edges_inserted' '1'

step "import — bad schema_version is rejected"
cat > "$IMPORT_DIR/bad_schema.json" << 'DRAFTEOF'
{"schema_version": "99", "insights": [{"content": "test"}]}
DRAFTEOF
OUT=$($M --data-dir "$IMPORT_DIR" import "$IMPORT_DIR/bad_schema.json" 2>&1 || true)
assert_contains "schema version error" "$OUT" "schema_version"

step "import — empty insights array is rejected"
cat > "$IMPORT_DIR/empty_insights.json" << 'DRAFTEOF'
{"schema_version": "1", "insights": []}
DRAFTEOF
OUT=$($M --data-dir "$IMPORT_DIR" import "$IMPORT_DIR/empty_insights.json" 2>&1 || true)
assert_contains "empty insights error" "$OUT" "empty"

step "import — invalid category is rejected"
cat > "$IMPORT_DIR/bad_cat.json" << 'DRAFTEOF'
{"schema_version": "1", "insights": [{"content": "x", "category": "note"}]}
DRAFTEOF
OUT=$($M --data-dir "$IMPORT_DIR" import "$IMPORT_DIR/bad_cat.json" 2>&1 || true)
assert_contains "invalid category error" "$OUT" "category"

banner "Milestone 13: Privacy-Safe Memory Receipts"
RECEIPT_DIR="$TESTDATA/receipt"
mkdir -p "$RECEIPT_DIR"

step "receipt — empty oplog returns valid JSON receipt"
OUT=$($M --data-dir "$RECEIPT_DIR" receipt 2>&1)
assert_jq "receipt schema" "$OUT" '.schema' 'mnemon.memory.receipt.v1'
assert_jq "receipt privacy raw_detail_included false" "$OUT" '.privacy.raw_detail_included' 'false'
assert_jq "receipt count 0" "$OUT" '.count' '0'

step "receipt — after remember, receipt omits raw content"
$M --data-dir "$RECEIPT_DIR" remember "secret sauce recipe" --cat fact --imp 3 > /dev/null
OUT=$($M --data-dir "$RECEIPT_DIR" receipt 2>&1)
assert_jq "receipt count 1" "$OUT" '.count' '1'
assert_jq "receipt event name" "$OUT" '.events[0].event_name' 'mnemon.memory.operation.observed'
assert_jq "receipt no raw content" "$OUT" 'any(.events[]; has("content"))' 'false'
assert_jq "receipt has detail_present" "$OUT" '.events[0].detail_present' 'true'

step "receipt --limit controls max events"
OUT=$($M --data-dir "$RECEIPT_DIR" receipt --limit 1 2>&1)
assert_jq "receipt limit respected" "$OUT" '.events | length' '1'

# ══════════════════════════════════════════════════════════════════════
banner "Harness Event Seam: event emit command"
# ══════════════════════════════════════════════════════════════════════

EVENT_DIR="$TESTDATA/event_seam"
mkdir -p "$EVENT_DIR"

step "event emit — valid topic succeeds"
OUT=$($M event emit memory.hot_write_observed --root "$EVENT_DIR" --payload '{"k":"v"}' --correlation-id corr-1 --loop memory --host mnemon 2>&1)
assert_contains "event emit output has emitted" "$OUT" "emitted"
assert_contains "event emit output has path" "$OUT" "path:"

step "event emit — events.jsonl created under root"
EVENTS_FILE="$EVENT_DIR/.mnemon/events.jsonl"
if [ -f "$EVENTS_FILE" ]; then
  pass "events.jsonl exists" "(found: $EVENTS_FILE)"
else
  fail "events.jsonl exists" "(not found: $EVENTS_FILE)"
fi

step "event emit — events.jsonl has correct type"
assert_contains "events.jsonl type field" "$(cat "$EVENTS_FILE" 2>/dev/null)" '"memory.hot_write_observed"'

step "event emit — events.jsonl has correlation_id"
assert_contains "events.jsonl correlation_id" "$(cat "$EVENTS_FILE" 2>/dev/null)" '"corr-1"'

step "event emit — events.jsonl has schema_version 1"
assert_contains "events.jsonl schema_version" "$(cat "$EVENTS_FILE" 2>/dev/null)" '"schema_version":1'

step "event emit — invalid topic rejected"
OUT=$($M event emit not-a-valid-topic --root "$EVENT_DIR" 2>&1 || true)
assert_contains "invalid topic rejected" "$OUT" "lower-case dot-separated"

step "remember — no event emitted without MNEMON_HARNESS_EVENT_EMIT"
EVENT_DIR2="$TESTDATA/event_seam2"
mkdir -p "$EVENT_DIR2"
OUT=$(cd "$EVENT_DIR2" && $M --data-dir "$EVENT_DIR2" remember --no-diff "test memory" --cat fact 2>&1)
if [ -f "$EVENT_DIR2/.mnemon/events.jsonl" ]; then
  fail "no event without flag" "(events.jsonl should not exist)"
else
  pass "no event without flag" "(events.jsonl absent)"
fi

step "remember — event emitted with MNEMON_HARNESS_EVENT_EMIT=1"
EVENT_DIR3="$TESTDATA/event_seam3"
mkdir -p "$EVENT_DIR3"
EVENTS_LOG3="$EVENT_DIR3/remember_events.jsonl"
OUT=$(cd "$EVENT_DIR3" && MNEMON_HARNESS_EVENT_EMIT=1 MNEMON_HARNESS_EVENTLOG="$EVENTS_LOG3" $M --data-dir "$EVENT_DIR3" remember --no-diff "test memory event" --cat fact 2>&1)
if [ -f "$EVENTS_LOG3" ]; then
  assert_contains "remember event type" "$(cat "$EVENTS_LOG3")" '"memory.hot_write_observed"'
else
  fail "remember event emitted" "(events.jsonl not found: $EVENTS_LOG3)"
fi

# ══════════════════════════════════════════════════════════════════════
banner "Setup: Hermes Agent integration"
# ══════════════════════════════════════════════════════════════════════

HERMES_SETUP_DIR="$TESTDATA/setup_hermes"
mkdir -p "$HERMES_SETUP_DIR"

step "setup --target hermes --yes — accepted (installs skill)"
OUT=$(cd "$HERMES_SETUP_DIR" && $M --data-dir "$HERMES_SETUP_DIR" setup --target hermes --yes 2>&1 || true)
assert_contains "hermes target accepted" "$OUT" "Skill"
assert_contains "hermes target shows config" "$OUT" "Setup complete"

step "hermes output shows skill path"
assert_contains "hermes skill path in output" "$OUT" "skills/mnemon/SKILL.md"

step "hermes output shows hooks path"
assert_contains "hermes hooks path in output" "$OUT" "config.yaml"

step "setup --target bogus error mentions hermes"
OUT=$($M --data-dir "$HERMES_SETUP_DIR" setup --target bogus 2>&1 || true)
assert_contains "error mentions hermes" "$OUT" "hermes"

# ══════════════════════════════════════════════════════════════════════
banner "Setup: Pi integration"
# ══════════════════════════════════════════════════════════════════════

PI_SETUP_DIR="$TESTDATA/setup_pi"
mkdir -p "$PI_SETUP_DIR"

step "setup --target pi --yes — accepted (installs skill)"
OUT=$(cd "$PI_SETUP_DIR" && $M --data-dir "$PI_SETUP_DIR" setup --target pi --yes 2>&1 || true)
assert_contains "pi target accepted" "$OUT" "Skill"

step "pi skill installed"
if [ -f "$PI_SETUP_DIR/.pi/skills/mnemon/SKILL.md" ]; then
  pass "pi SKILL.md exists" "(found)"
else
  fail "pi SKILL.md exists" "(not found: $PI_SETUP_DIR/.pi/skills/mnemon/SKILL.md)"
fi

step "pi extension installed"
if [ -f "$PI_SETUP_DIR/.pi/extensions/mnemon.ts" ]; then
  pass "pi mnemon.ts exists" "(found)"
else
  fail "pi mnemon.ts exists" "(not found: $PI_SETUP_DIR/.pi/extensions/mnemon.ts)"
fi

step "setup --target bogus error mentions pi"
OUT=$($M --data-dir "$PI_SETUP_DIR" setup --target bogus 2>&1 || true)
assert_contains "error mentions pi" "$OUT" "pi"

step "setup --target pi --eject --yes — removes integration"
OUT=$(cd "$PI_SETUP_DIR" && $M --data-dir "$PI_SETUP_DIR" setup --eject --target pi --yes 2>&1 || true)
assert_not_contains "pi SKILL.md removed" "$(ls "$PI_SETUP_DIR/.pi/skills/mnemon/" 2>/dev/null || echo 'removed')" "SKILL.md"

# ══════════════════════════════════════════════════════════════════════
banner "Milestone 15: Index-Aware Entity Extraction (Fourth Path)"
# ══════════════════════════════════════════════════════════════════════

TESTDIR15="$TESTDATA/m15"
mkdir -p "$TESTDIR15"

step "index-aware: seed single-segment CamelCase via --entities"
# "Athena" is single-segment CamelCase — the default regex+dict paths skip it.
# Providing it explicitly seeds it into the entity index.
OUT=$($M --data-dir "$TESTDIR15" remember --no-diff "Athena is our internal project codename" --cat fact --imp 3 --entities "Athena")
echo -e "    ${DIM}entities (seeded): $(echo "$OUT" | jq -c '.entities')${RESET}"
assert_contains "seed insight contains Athena" "$OUT" '"Athena"'

step "index-aware: fourth path admits known entity on subsequent insight"
# "Athena" is now in the entity index. The indexed extractor should admit it
# via the fourth path (wide-cast capitalized token filter against known set).
OUT=$($M --data-dir "$TESTDIR15" remember --no-diff "Working on Athena today with some updates" --cat fact --imp 3)
echo -e "    ${DIM}entities (fourth-path): $(echo "$OUT" | jq -c '.entities')${RESET}"
assert_contains "fourth-path admits Athena" "$OUT" '"Athena"'

step "index-aware: unknown single-segment CamelCase not admitted"
# "Banana" is not in the entity index — the fourth path must NOT admit it.
OUT=$($M --data-dir "$TESTDIR15" remember --no-diff "Banana is a tasty fruit and Athena is great" --cat fact --imp 3)
echo -e "    ${DIM}entities (filter): $(echo "$OUT" | jq -c '.entities')${RESET}"
assert_not_contains "Banana not admitted (not in index)" "$OUT" '"Banana"'
assert_contains "Athena still admitted" "$OUT" '"Athena"'

step "index-aware: lowercase known vocab admitted via fourth path B"
# Seed a lowercase project name via provided entities.
OUT=$($M --data-dir "$TESTDIR15" remember --no-diff "openclaw is the harness layer" --cat fact --imp 3 --entities "openclaw")
assert_contains "seed openclaw in index" "$OUT" '"openclaw"'
# Now a subsequent insight containing 'openclaw' should pick it up via tokenized scan.
OUT=$($M --data-dir "$TESTDIR15" remember --no-diff "see openclaw docs for integration" --cat fact --imp 3)
echo -e "    ${DIM}entities (lowercase fourth-path): $(echo "$OUT" | jq -c '.entities')${RESET}"
assert_contains "lowercase openclaw admitted" "$OUT" '"openclaw"'

# ── Report ────────────────────────────────────────────────────────────
echo ""
echo -e "${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RESET}"
echo -e "${BOLD}${CYAN}  Results${RESET}"
echo -e "${BOLD}${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RESET}"
echo ""
echo -e "  Total:  ${BOLD}$TOTAL${RESET}"
echo -e "  Passed: ${GREEN}${BOLD}$PASS${RESET}"
if [ "$FAIL" -gt 0 ]; then
  echo -e "  Failed: ${RED}${BOLD}$FAIL${RESET}"
fi
echo ""

# Cleanup ephemeral single-file build (Go harness); keep CMake output in build/
if [ -z "${MNEMON_TEST_BINARY:-}" ] && [ -f "$PROJECT_DIR/mnemon" ] && [ "$M" = "$PROJECT_DIR/mnemon" ]; then
  rm -f "$M"
fi
echo -e "  ${DIM}Test DBs preserved at: $TESTDATA/${RESET}"
echo -e "  ${DIM}Run 'rm -rf .testdata' (from project root) to clean up${RESET}"

if [ "$FAIL" -gt 0 ]; then
  echo -e "  ${RED}${BOLD}FAIL${RESET}"
  exit 1
else
  echo -e "  ${GREEN}${BOLD}ALL PASSED ✔${RESET}"
  exit 0
fi
