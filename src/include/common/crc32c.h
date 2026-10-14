#pragma once

#include <cstddef>
#include <cstdint>

namespace ontodb {

// CRC-32C (Castagnoli), the checksum PostgreSQL, RocksDB, and InnoDB put on
// WAL records. Software table version; `seed` chains calls over split buffers.
auto Crc32c(const void* data, size_t len, uint32_t seed = 0) -> uint32_t;

}  // namespace ontodb
