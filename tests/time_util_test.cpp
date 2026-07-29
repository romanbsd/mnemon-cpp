#include "time_util.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

TEST_CASE("RFC3339 parser accepts exact UTC and SQLite timestamps") {
  REQUIRE(mnemon::time_util::rfc3339_utc(
              mnemon::time_util::parse_rfc3339("2024-02-29T09:30:45Z")) ==
          "2024-02-29T09:30:45Z");
  REQUIRE(mnemon::time_util::rfc3339_utc(
              mnemon::time_util::parse_rfc3339("2024-02-29 09:30:45")) ==
          "2024-02-29T09:30:45Z");
}

TEST_CASE("RFC3339 parser rejects malformed and out-of-range timestamps") {
  const std::vector<std::string> invalid = {
      "2024-01-15T09:30:00garbageZ",
      "2024-01-15T09:30:00Ztrailing",
      "2023-02-29T09:30:00Z",
      "2024-13-01T09:30:00Z",
      "2024-01-32T09:30:00Z",
      "2024-01-15T24:00:00Z",
      "2024-01-15T09:60:00Z",
      "2024-01-15T09:30:60Z",
      "1969-12-31T23:59:59Z",
  };

  for (const auto& timestamp : invalid) {
    CAPTURE(timestamp);
    REQUIRE_THROWS(mnemon::time_util::parse_rfc3339(timestamp));
  }
}
