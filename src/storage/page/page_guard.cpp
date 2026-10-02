#include "storage/page/page_guard.h"

#include "common/exception.h"

namespace ontodb {

ReadPageGuard::ReadPageGuard(BufferPoolManager*, Page*) {
  throw NotImplementedException("PLAN 1.5: ReadPageGuard::ReadPageGuard");
}

ReadPageGuard::ReadPageGuard(ReadPageGuard&& other) noexcept
    : bpm_(other.bpm_), page_(other.page_) {
  other.bpm_ = nullptr;
  other.page_ = nullptr;
}

auto ReadPageGuard::operator=(ReadPageGuard&& other) noexcept -> ReadPageGuard& {
  if (this != &other) {
    Drop();
    bpm_ = other.bpm_;
    page_ = other.page_;
    other.bpm_ = nullptr;
    other.page_ = nullptr;
  }
  return *this;
}

ReadPageGuard::~ReadPageGuard() {
  Drop();
}

auto ReadPageGuard::GetData() const -> const char* {
  throw NotImplementedException("PLAN 1.5: ReadPageGuard::GetData");
}

auto ReadPageGuard::GetPageId() const -> page_id_t {
  throw NotImplementedException("PLAN 1.5: ReadPageGuard::GetPageId");
}

auto ReadPageGuard::IsValid() const -> bool {
  return page_ != nullptr;
}

void ReadPageGuard::Drop() {
  bpm_ = nullptr;
  page_ = nullptr;
}

WritePageGuard::WritePageGuard(BufferPoolManager*, Page*) {
  throw NotImplementedException("PLAN 1.5: WritePageGuard::WritePageGuard");
}

WritePageGuard::WritePageGuard(WritePageGuard&& other) noexcept
    : bpm_(other.bpm_), page_(other.page_) {
  other.bpm_ = nullptr;
  other.page_ = nullptr;
}

auto WritePageGuard::operator=(WritePageGuard&& other) noexcept -> WritePageGuard& {
  if (this != &other) {
    Drop();
    bpm_ = other.bpm_;
    page_ = other.page_;
    other.bpm_ = nullptr;
    other.page_ = nullptr;
  }
  return *this;
}

WritePageGuard::~WritePageGuard() {
  Drop();
}

auto WritePageGuard::GetData() -> char* {
  throw NotImplementedException("PLAN 1.5: WritePageGuard::GetData");
}

auto WritePageGuard::GetPageId() const -> page_id_t {
  throw NotImplementedException("PLAN 1.5: WritePageGuard::GetPageId");
}

auto WritePageGuard::IsValid() const -> bool {
  return page_ != nullptr;
}

void WritePageGuard::Drop() {
  bpm_ = nullptr;
  page_ = nullptr;
}

}  // namespace ontodb
