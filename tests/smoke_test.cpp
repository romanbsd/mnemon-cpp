#include <catch2/catch_test_macros.hpp>

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

// placeholder to keep the binary alive before any other tests are added
TEST_CASE("placeholder") { REQUIRE(true); }
