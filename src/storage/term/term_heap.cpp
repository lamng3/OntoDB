#include "storage/term/term_heap.h"

#include <cstring>
#include <stdexcept>
#include <string>

#include "common/exception.h"
#include "storage/page/term_heap_page.h"

namespace ontodb {

namespace {

constexpr size_t kNextOffset = kPagePayloadOffset;   // 8
constexpr size_t kCountOffset = kNextOffset + 4;     // 12
constexpr size_t kFreeEndOffset = kCountOffset + 2;  // 14
constexpr size_t kSlotsOffset = kFreeEndOffset + 2;  // 16
constexpr size_t kSlotSize = 4;
constexpr uint16_t kFreeSlot = 0xFFFF;

template<typename T>
auto Load(const char* at) -> T {
  T value;
  std::memcpy(&value, at, sizeof(T));
  return value;
}

template<typename T>
void Store(char* at, T value) {
  std::memcpy(at, &value, sizeof(T));
}

}  // namespace

TermHeapPage::TermHeapPage(Page* page) : page_(page) {}

void TermHeapPage::Init() {
  SetNextPageId(INVALID_PAGE_ID);
  Store<uint16_t>(page_->GetData() + kCountOffset, 0);
  Store<uint16_t>(page_->GetData() + kFreeEndOffset, static_cast<uint16_t>(PAGE_SIZE));
}

auto TermHeapPage::IsInitialized() const -> bool {
  return Load<uint16_t>(page_->GetData() + kFreeEndOffset) != 0;
}

auto TermHeapPage::GetNextPageId() const -> page_id_t {
  return Load<page_id_t>(page_->GetData() + kNextOffset);
}

void TermHeapPage::SetNextPageId(page_id_t next) {
  Store<page_id_t>(page_->GetData() + kNextOffset, next);
}

auto TermHeapPage::GetSlotCount() const -> uint16_t {
  return Load<uint16_t>(page_->GetData() + kCountOffset);
}

auto TermHeapPage::FreeSpace() const -> uint16_t {
  const auto free_end = Load<uint16_t>(page_->GetData() + kFreeEndOffset);
  return static_cast<uint16_t>(free_end - (kSlotsOffset + GetSlotCount() * kSlotSize));
}

auto TermHeapPage::IsLive(slot_offset_t slot) const -> bool {
  return slot < GetSlotCount() &&
         Load<uint16_t>(page_->GetData() + kSlotsOffset + slot * kSlotSize) != kFreeSlot;
}

auto TermHeapPage::Insert(std::string_view bytes) -> std::optional<slot_offset_t> {
  char* data = page_->GetData();
  const uint16_t count = GetSlotCount();
  uint16_t slot = count;
  for (uint16_t i = 0; i < count; i++) {
    if (!IsLive(i)) {
      slot = i;
      break;
    }
  }
  const size_t need = bytes.size() + (slot == count ? kSlotSize : 0);
  if (need > FreeSpace()) {
    return std::nullopt;
  }
  const auto free_end = static_cast<uint16_t>(Load<uint16_t>(data + kFreeEndOffset) - bytes.size());
  std::memcpy(data + free_end, bytes.data(), bytes.size());
  Store<uint16_t>(data + kFreeEndOffset, free_end);
  Store<uint16_t>(data + kSlotsOffset + slot * kSlotSize, free_end);
  Store<uint16_t>(data + kSlotsOffset + slot * kSlotSize + 2, static_cast<uint16_t>(bytes.size()));
  if (slot == count) {
    Store<uint16_t>(data + kCountOffset, static_cast<uint16_t>(count + 1));
  }
  return slot;
}

auto TermHeapPage::Get(slot_offset_t slot) const -> std::string_view {
  if (!IsLive(slot)) {
    throw std::out_of_range("term heap slot " + std::to_string(slot) + " is not live");
  }
  const char* entry = page_->GetData() + kSlotsOffset + slot * kSlotSize;
  return {page_->GetData() + Load<uint16_t>(entry), Load<uint16_t>(entry + 2)};
}

// Removes the record and slides every record stored below it up by its
// length, so the freed bytes join the free gap at once. Slot numbers do not
// change; trailing free slots are dropped from the directory.
void TermHeapPage::Delete(slot_offset_t slot) {
  if (!IsLive(slot)) {
    throw std::out_of_range("term heap slot " + std::to_string(slot) + " is not live");
  }
  char* data = page_->GetData();
  char* entry = data + kSlotsOffset + slot * kSlotSize;
  const auto offset = Load<uint16_t>(entry);
  const auto length = Load<uint16_t>(entry + 2);
  const auto free_end = Load<uint16_t>(data + kFreeEndOffset);
  std::memmove(data + free_end + length, data + free_end, offset - free_end);
  Store<uint16_t>(data + kFreeEndOffset, static_cast<uint16_t>(free_end + length));
  Store<uint16_t>(entry, kFreeSlot);
  Store<uint16_t>(entry + 2, 0);
  uint16_t count = GetSlotCount();
  for (uint16_t i = 0; i < count; i++) {
    char* other = data + kSlotsOffset + i * kSlotSize;
    const auto other_offset = Load<uint16_t>(other);
    if (other_offset != kFreeSlot && other_offset < offset) {
      Store<uint16_t>(other, static_cast<uint16_t>(other_offset + length));
    }
  }
  while (count > 0 && !IsLive(static_cast<slot_offset_t>(count - 1))) {
    Store<uint16_t>(data + kCountOffset, --count);
  }
}

TermHeap::TermHeap(BufferPoolManager* bpm, page_id_t first_page)
    : bpm_(bpm), first_page_(first_page) {
  WritePageGuard guard = bpm_->FetchPageWrite(first_page_);
  if (!TermHeapPage(const_cast<Page*>(guard.View())).IsInitialized()) {
    TermHeapPage(guard.GetPage()).Init();
  }
}

auto TermHeap::Insert(std::string_view bytes) -> RID {
  if (bytes.size() > TermHeapPage::kMaxRecordSize) {
    throw StorageException("term of " + std::to_string(bytes.size()) +
                           " bytes does not fit on a page (the limit is " +
                           std::to_string(TermHeapPage::kMaxRecordSize) + ")");
  }
  const page_id_t last = LastPage();
  {
    WritePageGuard guard = bpm_->FetchPageWrite(last);
    if (TermHeapPage(const_cast<Page*>(guard.View())).FreeSpace() >= bytes.size() + 4) {
      const auto slot = TermHeapPage(guard.GetPage()).Insert(bytes);
      if (slot.has_value()) {
        return {last, *slot};
      }
    }
  }
  page_id_t fresh = INVALID_PAGE_ID;
  slot_offset_t slot = INVALID_SLOT;
  {
    WritePageGuard guard = bpm_->NewPageGuarded(&fresh);
    TermHeapPage page(guard.GetPage());
    page.Init();
    const auto inserted = page.Insert(bytes);
    if (!inserted.has_value()) {  // cannot happen: the size was checked above
      throw StorageException("term does not fit on an empty page");
    }
    slot = *inserted;
  }
  {
    WritePageGuard guard = bpm_->FetchPageWrite(last);
    TermHeapPage(guard.GetPage()).SetNextPageId(fresh);
  }
  last_page_ = fresh;
  return {fresh, slot};
}

auto TermHeap::Get(const RID& rid) const -> std::string {
  ReadPageGuard guard = bpm_->FetchPageRead(rid.GetPageId());
  const TermHeapPage page(const_cast<Page*>(guard.View()));
  if (!page.IsLive(rid.GetSlot())) {
    throw StorageException("no term at page " + std::to_string(rid.GetPageId()) + " slot " +
                           std::to_string(rid.GetSlot()));
  }
  return std::string(page.Get(rid.GetSlot()));
}

auto TermHeap::Delete(const RID& rid) -> bool {
  if (rid.GetPageId() == INVALID_PAGE_ID) {
    return false;
  }
  WritePageGuard guard = bpm_->FetchPageWrite(rid.GetPageId());
  if (!TermHeapPage(const_cast<Page*>(guard.View())).IsLive(rid.GetSlot())) {
    return false;
  }
  TermHeapPage(guard.GetPage()).Delete(rid.GetSlot());
  return true;
}

auto TermHeap::Begin() const -> TermHeapIterator {
  TermHeapIterator it;
  if (const auto first = NextLive(RID{})) {
    it.heap_ = this;
    it.rid_ = *first;
    it.end_ = false;
  }
  return it;
}

auto TermHeap::End() const -> TermHeapIterator {
  return TermHeapIterator{};
}

auto TermHeap::NextLive(const RID& from) const -> std::optional<RID> {
  page_id_t page_id = from.GetPageId() == INVALID_PAGE_ID ? first_page_ : from.GetPageId();
  int slot = from.GetPageId() == INVALID_PAGE_ID ? 0 : from.GetSlot() + 1;
  while (page_id != INVALID_PAGE_ID) {
    ReadPageGuard guard = bpm_->FetchPageRead(page_id);
    const TermHeapPage page(const_cast<Page*>(guard.View()));
    for (; slot < page.GetSlotCount(); slot++) {
      if (page.IsLive(static_cast<slot_offset_t>(slot))) {
        return RID{page_id, static_cast<slot_offset_t>(slot)};
      }
    }
    page_id = page.GetNextPageId();
    slot = 0;
  }
  return std::nullopt;
}

auto TermHeap::LastPage() -> page_id_t {
  if (last_page_ == INVALID_PAGE_ID) {
    page_id_t page_id = first_page_;
    while (true) {
      ReadPageGuard guard = bpm_->FetchPageRead(page_id);
      const page_id_t next = TermHeapPage(const_cast<Page*>(guard.View())).GetNextPageId();
      if (next == INVALID_PAGE_ID) {
        break;
      }
      page_id = next;
    }
    last_page_ = page_id;
  }
  return last_page_;
}

auto TermHeapIterator::operator*() const -> std::pair<RID, std::string> {
  return {rid_, heap_->Get(rid_)};
}

auto TermHeapIterator::operator++() -> TermHeapIterator& {
  if (!end_) {
    if (const auto next = heap_->NextLive(rid_)) {
      rid_ = *next;
    } else {
      *this = TermHeapIterator{};
    }
  }
  return *this;
}

auto TermHeapIterator::operator==(const TermHeapIterator& other) const -> bool {
  return end_ == other.end_ && rid_ == other.rid_;
}

auto TermHeapIterator::IsEnd() const -> bool {
  return end_;
}

}  // namespace ontodb
