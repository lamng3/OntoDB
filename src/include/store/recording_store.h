#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "store/triple_store.h"

namespace ontodb {

// Forwards to another store and remembers every insert and delete that
// changed it, in order. Undo reverses them newest-first. A statement runs
// against one of these so its changes can be logged as one transaction, or
// taken back if the statement fails partway.
class RecordingStore : public TripleStore {
 public:
  struct Change {
    bool is_insert;
    Triple triple;
  };

  explicit RecordingStore(TripleStore* inner) : inner_(inner) {}

  auto Insert(const Triple& triple) -> bool override {
    const bool changed = inner_->Insert(triple);
    if (changed) {
      changes_.push_back({true, triple});
    }
    return changed;
  }

  auto Delete(const Triple& triple) -> bool override {
    const bool changed = inner_->Delete(triple);
    if (changed) {
      changes_.push_back({false, triple});
    }
    return changed;
  }

  auto Scan(const TriplePattern& pattern) -> std::unique_ptr<TripleIterator> override {
    return inner_->Scan(pattern);
  }

  auto Changes() const -> const std::vector<Change>& { return changes_; }

  void Undo() {
    for (auto it = changes_.rbegin(); it != changes_.rend(); ++it) {
      if (it->is_insert) {
        inner_->Delete(it->triple);
      } else {
        inner_->Insert(it->triple);
      }
    }
    changes_.clear();
  }

 private:
  TripleStore* inner_;
  std::vector<Change> changes_;
};

}  // namespace ontodb
