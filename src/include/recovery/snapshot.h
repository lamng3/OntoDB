#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "common/exception.h"
#include "common/types.h"
#include "concurrency/version_index.h"
#include "dictionary/dictionary.h"
#include "store/triple_store.h"

namespace ontodb {

// A full image of the memory database at one LSN: every dictionary term in id
// order and every triple. Written to a temporary file, fsynced, then renamed
// over the old one, so a crash leaves either the old snapshot or the new one,
// never half of each. The body carries a CRC-32C (PLAN 9.4).
//
//   ontodb-snapshot 2
//   lsn <last LSN the image includes>
//   txn <next transaction id>
//   clock <newest commit timestamp>
//   terms <n>
//   triples <n>                  how many are in the latest version
//   spans <n>                    every span, ended or not
//   tags <n>
//   crc32c <8 hex digits over everything after the blank line>
//
//   T<tab><canonical term>       one per id, ids 1..n in order
//   S <s> <p> <o> <begin>        a triple in the latest version, since that commit
//   H <s> <p> <o> <begin> <end>  a span that has ended
//   G <timestamp> <name>         a version's name
//
// A snapshot holds committed state only: it is written from the version index, whatever a
// transaction still open has done to the store. Version 1 has neither S's <begin> nor the other
// records. It still reads, as one commit holding every triple.
struct Snapshot {
  lsn_t lsn{0};
  txn_id_t next_txn{1};
  commit_ts_t clock{0};
  std::vector<std::string> terms;
  std::vector<Triple> triples;  // the latest version
  std::vector<VersionedTriple> spans;
  std::vector<VersionTag> tags;
};

class SnapshotCorruptionException : public StorageException {
 public:
  using StorageException::StorageException;
};

void WriteSnapshot(const std::filesystem::path& path, const Dictionary& dictionary,
                   const VersionIndex& versions, lsn_t lsn, txn_id_t next_txn);

// Throws SnapshotCorruptionException on a bad header, a count that does not
// match, or a checksum mismatch.
auto ReadSnapshot(const std::filesystem::path& path) -> Snapshot;

}  // namespace ontodb
