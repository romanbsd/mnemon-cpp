#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

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
  std::vector<double> new_vec  = {1.0, 0.0};
  std::vector<double> old_vec  = {0.75, 0.6614}; // cos(new, old) ≈ 0.75

  DiffOptions opts;
  opts.limit        = 5;
  opts.new_embedding = new_vec;
  opts.existing_embed.push_back({"kinabalu", old_vec});

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

// placeholder to keep the binary alive before any other tests are added
TEST_CASE("placeholder") { REQUIRE(true); }

TEST_CASE("cosine_similarity_many matches single cosine behavior") {
  const std::vector<double> q{1.0, 2.0, 3.0};
  const std::vector<double> a{1.0, 2.0, 3.0};
  const std::vector<double> b{3.0, 2.0, 1.0};
  const std::vector<double> z{0.0, 0.0, 0.0};
  const std::vector<double> bad{1.0, 2.0};
  const std::vector<const std::vector<double>*> many{&a, &b, &z, &bad};

  const auto sims = mnemon::cosine_similarity_many(q, many);
  REQUIRE(sims.size() == many.size());
  REQUIRE(sims[0] == Catch::Approx(mnemon::cosine_similarity(q, a)));
  REQUIRE(sims[1] == Catch::Approx(mnemon::cosine_similarity(q, b)));
  REQUIRE(sims[2] == Catch::Approx(0.0));
  REQUIRE(sims[3] == Catch::Approx(0.0));
}
