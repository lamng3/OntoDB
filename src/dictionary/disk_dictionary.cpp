#include "dictionary/disk_dictionary.h"

#include <cstring>
#include <string>
#include <vector>

#include "common/exception.h"
#include "storage/page/b_plus_tree_header_page.h"

namespace ontodb {

namespace {

constexpr uint32_t kMagic = 0x4349444F;  // "ODIC" in little-endian bytes
constexpr size_t kMagicOffset = kPagePayloadOffset;
constexpr size_t kCountOffset = kMagicOffset + 4;
constexpr size_t kHeapOffset = kCountOffset + 8;
constexpr size_t kByIdOffset = kHeapOffset + 4;
constexpr size_t kByHashOffset = kByIdOffset + 4;

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

// 64-bit FNV-1a: simple, and the same on every build and machine, which a
// hash stored on disk must be. std::hash promises neither.
auto Hash(std::string_view bytes) -> uint64_t {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (const char c : bytes) {
    h ^= static_cast<unsigned char>(c);
    h *= 0x100000001b3ULL;
  }
  return h;
}

auto NewTreeHeader(BufferPoolManager* bpm) -> page_id_t {
  page_id_t id = INVALID_PAGE_ID;
  WritePageGuard guard = bpm->NewPageGuarded(&id);
  BPlusTreeHeaderPage(guard.GetPage()).Init();
  return id;
}

}  // namespace

DiskDictionary::DiskDictionary(BufferPoolManager* bpm, Catalog* catalog, size_t cache_slots)
    : bpm_(bpm), catalog_(catalog), cache_slots_(cache_slots), cache_(cache_slots) {
  page_id_t heap_page = INVALID_PAGE_ID;
  page_id_t by_id = INVALID_PAGE_ID;
  page_id_t by_hash = INVALID_PAGE_ID;
  header_page_ = catalog_->GetDictionaryRoot();
  if (header_page_ == INVALID_PAGE_ID) {
    {
      WritePageGuard guard = bpm_->NewPageGuarded(&heap_page);  // TermHeap initializes it
    }
    by_id = NewTreeHeader(bpm_);
    by_hash = NewTreeHeader(bpm_);
    {
      WritePageGuard guard = bpm_->NewPageGuarded(&header_page_);
      char* data = guard.GetData();
      Store<uint32_t>(data + kMagicOffset, kMagic);
      Store<uint64_t>(data + kCountOffset, 0);
      Store<page_id_t>(data + kHeapOffset, heap_page);
      Store<page_id_t>(data + kByIdOffset, by_id);
      Store<page_id_t>(data + kByHashOffset, by_hash);
    }
    catalog_->SetDictionaryRoot(header_page_);
  } else {
    ReadPageGuard guard = bpm_->FetchPageRead(header_page_);
    const char* data = guard.GetData();
    if (Load<uint32_t>(data + kMagicOffset) != kMagic) {
      throw StorageException("page " + std::to_string(header_page_) + " is not a dictionary");
    }
    heap_page = Load<page_id_t>(data + kHeapOffset);
    by_id = Load<page_id_t>(data + kByIdOffset);
    by_hash = Load<page_id_t>(data + kByHashOffset);
  }
  heap_ = std::make_unique<TermHeap>(bpm_, heap_page);
  by_id_ = std::make_unique<BPlusTree>("terms", by_id, bpm_);
  by_hash_ = std::make_unique<BPlusTree>("spelling", by_hash, bpm_);
}

auto DiskDictionary::Insert(std::string_view term) -> term_id_t {
  std::lock_guard<std::mutex> guard(insert_mu_);
  if (const auto existing = LookupLocked(term)) {
    return *existing;
  }
  const term_id_t id = Size() + 1;  // Size takes no lock; mu_ is already held
  CheckAssignable(id);
  const RID rid = heap_->Insert(term);
  by_id_->Insert(TripleKey::Encode(id, static_cast<uint32_t>(rid.GetPageId()), rid.GetSlot()));
  by_hash_->Insert(TripleKey::Encode(Hash(term), id, 0));
  WritePageGuard header = bpm_->FetchPageWrite(header_page_);
  Store<uint64_t>(header.GetData() + kCountOffset, id);
  return id;
}

auto DiskDictionary::Lookup(std::string_view term) const -> std::optional<term_id_t> {
  return LookupLocked(term);
}

auto DiskDictionary::Lookup(term_id_t id) const -> std::optional<std::string> {
  const bool cached = cache_slots_ != 0 && id != 0;
  const size_t slot = cached ? id % cache_slots_ : 0;
  if (cached) {
    std::lock_guard<std::mutex> stripe(cache_mu_[slot % kCacheStripes]);
    if (!cache_.empty() && cache_[slot].id == id) {
      return cache_[slot].spelling;
    }
  }
  const auto rid = RidOf(id);
  if (!rid.has_value()) {
    return std::nullopt;
  }
  std::string spelling = heap_->Get(*rid);
  if (cached) {
    std::lock_guard<std::mutex> stripe(cache_mu_[slot % kCacheStripes]);
    cache_[slot].id = id;
    cache_[slot].spelling = spelling;
  }
  return spelling;
}

auto DiskDictionary::Size() const -> size_t {
  ReadPageGuard header = bpm_->FetchPageRead(header_page_);
  return static_cast<size_t>(Load<uint64_t>(header.GetData() + kCountOffset));
}

// Collect the candidate ids first and release the hash tree's leaf before
// reading the heap, so a lookup pins at most one page at a time.
auto DiskDictionary::LookupLocked(std::string_view term) const -> std::optional<term_id_t> {
  std::vector<term_id_t> candidates;
  for (auto it = by_hash_->Begin(TripleKey::Encode(Hash(term), 0, 0), 1); !it.IsEnd(); ++it) {
    term_id_t hash = 0;
    term_id_t id = 0;
    term_id_t unused = 0;
    (*it).Decode(&hash, &id, &unused);
    candidates.push_back(id);
  }
  for (const term_id_t id : candidates) {
    if (const auto rid = RidOf(id); rid && heap_->Get(*rid) == term) {
      return id;
    }
  }
  return std::nullopt;
}

auto DiskDictionary::RidOf(term_id_t id) const -> std::optional<RID> {
  const auto key = by_id_->FindFirst(TripleKey::Encode(id, 0, 0), 1);
  if (!key.has_value()) {
    return std::nullopt;
  }
  term_id_t found = 0;
  term_id_t page = 0;
  term_id_t slot = 0;
  key->Decode(&found, &page, &slot);
  return RID(static_cast<page_id_t>(static_cast<uint32_t>(page)), static_cast<slot_offset_t>(slot));
}

}  // namespace ontodb
