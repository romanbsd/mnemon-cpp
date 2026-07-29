#include "time_util.hpp"

#include <charconv>
#include <ctime>
#include <stdexcept>
#include <string_view>

namespace mnemon::time_util {

namespace {

unsigned parse_component(std::string_view input, size_t offset, size_t length) {
  unsigned value = 0;
  const char* begin = input.data() + offset;
  const char* end = begin + length;
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end) {
    throw std::runtime_error("bad rfc3339: " + std::string(input));
  }
  return value;
}

} // namespace

// Always UTC with Z suffix — wire format for JSON/SQLite timestamps in mnemon.
std::string rfc3339_utc(TimePoint tp) {
  using namespace std::chrono;
  auto secs = floor<seconds>(tp);
  std::time_t t = system_clock::to_time_t(secs);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[64];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

// Accepts strict RFC3339 UTC (...Z) or SQLite-style "YYYY-MM-DD HH:MM:SS" (UTC assumed).
TimePoint parse_rfc3339(const std::string& s) {
  const bool is_rfc3339_utc =
      s.size() == 20 && s[4] == '-' && s[7] == '-' && s[10] == 'T' &&
      s[13] == ':' && s[16] == ':' && s[19] == 'Z';
  const bool is_sqlite_utc =
      s.size() == 19 && s[4] == '-' && s[7] == '-' && s[10] == ' ' &&
      s[13] == ':' && s[16] == ':';
  if (!is_rfc3339_utc && !is_sqlite_utc) {
    throw std::runtime_error("bad rfc3339: " + s);
  }

  const unsigned y = parse_component(s, 0, 4);
  const unsigned mon = parse_component(s, 5, 2);
  const unsigned day = parse_component(s, 8, 2);
  const unsigned h = parse_component(s, 11, 2);
  const unsigned min = parse_component(s, 14, 2);
  const unsigned sec = parse_component(s, 17, 2);
  const std::chrono::year_month_day date{
      std::chrono::year{static_cast<int>(y)},
      std::chrono::month{mon},
      std::chrono::day{day},
  };
  if (!date.ok() || h > 23 || min > 59 || sec > 59) {
    throw std::runtime_error("bad rfc3339 time: " + s);
  }

  const std::chrono::sys_seconds parsed =
      std::chrono::sys_days{date} + std::chrono::hours{h} +
      std::chrono::minutes{min} + std::chrono::seconds{sec};
  const auto max_time = std::chrono::floor<std::chrono::seconds>(TimePoint::max());
  if (parsed < std::chrono::sys_seconds{} || parsed > max_time) {
    throw std::runtime_error("bad rfc3339 time: " + s);
  }
  return std::chrono::time_point_cast<Clock::duration>(parsed);
}

TimePoint now_utc() {
  return Clock::now();
}

} // namespace mnemon::time_util
