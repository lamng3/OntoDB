#pragma once

#include "common/types.h"

namespace ontodb {

class BufferPoolManager;
class Page;

// RAII pin. Moving a guard transfers the pin; the source becomes empty.
// Drop and the destructor unpin exactly once. An empty guard (default
// constructed, moved-from, or dropped) has IsValid() == false and must not
// be dereferenced. Copying is not allowed.
class ReadPageGuard {
 public:
  ReadPageGuard() = default;
  ReadPageGuard(BufferPoolManager* bpm, Page* page);
  ReadPageGuard(const ReadPageGuard&) = delete;
  auto operator=(const ReadPageGuard&) -> ReadPageGuard& = delete;
  ReadPageGuard(ReadPageGuard&& other) noexcept;
  auto operator=(ReadPageGuard&& other) noexcept -> ReadPageGuard&;
  ~ReadPageGuard();

  auto GetData() const -> const char*;
  auto GetPageId() const -> page_id_t;
  auto IsValid() const -> bool;
  void Drop();

 private:
  BufferPoolManager* bpm_{nullptr};
  Page* page_{nullptr};
};

// Same lifetime rules as ReadPageGuard. GetData is mutable. While a write
// guard is held, no other read or write guard may be held on the same page.
class WritePageGuard {
 public:
  WritePageGuard() = default;
  WritePageGuard(BufferPoolManager* bpm, Page* page);
  WritePageGuard(const WritePageGuard&) = delete;
  auto operator=(const WritePageGuard&) -> WritePageGuard& = delete;
  WritePageGuard(WritePageGuard&& other) noexcept;
  auto operator=(WritePageGuard&& other) noexcept -> WritePageGuard&;
  ~WritePageGuard();

  auto GetData() -> char*;
  auto GetPageId() const -> page_id_t;
  auto IsValid() const -> bool;
  void Drop();

 private:
  BufferPoolManager* bpm_{nullptr};
  Page* page_{nullptr};
};

}  // namespace ontodb
