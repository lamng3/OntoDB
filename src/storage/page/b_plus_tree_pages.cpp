#include <cstring>
#include <stdexcept>
#include <string>

#include "storage/page/b_plus_tree_header_page.h"
#include "storage/page/b_plus_tree_internal_page.h"
#include "storage/page/b_plus_tree_leaf_page.h"
#include "storage/page/b_plus_tree_page.h"

namespace ontodb {

namespace {

constexpr size_t kTypeOffset = kPagePayloadOffset;  // 8
constexpr size_t kSizeOffset = kTypeOffset + 4;     // 12
constexpr size_t kMaxSizeOffset = kSizeOffset + 4;  // 16
static_assert(kMaxSizeOffset + 4 == BPlusTreePage::kHeaderSize);

constexpr size_t kLeafNextOffset = BPlusTreePage::kHeaderSize;  // 20
constexpr size_t kLeafKeysOffset = kLeafNextOffset + 4;         // 24

constexpr size_t kSlotsOffset = BPlusTreePage::kHeaderSize;  // 20
constexpr size_t kSlotSize = TripleKey::SIZE + sizeof(page_id_t);

constexpr size_t kRootOffset = kPagePayloadOffset;

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

void CheckIndex(int index, int capacity, const char* what) {
  if (index < 0 || index >= capacity) {
    throw std::out_of_range(std::string(what) + " index " + std::to_string(index) +
                            " out of range [0, " + std::to_string(capacity) + ")");
  }
}

}  // namespace

BPlusTreePage::BPlusTreePage(Page* page) : page_(page) {}

void BPlusTreePage::Init(IndexPageType type, int max_size) {
  SetPageType(type);
  SetSize(0);
  SetMaxSize(max_size);
}
auto BPlusTreePage::GetPageType() const -> IndexPageType {
  return static_cast<IndexPageType>(Load<uint32_t>(page_->GetData() + kTypeOffset));
}
void BPlusTreePage::SetPageType(IndexPageType type) {
  Store<uint32_t>(page_->GetData() + kTypeOffset, static_cast<uint32_t>(type));
}
auto BPlusTreePage::IsLeaf() const -> bool {
  return GetPageType() == IndexPageType::kLeaf;
}
auto BPlusTreePage::GetSize() const -> int {
  return Load<int32_t>(page_->GetData() + kSizeOffset);
}
void BPlusTreePage::SetSize(int size) {
  Store<int32_t>(page_->GetData() + kSizeOffset, size);
}
auto BPlusTreePage::GetMaxSize() const -> int {
  return Load<int32_t>(page_->GetData() + kMaxSizeOffset);
}
void BPlusTreePage::SetMaxSize(int max_size) {
  Store<int32_t>(page_->GetData() + kMaxSizeOffset, max_size);
}
auto BPlusTreePage::GetLSN() const -> lsn_t {
  return page_->GetLSN();
}
void BPlusTreePage::SetLSN(lsn_t lsn) {
  page_->SetLSN(lsn);
}

BPlusTreeHeaderPage::BPlusTreeHeaderPage(Page* page) : page_(page) {}
void BPlusTreeHeaderPage::Init() {
  SetRootPageId(INVALID_PAGE_ID);
}
auto BPlusTreeHeaderPage::GetRootPageId() const -> page_id_t {
  return Load<page_id_t>(page_->GetData() + kRootOffset);
}
void BPlusTreeHeaderPage::SetRootPageId(page_id_t root) {
  Store<page_id_t>(page_->GetData() + kRootOffset, root);
}

BPlusTreeInternalPage::BPlusTreeInternalPage(Page* page) : BPlusTreePage(page) {}
auto BPlusTreeInternalPage::MaxSize() -> int {
  return static_cast<int>((PAGE_SIZE - kSlotsOffset) / kSlotSize) - 1;
}
void BPlusTreeInternalPage::Init(int max_size) {
  BPlusTreePage::Init(IndexPageType::kInternal, max_size);
}
auto BPlusTreeInternalPage::KeyAt(int index) const -> TripleKey {
  CheckIndex(index, MaxSize() + 1, "internal key");
  TripleKey key;
  std::memcpy(key.Data(), GetPage()->GetData() + kSlotsOffset + index * kSlotSize, TripleKey::SIZE);
  return key;
}
void BPlusTreeInternalPage::SetKeyAt(int index, const TripleKey& key) {
  CheckIndex(index, MaxSize() + 1, "internal key");
  std::memcpy(GetPage()->GetData() + kSlotsOffset + index * kSlotSize, key.Data(), TripleKey::SIZE);
}
auto BPlusTreeInternalPage::ValueAt(int index) const -> page_id_t {
  CheckIndex(index, MaxSize() + 1, "internal child");
  return Load<page_id_t>(GetPage()->GetData() + kSlotsOffset + index * kSlotSize + TripleKey::SIZE);
}
void BPlusTreeInternalPage::SetValueAt(int index, page_id_t page_id) {
  CheckIndex(index, MaxSize() + 1, "internal child");
  Store<page_id_t>(GetPage()->GetData() + kSlotsOffset + index * kSlotSize + TripleKey::SIZE,
                   page_id);
}

BPlusTreeLeafPage::BPlusTreeLeafPage(Page* page) : BPlusTreePage(page) {}
auto BPlusTreeLeafPage::MaxSize() -> int {
  return static_cast<int>((PAGE_SIZE - kLeafKeysOffset) / TripleKey::SIZE);
}
void BPlusTreeLeafPage::Init(int max_size) {
  BPlusTreePage::Init(IndexPageType::kLeaf, max_size);
  SetNextPageId(INVALID_PAGE_ID);
}
auto BPlusTreeLeafPage::KeyAt(int index) const -> TripleKey {
  CheckIndex(index, MaxSize(), "leaf key");
  TripleKey key;
  std::memcpy(key.Data(), GetPage()->GetData() + kLeafKeysOffset + index * TripleKey::SIZE,
              TripleKey::SIZE);
  return key;
}
void BPlusTreeLeafPage::SetKeyAt(int index, const TripleKey& key) {
  CheckIndex(index, MaxSize(), "leaf key");
  std::memcpy(GetPage()->GetData() + kLeafKeysOffset + index * TripleKey::SIZE, key.Data(),
              TripleKey::SIZE);
}
auto BPlusTreeLeafPage::GetNextPageId() const -> page_id_t {
  return Load<page_id_t>(GetPage()->GetData() + kLeafNextOffset);
}
void BPlusTreeLeafPage::SetNextPageId(page_id_t next) {
  Store<page_id_t>(GetPage()->GetData() + kLeafNextOffset, next);
}

}  // namespace ontodb
