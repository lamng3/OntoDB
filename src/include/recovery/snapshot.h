#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "common/exception.h"
#include "common/types.h"
#include "dictionary/dictionary.h"
#include "store/triple_store.h"

namespace ontodb {

// A full image of the memory database at one LSN: every dictionary term in id
// order and every triple. Written to a temporary file, fsynced, then renamed
// over the old one, so a crash leaves either the old snapshot or the new one,
// never half of each. The body carries a CRC-32C (PLAN 9.4).
//
//   ontodb-snapshot 1
//   lsn <last LSN the image includes>
//   txn <next transaction id>
//   terms <n>
//   triples <n>
//   crc32c <8 hex digits over everything after the blank line>
//
//   T<tab><canonical term>       one per id, ids 1..n in order
//   S <s> <p> <o>                one per triple
struct Snapshot {
  lsn_t lsn{0};
  txn_id_t next_txn{1};
  std::vector<std::string> terms;
  std::vector<Triple> triples;
};

class SnapshotCorruptionException : public StorageException {
 public:
  using StorageException::StorageException;
};

void WriteSnapshot(const std::filesystem::path& path, const Dictionary& dictionary,
                   TripleStore& store, lsn_t lsn, txn_id_t next_txn);

// Throws SnapshotCorruptionException on a bad header, a count that does not
// match, or a checksum mismatch.
auto ReadSnapshot(const std::filesystem::path& path) -> Snapshot;

}  // namespace ontodb
