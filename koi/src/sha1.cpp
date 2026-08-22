#include "sha1.h"

#include <cstring>

namespace koi {
namespace {

std::uint32_t Rotl(std::uint32_t v, int by) { return (v << by) | (v >> (32 - by)); }

struct Sha1State {
  std::uint32_t h[5]{0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};

  // One 64-byte block, expanded in place into the 80-word schedule.
  void Block(const std::uint8_t* in) {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<std::uint32_t>(in[(i * 4) + 0]) << 24) |
             (static_cast<std::uint32_t>(in[(i * 4) + 1]) << 16) |
             (static_cast<std::uint32_t>(in[(i * 4) + 2]) << 8) |
             static_cast<std::uint32_t>(in[(i * 4) + 3]);
    }
    for (int i = 16; i < 80; ++i) w[i] = Rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    std::uint32_t a = h[0];
    std::uint32_t b = h[1];
    std::uint32_t c = h[2];
    std::uint32_t d = h[3];
    std::uint32_t e = h[4];
    for (int i = 0; i < 80; ++i) {
      std::uint32_t f = 0;
      std::uint32_t k = 0;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999u;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      const std::uint32_t t = Rotl(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = Rotl(b, 30);
      b = a;
      a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }
};

}

std::array<std::uint8_t, 20> Sha1(std::string_view bytes) {
  Sha1State state;
  const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
  std::size_t at = 0;
  for (; (bytes.size() - at) >= 64; at += 64) state.Block(data + at);

  // The tail: what is left, a 0x80 byte, zeroes, and the bit count big-endian
  // in the last eight. Two blocks when the remainder leaves no room for the
  // length, one otherwise.
  std::uint8_t tail[128]{};
  const std::size_t rest = bytes.size() - at;
  if (rest != 0) std::memcpy(tail, data + at, rest);
  tail[rest] = 0x80;
  const std::size_t total = (rest >= 56) ? 128 : 64;
  const std::uint64_t bits = static_cast<std::uint64_t>(bytes.size()) * 8;
  for (int i = 0; i < 8; ++i) {
    tail[total - 1 - static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFF);
  }
  for (std::size_t off = 0; off < total; off += 64) state.Block(tail + off);

  std::array<std::uint8_t, 20> out{};
  for (int i = 0; i < 5; ++i) {
    out[static_cast<std::size_t>(i * 4) + 0] = static_cast<std::uint8_t>(state.h[i] >> 24);
    out[static_cast<std::size_t>(i * 4) + 1] = static_cast<std::uint8_t>(state.h[i] >> 16);
    out[static_cast<std::size_t>(i * 4) + 2] = static_cast<std::uint8_t>(state.h[i] >> 8);
    out[static_cast<std::size_t>(i * 4) + 3] = static_cast<std::uint8_t>(state.h[i]);
  }
  return out;
}

std::string GitBlobOid(std::string_view bytes) {
  // The header and the content hash as one message, so this is the object id
  // git would print for the same bytes -- not a hash of the file's contents.
  std::string message = "blob " + std::to_string(bytes.size());
  message += '\0';
  message.append(bytes);
  const std::array<std::uint8_t, 20> digest = Sha1(message);

  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.resize(40);
  for (std::size_t i = 0; i < digest.size(); ++i) {
    out[i * 2] = kHex[digest[i] >> 4];
    out[(i * 2) + 1] = kHex[digest[i] & 0x0F];
  }
  return out;
}

}
