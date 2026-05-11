#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "vector_math.hpp"

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
