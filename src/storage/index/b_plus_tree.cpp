#include "storage/index/b_plus_tree.h"

#include "common/exception.h"

namespace ontodb {

BPlusTree::BPlusTree(std::string name, page_id_t header_page_id, BufferPoolManager* bpm,
                     TraceSink* trace)
    : name_(std::move(name)), header_page_id_(header_page_id), bpm_(bpm), trace_(trace) {}

auto BPlusTree::IsEmpty() const -> bool {
  throw NotImplementedException("PLAN 2.3: BPlusTree::IsEmpty");
}
auto BPlusTree::Insert(const TripleKey&) -> bool {
  throw NotImplementedException("PLAN 2.4: BPlusTree::Insert");
}
auto BPlusTree::Remove(const TripleKey&) -> bool {
  throw NotImplementedException("PLAN 2.7: BPlusTree::Remove");
}
auto BPlusTree::GetValue(const TripleKey&) const -> bool {
  throw NotImplementedException("PLAN 2.3: BPlusTree::GetValue");
}
auto BPlusTree::Begin() -> IndexIterator {
  throw NotImplementedException("PLAN 2.5: BPlusTree::Begin");
}
auto BPlusTree::Begin(const TripleKey&, int) -> IndexIterator {
  throw NotImplementedException("PLAN 2.5: BPlusTree::Begin(prefix)");
}
auto BPlusTree::End() -> IndexIterator {
  return IndexIterator{};
}
void BPlusTree::BulkLoad(const std::vector<TripleKey>&) {
  throw NotImplementedException("PLAN 2.6: BPlusTree::BulkLoad");
}
auto BPlusTree::ToString() const -> std::string {
  throw NotImplementedException("PLAN 2.2: BPlusTree::ToString");
}
auto BPlusTree::ToDot() const -> std::string {
  throw NotImplementedException("PLAN 2.2: BPlusTree::ToDot");
}
auto BPlusTree::CheckInvariants() const -> std::optional<std::string> {
  throw NotImplementedException("PLAN 2.2: BPlusTree::CheckInvariants");
}

}  // namespace ontodb
