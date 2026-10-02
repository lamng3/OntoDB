#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ontodb {

using term_id_t = uint64_t;
using page_id_t = int32_t;
using frame_id_t = int32_t;
using txn_id_t = int64_t;
using lsn_t = int64_t;
using slot_offset_t = uint16_t;

inline constexpr term_id_t INVALID_TERM_ID = static_cast<term_id_t>(-1);
inline constexpr page_id_t INVALID_PAGE_ID = -1;
inline constexpr frame_id_t INVALID_FRAME_ID = -1;
inline constexpr txn_id_t INVALID_TXN_ID = -1;
// In-memory sentinel. A stored pageLSN of 0 means the page has never been
// updated under write-ahead logging.
inline constexpr lsn_t INVALID_LSN = -1;
inline constexpr slot_offset_t INVALID_SLOT = static_cast<slot_offset_t>(-1);

struct Triple {
  term_id_t subject{INVALID_TERM_ID};
  term_id_t predicate{INVALID_TERM_ID};
  term_id_t object{INVALID_TERM_ID};

  friend auto operator==(const Triple& a, const Triple& b) -> bool {
    return a.subject == b.subject && a.predicate == b.predicate && a.object == b.object;
  }

  friend auto operator<(const Triple& a, const Triple& b) -> bool {
    if (a.subject != b.subject) {
      return a.subject < b.subject;
    }
    if (a.predicate != b.predicate) {
      return a.predicate < b.predicate;
    }
    return a.object < b.object;
  }
};

// A bound position is a dictionary id. An unbound position is nullopt.
struct TriplePattern {
  std::optional<term_id_t> subject;
  std::optional<term_id_t> predicate;
  std::optional<term_id_t> object;
};

inline auto Matches(const Triple& triple, const TriplePattern& pattern) -> bool {
  if (pattern.subject.has_value() && *pattern.subject != triple.subject) {
    return false;
  }
  if (pattern.predicate.has_value() && *pattern.predicate != triple.predicate) {
    return false;
  }
  if (pattern.object.has_value() && *pattern.object != triple.object) {
    return false;
  }
  return true;
}

// One solution. INVALID_TERM_ID marks a variable this operator has not bound.
using Row = std::vector<term_id_t>;

struct Column {
  std::string name;
  uint32_t slot{0};
};

struct Schema {
  std::vector<Column> columns;
};

enum class BackendKind { kMem, kIndexed };

enum class JoinMethod { kAuto, kNestedLoop, kIndexNestedLoop, kHash };

enum class IsolationLevel { kReadUncommitted, kReadCommitted, kRepeatableRead, kSerializable };

struct Config {
  BackendKind backend{BackendKind::kMem};
  JoinMethod join{JoinMethod::kNestedLoop};
  IsolationLevel isolation{IsolationLevel::kReadCommitted};
  size_t pool_size{64};
  size_t lru_k{2};
  bool timing{false};
};

}  // namespace ontodb
