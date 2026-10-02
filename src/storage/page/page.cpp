#include "storage/page/page.h"

namespace ontodb {

auto Page::GetLSN() const -> lsn_t {
  uint64_t value = 0;
  for (size_t i = 0; i < kPageLsnSize; i++) {
    value |= static_cast<uint64_t>(static_cast<unsigned char>(data_[kPageLsnOffset + i]))
             << (8 * i);
  }
  return static_cast<lsn_t>(value);
}

void Page::SetLSN(lsn_t lsn) {
  auto value = static_cast<uint64_t>(lsn);
  for (size_t i = 0; i < kPageLsnSize; i++) {
    data_[kPageLsnOffset + i] = static_cast<char>(value & 0xFF);
    value >>= 8;
  }
}

}  // namespace ontodb
