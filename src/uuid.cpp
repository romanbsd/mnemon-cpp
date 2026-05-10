#include "uuid.hpp"

#include <array>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

namespace mnemon {

// RFC 4122 variant 4: random 122 bits + version/variant nibbles; prefer OS CSPRNG (/dev/urandom).
std::string new_uuid_v4() {
  std::array<unsigned char, 16> b{};

  {
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (urandom) {
      urandom.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(b.size()));
    }
    if (!urandom || urandom.gcount() != static_cast<std::streamsize>(b.size())) {
      std::random_device rd;
      for (auto& x : b) {
        x = static_cast<unsigned char>(rd());
      }
    }
  }

  b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x40);
  b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80);
  auto hex2 = [](unsigned char c) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s += d[c >> 4];
    s += d[c & 0xF];
    return s;
  };
  std::string s;
  for (int i = 0; i < 4; ++i) {
    s += hex2(b[static_cast<size_t>(i)]);
  }
  s += '-';
  for (int i = 4; i < 6; ++i) {
    s += hex2(b[static_cast<size_t>(i)]);
  }
  s += '-';
  for (int i = 6; i < 8; ++i) {
    s += hex2(b[static_cast<size_t>(i)]);
  }
  s += '-';
  for (int i = 8; i < 10; ++i) {
    s += hex2(b[static_cast<size_t>(i)]);
  }
  s += '-';
  for (int i = 10; i < 16; ++i) {
    s += hex2(b[static_cast<size_t>(i)]);
  }
  return s;
}

} // namespace mnemon
