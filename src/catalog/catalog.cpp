#include "catalog/catalog.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

#include "common/exception.h"

namespace ontodb {

namespace {

constexpr uint32_t kMagic = 0x5441434F;  // "OCAT" in little-endian bytes
constexpr size_t kMagicOffset = kPagePayloadOffset;
constexpr size_t kDictionaryOffset = kMagicOffset + 4;
constexpr size_t kCountOffset = kDictionaryOffset + 4;
constexpr size_t kEntriesOffset = kCountOffset + 4;
constexpr size_t kNameSize = 8;
constexpr size_t kEntrySize = kNameSize + sizeof(page_id_t);
constexpr size_t kMaxEntries = (PAGE_SIZE - kEntriesOffset) / kEntrySize;

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

auto IsZero(const char* data) -> bool {
  return std::all_of(data, data + PAGE_SIZE, [](char c) { return c == 0; });
}

// The entry index for `name`, or the entry count if it is absent.
auto Find(const char* data, std::string_view name) -> uint32_t {
  const auto count = Load<uint32_t>(data + kCountOffset);
  for (uint32_t i = 0; i < count; i++) {
    const char* entry = data + kEntriesOffset + i * kEntrySize;
    const std::string_view stored(entry, strnlen(entry, kNameSize));
    if (stored == name) {
      return i;
    }
  }
  return count;
}

}  // namespace

Catalog::Catalog(BufferPoolManager* bpm, page_id_t catalog_page_id)
    : bpm_(bpm), catalog_page_id_(catalog_page_id) {
  bool fresh = false;
  {
    WritePageGuard guard = bpm_->FetchPageWrite(catalog_page_id_);
    const char* data = guard.View()->GetData();
    if (Load<uint32_t>(data + kMagicOffset) != kMagic) {
      if (!IsZero(data)) {
        throw StorageException("page " + std::to_string(catalog_page_id_) +
                               " is not a catalog and is not empty");
      }
      char* out = guard.GetData();
      Store<uint32_t>(out + kMagicOffset, kMagic);
      Store<page_id_t>(out + kDictionaryOffset, INVALID_PAGE_ID);
      Store<uint32_t>(out + kCountOffset, 0);
      fresh = true;
    }
  }
  if (fresh) {
    bpm_->FlushPage(catalog_page_id_);  // the file now holds the page: its id is taken
  }
}

auto Catalog::GetIndexRoot(std::string_view name) const -> page_id_t {
  if (name.size() > kNameSize) {
    return INVALID_PAGE_ID;
  }
  ReadPageGuard guard = bpm_->FetchPageRead(catalog_page_id_);
  const char* data = guard.GetData();
  const uint32_t at = Find(data, name);
  if (at == Load<uint32_t>(data + kCountOffset)) {
    return INVALID_PAGE_ID;
  }
  return Load<page_id_t>(data + kEntriesOffset + at * kEntrySize + kNameSize);
}

void Catalog::SetIndexRoot(std::string_view name, page_id_t root) {
  if (name.empty() || name.size() > kNameSize) {
    throw std::invalid_argument("catalog names are 1 to 8 bytes, got \"" + std::string(name) +
                                "\"");
  }
  WritePageGuard guard = bpm_->FetchPageWrite(catalog_page_id_);
  char* data = guard.GetData();
  const auto count = Load<uint32_t>(data + kCountOffset);
  const uint32_t at = Find(data, name);
  if (at == count) {
    if (count == kMaxEntries) {
      throw StorageException("catalog is full");
    }
    char* entry = data + kEntriesOffset + at * kEntrySize;
    std::memset(entry, 0, kNameSize);
    std::memcpy(entry, name.data(), name.size());
    Store<uint32_t>(data + kCountOffset, count + 1);
  }
  Store<page_id_t>(data + kEntriesOffset + at * kEntrySize + kNameSize, root);
}

auto Catalog::GetDictionaryRoot() const -> page_id_t {
  ReadPageGuard guard = bpm_->FetchPageRead(catalog_page_id_);
  return Load<page_id_t>(guard.GetData() + kDictionaryOffset);
}

void Catalog::SetDictionaryRoot(page_id_t page) {
  WritePageGuard guard = bpm_->FetchPageWrite(catalog_page_id_);
  Store<page_id_t>(guard.GetData() + kDictionaryOffset, page);
}

void Catalog::Flush() {
  bpm_->FlushPage(catalog_page_id_);
}

}  // namespace ontodb
