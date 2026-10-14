#include "common/crc32c.h"

#include <array>

namespace ontodb {
namespace {

constexpr uint32_t kPolynomial = 0x82F63B78;  // reflected Castagnoli

constexpr auto MakeTable() -> std::array<uint32_t, 256> {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t crc = i;
    for (int bit = 0; bit < 8; bit++) {
      crc = (crc & 1) != 0 ? (crc >> 1) ^ kPolynomial : crc >> 1;
    }
    table[i] = crc;
  }
  return table;
}

constexpr auto kTable = MakeTable();

}  // namespace

auto Crc32c(const void* data, size_t len, uint32_t seed) -> uint32_t {
  const auto* bytes = static_cast<const unsigned char*>(data);
  uint32_t crc = ~seed;
  for (size_t i = 0; i < len; i++) {
    crc = kTable[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
  }
  return ~crc;
}

}  // namespace ontodb
