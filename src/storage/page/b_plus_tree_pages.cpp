#include "common/exception.h"
#include "storage/page/b_plus_tree_header_page.h"
#include "storage/page/b_plus_tree_internal_page.h"
#include "storage/page/b_plus_tree_leaf_page.h"
#include "storage/page/b_plus_tree_page.h"

namespace ontodb {

BPlusTreePage::BPlusTreePage(Page* page) : page_(page) {}

void BPlusTreePage::Init(IndexPageType, int) {
  throw NotImplementedException("PLAN 2.1: BPlusTreePage::Init");
}
auto BPlusTreePage::GetPageType() const -> IndexPageType {
  throw NotImplementedException("PLAN 2.1: BPlusTreePage::GetPageType");
}
void BPlusTreePage::SetPageType(IndexPageType) {
  throw NotImplementedException("PLAN 2.1: BPlusTreePage::SetPageType");
}
auto BPlusTreePage::IsLeaf() const -> bool {
  throw NotImplementedException("PLAN 2.1: BPlusTreePage::IsLeaf");
}
auto BPlusTreePage::GetSize() const -> int {
  throw NotImplementedException("PLAN 2.1: BPlusTreePage::GetSize");
}
void BPlusTreePage::SetSize(int) {
  throw NotImplementedException("PLAN 2.1: BPlusTreePage::SetSize");
}
auto BPlusTreePage::GetMaxSize() const -> int {
  throw NotImplementedException("PLAN 2.1: BPlusTreePage::GetMaxSize");
}
void BPlusTreePage::SetMaxSize(int) {
  throw NotImplementedException("PLAN 2.1: BPlusTreePage::SetMaxSize");
}
auto BPlusTreePage::GetLSN() const -> lsn_t {
  return page_->GetLSN();
}
void BPlusTreePage::SetLSN(lsn_t lsn) {
  page_->SetLSN(lsn);
}

BPlusTreeHeaderPage::BPlusTreeHeaderPage(Page* page) : page_(page) {}
void BPlusTreeHeaderPage::Init() {
  throw NotImplementedException("PLAN 2.1: BPlusTreeHeaderPage::Init");
}
auto BPlusTreeHeaderPage::GetRootPageId() const -> page_id_t {
  throw NotImplementedException("PLAN 2.1: BPlusTreeHeaderPage::GetRootPageId");
}
void BPlusTreeHeaderPage::SetRootPageId(page_id_t) {
  throw NotImplementedException("PLAN 2.1: BPlusTreeHeaderPage::SetRootPageId");
}

BPlusTreeInternalPage::BPlusTreeInternalPage(Page* page) : BPlusTreePage(page) {}
auto BPlusTreeInternalPage::MaxSize() -> int {
  throw NotImplementedException("PLAN 2.1: BPlusTreeInternalPage::MaxSize");
}
void BPlusTreeInternalPage::Init(int) {
  throw NotImplementedException("PLAN 2.1: BPlusTreeInternalPage::Init");
}
auto BPlusTreeInternalPage::KeyAt(int) const -> TripleKey {
  throw NotImplementedException("PLAN 2.1: BPlusTreeInternalPage::KeyAt");
}
void BPlusTreeInternalPage::SetKeyAt(int, const TripleKey&) {
  throw NotImplementedException("PLAN 2.1: BPlusTreeInternalPage::SetKeyAt");
}
auto BPlusTreeInternalPage::ValueAt(int) const -> page_id_t {
  throw NotImplementedException("PLAN 2.1: BPlusTreeInternalPage::ValueAt");
}
void BPlusTreeInternalPage::SetValueAt(int, page_id_t) {
  throw NotImplementedException("PLAN 2.1: BPlusTreeInternalPage::SetValueAt");
}

BPlusTreeLeafPage::BPlusTreeLeafPage(Page* page) : BPlusTreePage(page) {}
auto BPlusTreeLeafPage::MaxSize() -> int {
  throw NotImplementedException("PLAN 2.1: BPlusTreeLeafPage::MaxSize");
}
void BPlusTreeLeafPage::Init(int) {
  throw NotImplementedException("PLAN 2.1: BPlusTreeLeafPage::Init");
}
auto BPlusTreeLeafPage::KeyAt(int) const -> TripleKey {
  throw NotImplementedException("PLAN 2.1: BPlusTreeLeafPage::KeyAt");
}
void BPlusTreeLeafPage::SetKeyAt(int, const TripleKey&) {
  throw NotImplementedException("PLAN 2.1: BPlusTreeLeafPage::SetKeyAt");
}
auto BPlusTreeLeafPage::GetNextPageId() const -> page_id_t {
  throw NotImplementedException("PLAN 2.1: BPlusTreeLeafPage::GetNextPageId");
}
void BPlusTreeLeafPage::SetNextPageId(page_id_t) {
  throw NotImplementedException("PLAN 2.1: BPlusTreeLeafPage::SetNextPageId");
}

}  // namespace ontodb
