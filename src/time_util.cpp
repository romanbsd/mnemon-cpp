#include "time_util.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#if defined(_WIN32)
#include <ctime>
#else
#include <ctime>
#endif

namespace mnemon::time_util {

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
  unsigned y = 0, mon = 0, day = 0, h = 0, min = 0, sec = 0;
  int n = 0;
  if (s.ends_with('Z')) {
    n = std::sscanf(s.c_str(), "%u-%u-%uT%u:%u:%uZ", &y, &mon, &day, &h, &min, &sec);
  } else {
    // +00:00 or offset — minimal support for SQLite datetime('now') style
    n = std::sscanf(s.c_str(), "%u-%u-%u %u:%u:%u", &y, &mon, &day, &h, &min, &sec);
  }
  if (n != 6) {
    throw std::runtime_error("bad rfc3339: " + s);
  }
  std::tm tm{};
  tm.tm_year = static_cast<int>(y) - 1900;
  tm.tm_mon = static_cast<int>(mon) - 1;
  tm.tm_mday = static_cast<int>(day);
  tm.tm_hour = static_cast<int>(h);
  tm.tm_min = static_cast<int>(min);
  tm.tm_sec = static_cast<int>(sec);
  tm.tm_isdst = 0;
#if defined(_WIN32)
  std::time_t t = _mkgmtime(&tm);
#else
  std::time_t t = timegm(&tm);
#endif
  if (t < 0) {
    throw std::runtime_error("bad rfc3339 time: " + s);
  }
  return std::chrono::system_clock::from_time_t(t);
}

TimePoint now_utc() {
  return Clock::now();
}

} // namespace mnemon::time_util
