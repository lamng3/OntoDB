#pragma once

#include <cstddef>
#include <cstdint>

#include "common/types.h"

namespace ontodb {

// 24-byte key: subject, predicate, object as big-endian uint64. Order is
// memcmp on the raw bytes. The layout is fixed; encode, decode, and the
// prefix bounds are PLAN 0.2.
//
// PrefixLower(key, n) is the smallest key whose first n components equal
// key's. PrefixUpper(key, n) is the smallest key strictly above every key
// with that prefix. n is 0, 1, 2, or 3. A scan of [lower, upper) visits
// exactly the matching prefix.
class TripleKey {
 public:
  static constexpr size_t SIZE = 24;

  TripleKey() = default;

  static auto Encode(term_id_t subject, term_id_t predicate, term_id_t object) -> TripleKey;
  void Decode(term_id_t* subject, term_id_t* predicate, term_id_t* object) const;
  auto Compare(const TripleKey& other) const -> int;

  static auto PrefixLower(const TripleKey& key, int prefix_len) -> TripleKey;
  static auto PrefixUpper(const TripleKey& key, int prefix_len) -> TripleKey;

  auto Data() const -> const uint8_t* { return bytes_; }
  auto Data() -> uint8_t* { return bytes_; }

 private:
  uint8_t bytes_[SIZE]{};
};

static_assert(sizeof(TripleKey) == TripleKey::SIZE, "TripleKey must be 24 bytes");

}  // namespace ontodb
