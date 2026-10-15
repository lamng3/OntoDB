#include "execution/executors/sort_executor.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace ontodb {

namespace {

constexpr size_t kNextOffset = kPagePayloadOffset;  // 8
constexpr size_t kCountOffset = kNextOffset + 4;    // 12
constexpr size_t kRowsOffset = kCountOffset + 4;    // 16

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

auto RowsPerPage(size_t width) -> size_t {
  return (PAGE_SIZE - kRowsOffset) / (width * sizeof(term_id_t));
}

void Forget(std::vector<page_id_t>* owned, page_id_t page) {
  owned->erase(std::remove(owned->begin(), owned->end(), page), owned->end());
}

// Appends rows to a new run: a chain of pages, each allocated when the one
// before it is full. Holds at most two pages pinned (while linking).
class RunWriter {
 public:
  RunWriter(BufferPoolManager* bpm, size_t width, std::vector<page_id_t>* owned)
      : bpm_(bpm), width_(width), per_page_(RowsPerPage(width)), owned_(owned) {}

  void Append(const Row& row) {
    if (!current_.IsValid() || count_ == per_page_) {
      page_id_t id = INVALID_PAGE_ID;
      WritePageGuard fresh = bpm_->NewPageGuarded(&id);
      Store<page_id_t>(fresh.GetData() + kNextOffset, INVALID_PAGE_ID);
      Store<uint32_t>(fresh.GetData() + kCountOffset, 0);
      owned_->push_back(id);
      if (current_.IsValid()) {
        Store<page_id_t>(current_.GetData() + kNextOffset, id);
      } else {
        first_ = id;
      }
      current_ = std::move(fresh);  // drops the full page
      count_ = 0;
    }
    char* data = current_.GetData();
    std::memcpy(data + kRowsOffset + count_ * width_ * sizeof(term_id_t), row.data(),
                width_ * sizeof(term_id_t));
    Store<uint32_t>(data + kCountOffset, static_cast<uint32_t>(++count_));
  }

  auto Finish() -> page_id_t {
    current_.Drop();
    return first_;
  }

 private:
  BufferPoolManager* bpm_;
  size_t width_;
  size_t per_page_;
  std::vector<page_id_t>* owned_;
  WritePageGuard current_;
  page_id_t first_{INVALID_PAGE_ID};
  size_t count_{0};
};

}  // namespace

// Reads a run front to back, one pinned page at a time, and frees each page
// as soon as it has been read.
class SortExecutor::RunReader {
 public:
  RunReader(BufferPoolManager* bpm, page_id_t first, size_t width, std::vector<page_id_t>* owned)
      : bpm_(bpm), width_(width), owned_(owned) {
    Open(first);
  }

  auto Valid() const -> bool { return valid_; }
  auto Current() const -> const Row& { return row_; }

  void Advance() {
    index_++;
    if (index_ < count_) {
      LoadRow();
      return;
    }
    const page_id_t next = Load<page_id_t>(guard_.GetData() + kNextOffset);
    const page_id_t done = page_;
    guard_.Drop();
    bpm_->DeletePage(done);
    Forget(owned_, done);
    if (next == INVALID_PAGE_ID) {
      valid_ = false;
      return;
    }
    Open(next);
  }

 private:
  void Open(page_id_t page) {
    guard_ = bpm_->FetchPageRead(page);
    page_ = page;
    count_ = Load<uint32_t>(guard_.GetData() + kCountOffset);
    index_ = 0;
    valid_ = true;
    if (count_ == 0) {
      index_ = -1;  // an empty page: Advance moves past it
      Advance();
      return;
    }
    LoadRow();
  }

  void LoadRow() {
    row_.resize(width_);
    std::memcpy(
        row_.data(),
        guard_.GetData() + kRowsOffset + static_cast<size_t>(index_) * width_ * sizeof(term_id_t),
        width_ * sizeof(term_id_t));
  }

  BufferPoolManager* bpm_;
  size_t width_;
  std::vector<page_id_t>* owned_;
  ReadPageGuard guard_;
  page_id_t page_{INVALID_PAGE_ID};
  long count_{0};
  long index_{0};
  bool valid_{false};
  Row row_;
};

SortExecutor::SortExecutor(ExecutorContext* ctx, std::unique_ptr<AbstractExecutor> child,
                           const SortPlan* plan)
    : child_(std::move(child))
    , plan_(plan)
    , bpm_(ctx->GetBufferPool())
    , order_(ctx->GetDictionary()) {}

SortExecutor::~SortExecutor() {
  Release();
}

auto SortExecutor::Less(const Row& a, const Row& b) -> bool {
  for (const auto& key : plan_->Keys()) {
    int c = order_.Compare(a[key.slot], b[key.slot]);
    if (!key.ascending) {
      c = -c;
    }
    if (c != 0) {
      return c < 0;
    }
  }
  return false;
}

void SortExecutor::Init() {
  Release();
  rows_.clear();
  next_ = 0;
  width_ = 0;
  runs_written_ = 0;
  merge_passes_ = 0;
  child_->Init();

  const size_t budget = bpm_ == nullptr ? 0 : std::max<size_t>(4, bpm_->GetPoolSize() / 2);
  size_t capacity = std::numeric_limits<size_t>::max();
  auto sort = [this](std::vector<Row>* rows) {
    std::stable_sort(rows->begin(), rows->end(),
                     [this](const Row& a, const Row& b) { return Less(a, b); });
  };
  std::vector<page_id_t> runs;
  Row row;
  while (child_->Next(&row)) {
    if (rows_.empty() && runs.empty()) {
      width_ = row.size();
      if (budget > 0 && width_ > 0) {
        capacity = budget * RowsPerPage(width_);
      }
    }
    rows_.push_back(row);
    if (rows_.size() >= capacity) {
      sort(&rows_);
      runs.push_back(WriteRun(rows_));
      rows_.clear();
    }
  }
  if (runs.empty()) {
    sort(&rows_);
    return;
  }
  if (!rows_.empty()) {
    sort(&rows_);
    runs.push_back(WriteRun(rows_));
    rows_.clear();
  }
  runs_written_ = runs.size();
  const size_t fan_in = std::max<size_t>(2, budget - 2);
  while (runs.size() > fan_in) {
    std::vector<page_id_t> merged;
    for (size_t at = 0; at < runs.size(); at += fan_in) {
      const size_t end = std::min(runs.size(), at + fan_in);
      merged.push_back(MergeToRun(
          {runs.begin() + static_cast<long>(at), runs.begin() + static_cast<long>(end)}));
    }
    runs = std::move(merged);
    merge_passes_++;
  }
  for (const page_id_t run : runs) {
    readers_.push_back(std::make_unique<RunReader>(bpm_, run, width_, &owned_));
  }
}

auto SortExecutor::WriteRun(const std::vector<Row>& rows) -> page_id_t {
  RunWriter writer(bpm_, width_, &owned_);
  for (const auto& row : rows) {
    writer.Append(row);
  }
  return writer.Finish();
}

// Ties go to the earlier run, which holds the earlier input, so merging keeps
// the sort stable.
auto SortExecutor::Smallest() -> int {
  int best = -1;
  for (size_t i = 0; i < readers_.size(); i++) {
    if (readers_[i]->Valid() &&
        (best < 0 ||
         Less(readers_[i]->Current(), readers_[static_cast<size_t>(best)]->Current()))) {
      best = static_cast<int>(i);
    }
  }
  return best;
}

auto SortExecutor::MergeToRun(std::vector<page_id_t> runs) -> page_id_t {
  std::vector<std::unique_ptr<RunReader>> outer = std::move(readers_);
  readers_.clear();
  for (const page_id_t run : runs) {
    readers_.push_back(std::make_unique<RunReader>(bpm_, run, width_, &owned_));
  }
  RunWriter writer(bpm_, width_, &owned_);
  for (int i = Smallest(); i >= 0; i = Smallest()) {
    writer.Append(readers_[static_cast<size_t>(i)]->Current());
    readers_[static_cast<size_t>(i)]->Advance();
  }
  readers_ = std::move(outer);
  return writer.Finish();
}

auto SortExecutor::Next(Row* row) -> bool {
  if (readers_.empty()) {
    if (next_ >= rows_.size()) {
      return false;
    }
    *row = rows_[next_++];
    return true;
  }
  const int i = Smallest();
  if (i < 0) {
    return false;
  }
  *row = readers_[static_cast<size_t>(i)]->Current();
  readers_[static_cast<size_t>(i)]->Advance();
  return true;
}

// Unpins every run page (readers first), then frees what is left.
void SortExecutor::Release() {
  readers_.clear();
  for (const page_id_t page : owned_) {
    bpm_->DeletePage(page);
  }
  owned_.clear();
}

auto SortExecutor::GetOutputSchema() const -> const Schema& {
  return plan_->OutputSchema();
}

}  // namespace ontodb
