#include "storage/term/term_heap.h"

#include "common/exception.h"
#include "storage/page/term_heap_page.h"

namespace ontodb {

TermHeapPage::TermHeapPage(Page* page) : page_(page) {}
void TermHeapPage::Init() {
  throw NotImplementedException("PLAN 3.1: TermHeapPage::Init");
}
auto TermHeapPage::Insert(std::string_view) -> std::optional<slot_offset_t> {
  throw NotImplementedException("PLAN 3.1: TermHeapPage::Insert");
}
auto TermHeapPage::Get(slot_offset_t) const -> std::string_view {
  throw NotImplementedException("PLAN 3.1: TermHeapPage::Get");
}
void TermHeapPage::Delete(slot_offset_t) {
  throw NotImplementedException("PLAN 3.1: TermHeapPage::Delete");
}
auto TermHeapPage::GetSlotCount() const -> uint16_t {
  throw NotImplementedException("PLAN 3.1: TermHeapPage::GetSlotCount");
}
auto TermHeapPage::FreeSpace() const -> uint16_t {
  throw NotImplementedException("PLAN 3.1: TermHeapPage::FreeSpace");
}

TermHeap::TermHeap(BufferPoolManager* bpm, page_id_t first_page)
    : bpm_(bpm), first_page_(first_page) {}
auto TermHeap::Insert(std::string_view) -> RID {
  throw NotImplementedException("PLAN 3.1: TermHeap::Insert");
}
auto TermHeap::Get(const RID&) const -> std::string {
  throw NotImplementedException("PLAN 3.1: TermHeap::Get");
}
auto TermHeap::Delete(const RID&) -> bool {
  throw NotImplementedException("PLAN 3.1: TermHeap::Delete");
}
auto TermHeap::Begin() const -> TermHeapIterator {
  throw NotImplementedException("PLAN 3.1: TermHeap::Begin");
}
auto TermHeap::End() const -> TermHeapIterator {
  return TermHeapIterator{};
}

auto TermHeapIterator::operator*() const -> std::pair<RID, std::string> {
  throw NotImplementedException("PLAN 3.1: TermHeapIterator::operator*");
}
auto TermHeapIterator::operator++() -> TermHeapIterator& {
  throw NotImplementedException("PLAN 3.1: TermHeapIterator::operator++");
}
auto TermHeapIterator::operator==(const TermHeapIterator& other) const -> bool {
  return end_ == other.end_ && rid_ == other.rid_;
}
auto TermHeapIterator::IsEnd() const -> bool {
  return end_;
}

}  // namespace ontodb
