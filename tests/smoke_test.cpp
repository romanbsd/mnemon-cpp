#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "vector_math.hpp"

#include "../src/diff.hpp"
#include "../src/keyword.hpp"
#include "../src/model.hpp"
#include "../src/tokenize.hpp"

using namespace mnemon;
using namespace mnemon::search_engine;

// --- cosine threshold ---

TEST_CASE("diff: cosine 0.75 does not override token sim (same-domain different-fact)") {
  // Regression: cosine ~0.75 for same-domain different-fact pairs must not trigger UPDATE.
  // Old threshold was 0.70; 0.75 > 0.70 triggered cosine override → false UPDATE.
  Insight base;
  base.id      = "kinabalu";
  base.content = "Dichorragia nesimachus singleton at Kinabalu Park, Sabah.";

  // Two unit vectors with cosine similarity ≈ 0.75
  std::vector<float> new_vec  = {1.0f, 0.0f};
  std::vector<float> old_vec  = {0.75f, 0.6614f}; // cos(new, old) ≈ 0.75

  DiffOptions opts;
  opts.limit        = 5;
  opts.new_embedding = new_vec;
  opts.existing_embed.push_back({"kinabalu", &old_vec});

  auto res = diff_insights(
      {base},
      "Dichorragia nesimachus first record in Bentong, Pahang.",
      opts);

  REQUIRE(res.suggestion == DiffSuggestion::Add);
}

// --- Jaccard similarity ---

TEST_CASE("jaccard: identical strings score 1.0") {
  REQUIRE(jaccard_similarity("Go uses SQLite", "Go uses SQLite") == Catch::Approx(1.0));
}

TEST_CASE("jaccard: disjoint strings score 0.0") {
  REQUIRE(jaccard_similarity("apple banana", "dog elephant") == Catch::Approx(0.0));
}

TEST_CASE("jaccard: same-domain different-fact pair scores low") {
  // Shared tokens: species name + survey jargon; distinct tokens: location names
  double sim = jaccard_similarity(
      "Dichorragia nesimachus singleton at Kinabalu Park, Sabah.",
      "Dichorragia nesimachus first record in Bentong, Pahang.");
  REQUIRE(sim < 0.5);
}

TEST_CASE("diff: jaccard used in dedup — same-domain different-fact produces ADD") {
  Insight base;
  base.id      = "kinabalu";
  base.content = "Dichorragia nesimachus singleton at Kinabalu Park, Sabah.";

  auto res = diff_insights(
      {base},
      "Dichorragia nesimachus first record in Bentong, Pahang.",
      DiffOptions{});

  REQUIRE(res.suggestion == DiffSuggestion::Add);
}

// --- negation words / conflict gate ---

TEST_CASE("diff: 'not' in scientific text does not trigger CONFLICT") {
  // Regression: bare "not" removed from negation list — it appears constantly in scientific text.
  Insight base;
  base.id      = "prev";
  base.content = "Species not previously recorded in this region";

  auto res = diff_insights(
      {base},
      "Species not previously recorded in Pahang.",
      DiffOptions{});

  // Must be ADD or UPDATE, not CONFLICT
  REQUIRE(res.suggestion != DiffSuggestion::Conflict);
}

TEST_CASE("diff: 'no longer' still triggers CONFLICT at high similarity") {
  Insight base;
  base.id      = "prev";
  base.content = "Uses Redis for caching";

  auto res = diff_insights(
      {base},
      "No longer uses Redis for caching",
      DiffOptions{});

  // "no longer" is a genuine state-change signal at high Jaccard similarity
  REQUIRE(res.suggestion == DiffSuggestion::Conflict);
}

// --- sort matches by similarity ---

TEST_CASE("diff: sort by similarity — UPDATE not masked by lower-Jaccard ADD candidate") {
  // insA: high importance so keyword_search returns it first.
  //   keyword_score = 3/3 = 1.0 (all query tokens present),
  //   but 5 extra tokens → Jaccard = 3/8 = 0.375 → ADD.
  // insB: lower importance (keyword_search returns second),
  //   Jaccard(query, insB) = 3/4 = 0.75 → UPDATE.
  // Without sort: matches[0] = insA (ADD) → overall = ADD (wrong).
  // With sort by similarity: matches[0] = insB (UPDATE, sim=0.75) → overall = UPDATE.
  Insight insA;
  insA.id         = "a";
  insA.content    = "Redis caching performance reliability scalability durability availability consistency";
  insA.importance = 5;

  Insight insB;
  insB.id         = "b";
  insB.content    = "Redis caching performance solution";
  insB.importance = 1;

  auto res = diff_insights(
      {insA, insB},
      "Redis caching performance",
      DiffOptions{});

  REQUIRE(res.suggestion == DiffSuggestion::Update);
}

TEST_CASE("tokenize preserves non-BMP UTF-8 tokens") {
  // Regression: 4-byte code points decoded successfully, but the word builder
  // only re-encoded up to 3-byte UTF-8 sequences.
  const auto tokens = tokenize("script \xF0\x90\x90\xB7 marker");
  REQUIRE(tokens.count("\xF0\x90\x90\xB7") == 1);
  REQUIRE(tokens.count("script") == 1);
  REQUIRE(tokens.count("marker") == 1);
}

// placeholder to keep the binary alive before any other tests are added
TEST_CASE("placeholder") { REQUIRE(true); }

TEST_CASE("float runtime vectors serialize as compatible float64 blobs") {
  std::vector<float> runtime{3.0f, 4.0f};
  mnemon::normalize_vector(runtime);

  const auto blob = mnemon::serialize_vector(runtime);
  REQUIRE(blob.size() == runtime.size() * sizeof(double));

  const auto compat = mnemon::deserialize_vector_double(blob);
  REQUIRE(compat.size() == runtime.size());
  REQUIRE(compat[0] == Catch::Approx(0.6));
  REQUIRE(compat[1] == Catch::Approx(0.8));

  const auto restored = mnemon::deserialize_vector(blob);
  REQUIRE(restored.size() == runtime.size());
  REQUIRE(restored[0] == Catch::Approx(runtime[0]));
  REQUIRE(restored[1] == Catch::Approx(runtime[1]));
}

TEST_CASE("cosine_similarity_many matches single cosine behavior") {
  const std::vector<float> q{1.0f, 2.0f, 3.0f};
  const std::vector<float> a{1.0f, 2.0f, 3.0f};
  const std::vector<float> b{3.0f, 2.0f, 1.0f};
  const std::vector<float> z{0.0f, 0.0f, 0.0f};
  const std::vector<float> bad{1.0f, 2.0f};
  const std::vector<const std::vector<float>*> many{&a, &b, &z, &bad};

  const auto sims = mnemon::cosine_similarity_many(q, many);
  REQUIRE(sims.size() == many.size());
  REQUIRE(sims[0] == Catch::Approx(mnemon::cosine_similarity(q, a)));
  REQUIRE(sims[1] == Catch::Approx(mnemon::cosine_similarity(q, b)));
  REQUIRE(sims[2] == Catch::Approx(0.0f));
  REQUIRE(sims[3] == Catch::Approx(0.0f));
}
