#pragma once

#include <memory>
#include <set>
#include <vector>

#include "concurrency/transaction.h"
#include "concurrency/version_index.h"
#include "store/mem_triple_iterator.h"
#include "store/triple_store.h"

namespace ontodb {

// The database as of one commit, and a transaction's own uncommitted changes on top when it has
// any. A scan reads the version index, so it waits for no transaction and sees nothing another
// transaction has not committed. Writes go to the store the locks protect; they reach this view
// through the transaction's write set, and other transactions' views when it commits.
class SnapshotStore : public TripleStore {
 public:
  SnapshotStore(const VersionIndex* versions, commit_ts_t at, TripleStore* live, Transaction* own)
      : versions_(versions), at_(at), live_(live), own_(own) {}

  auto Insert(const Triple& triple) -> bool override { return live_->Insert(triple); }
  auto Delete(const Triple& triple) -> bool override { return live_->Delete(triple); }

  auto Scan(const TriplePattern& pattern) -> std::unique_ptr<TripleIterator> override {
    auto found = versions_->Scan(pattern, at_);
    if (own_ != nullptr && !own_->GetWriteSet().empty()) {
      std::set<Triple> merged(found.begin(), found.end());
      for (const auto& write : own_->GetWriteSet()) {
        if (!Matches(write.triple, pattern)) {
          continue;
        }
        if (write.is_insert) {
          merged.insert(write.triple);
        } else {
          merged.erase(write.triple);
        }
      }
      found.assign(merged.begin(), merged.end());
    }
    return std::make_unique<MemTripleIterator>(std::move(found));
  }

 private:
  const VersionIndex* versions_;
  commit_ts_t at_;
  TripleStore* live_;
  Transaction* own_;
};

}  // namespace ontodb
