#include "storage/page/page_guard.h"

#include "buffer/buffer_pool_manager.h"
#include "storage/page/page.h"

namespace ontodb {

ReadPageGuard::ReadPageGuard(BufferPoolManager* bpm, Page* page) : bpm_(bpm), page_(page) {
  page_->RLatch();
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
  return page_->GetData();
}

auto ReadPageGuard::GetPageId() const -> page_id_t {
  return page_->GetPageId();
}

auto ReadPageGuard::IsValid() const -> bool {
  return page_ != nullptr;
}

void ReadPageGuard::Drop() {
  if (page_ == nullptr) {
    return;
  }
  const page_id_t id = page_->GetPageId();
  page_->RUnlatch();
  bpm_->UnpinPage(id, false);
  bpm_ = nullptr;
  page_ = nullptr;
}

WritePageGuard::WritePageGuard(BufferPoolManager* bpm, Page* page) : bpm_(bpm), page_(page) {
  page_->WLatch();
}

WritePageGuard::WritePageGuard(WritePageGuard&& other) noexcept
    : bpm_(other.bpm_), page_(other.page_), dirty_(other.dirty_) {
  other.bpm_ = nullptr;
  other.page_ = nullptr;
  other.dirty_ = false;
}

auto WritePageGuard::operator=(WritePageGuard&& other) noexcept -> WritePageGuard& {
  if (this != &other) {
    Drop();
    bpm_ = other.bpm_;
    page_ = other.page_;
    dirty_ = other.dirty_;
    other.bpm_ = nullptr;
    other.page_ = nullptr;
    other.dirty_ = false;
  }
  return *this;
}

WritePageGuard::~WritePageGuard() {
  Drop();
}

auto WritePageGuard::GetData() -> char* {
  dirty_ = true;
  return page_->GetData();
}

auto WritePageGuard::GetPageId() const -> page_id_t {
  return page_->GetPageId();
}

auto WritePageGuard::IsValid() const -> bool {
  return page_ != nullptr;
}

void WritePageGuard::Drop() {
  if (page_ == nullptr) {
    return;
  }
  const page_id_t id = page_->GetPageId();
  page_->WUnlatch();
  bpm_->UnpinPage(id, dirty_);
  bpm_ = nullptr;
  page_ = nullptr;
  dirty_ = false;
}

}  // namespace ontodb
