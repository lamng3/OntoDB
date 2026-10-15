#include "storage/index/b_plus_tree.h"

#include <algorithm>
#include <deque>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "common/crash_point.h"
#include "common/exception.h"
#include "storage/page/b_plus_tree_header_page.h"
#include "storage/page/b_plus_tree_internal_page.h"
#include "storage/page/b_plus_tree_leaf_page.h"

namespace ontodb {

namespace {

constexpr char kAfterLeafSplit[] = "BPlusTree::Insert::after_leaf_split";
constexpr char kAfterInternalSplit[] = "BPlusTree::Insert::after_internal_split";
constexpr char kAfterRedistribute[] = "BPlusTree::Remove::after_redistribute";
constexpr char kAfterMerge[] = "BPlusTree::Remove::after_merge";

auto Less(const TripleKey& a, const TripleKey& b) -> bool {
  return a.Compare(b) < 0;
}

// Read-only wrappers over a guarded page. The wrappers only read through the
// pointer, so the const_cast does not let anyone write a read-latched page.
auto ReadNode(const ReadPageGuard& guard) -> BPlusTreePage {
  return BPlusTreePage(const_cast<Page*>(guard.View()));
}
auto ReadLeaf(const ReadPageGuard& guard) -> BPlusTreeLeafPage {
  return BPlusTreeLeafPage(const_cast<Page*>(guard.View()));
}
auto ReadInternal(const ReadPageGuard& guard) -> BPlusTreeInternalPage {
  return BPlusTreeInternalPage(const_cast<Page*>(guard.View()));
}
auto PeekNode(const WritePageGuard& guard) -> BPlusTreePage {
  return BPlusTreePage(const_cast<Page*>(guard.View()));
}

// A node's contents as plain vectors. For an internal node keys[0] is a
// placeholder, so keys and children have the same length.
auto LeafKeys(const BPlusTreeLeafPage& leaf) -> std::vector<TripleKey> {
  std::vector<TripleKey> keys;
  keys.reserve(leaf.GetSize());
  for (int i = 0; i < leaf.GetSize(); i++) {
    keys.push_back(leaf.KeyAt(i));
  }
  return keys;
}

void WriteLeafKeys(BPlusTreeLeafPage* leaf, const std::vector<TripleKey>& keys) {
  for (size_t i = 0; i < keys.size(); i++) {
    leaf->SetKeyAt(static_cast<int>(i), keys[i]);
  }
  leaf->SetSize(static_cast<int>(keys.size()));
}

struct Entries {
  std::vector<TripleKey> keys;  // keys[0] unused
  std::vector<page_id_t> children;
  auto KeyCount() const -> int { return static_cast<int>(keys.size()) - 1; }
};

auto ReadEntries(const BPlusTreeInternalPage& node) -> Entries {
  Entries e;
  for (int i = 0; i <= node.GetSize(); i++) {
    e.keys.push_back(node.KeyAt(i));
    e.children.push_back(node.ValueAt(i));
  }
  return e;
}

void WriteEntries(BPlusTreeInternalPage* node, const Entries& e) {
  for (size_t i = 0; i < e.children.size(); i++) {
    node->SetKeyAt(static_cast<int>(i), e.keys[i]);
    node->SetValueAt(static_cast<int>(i), e.children[i]);
  }
  node->SetSize(e.KeyCount());
}

// The child whose range holds `key`: the last i >= 1 with KeyAt(i) <= key,
// or child 0.
auto ChildFor(const BPlusTreeInternalPage& node, const TripleKey& key) -> page_id_t {
  int lo = 1;
  int hi = node.GetSize();
  int found = 0;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    if (node.KeyAt(mid).Compare(key) <= 0) {
      found = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return node.ValueAt(found);
}

auto KeyText(const TripleKey& key) -> std::string {
  term_id_t s = 0;
  term_id_t p = 0;
  term_id_t o = 0;
  key.Decode(&s, &p, &o);
  return "(" + std::to_string(s) + "," + std::to_string(p) + "," + std::to_string(o) + ")";
}

auto IndexOf(const std::vector<page_id_t>& children, page_id_t child) -> int {
  const auto it = std::find(children.begin(), children.end(), child);
  if (it == children.end()) {
    throw StorageException("B+ tree: page " + std::to_string(child) +
                           " is not a child of its parent");
  }
  return static_cast<int>(it - children.begin());
}

void CheckMax(int value, int page_max, const char* what) {
  if (value < 2 || value > page_max) {
    throw std::invalid_argument(std::string("B+ tree ") + what + " max size must be in [2, " +
                                std::to_string(page_max) + "], got " + std::to_string(value));
  }
}

}  // namespace

BPlusTree::BPlusTree(std::string name, page_id_t header_page_id, BufferPoolManager* bpm,
                     TraceSink* trace, int leaf_max_size, int internal_max_size)
    : name_(std::move(name))
    , header_page_id_(header_page_id)
    , bpm_(bpm)
    , trace_(trace)
    , leaf_max_(leaf_max_size > 0 ? leaf_max_size : BPlusTreeLeafPage::MaxSize())
    , internal_max_(internal_max_size > 0 ? internal_max_size : BPlusTreeInternalPage::MaxSize()) {
  CheckMax(leaf_max_, BPlusTreeLeafPage::MaxSize(), "leaf");
  CheckMax(internal_max_, BPlusTreeInternalPage::MaxSize(), "internal");
}

auto BPlusTree::IsEmpty() const -> bool {
  return GetRoot() == INVALID_PAGE_ID;
}

auto BPlusTree::GetValue(const TripleKey& key) const -> bool {
  if (IsEmpty()) {
    return false;
  }
  const page_id_t leaf_id = FindLeaf(key, nullptr);
  ReadPageGuard guard = bpm_->FetchPageRead(leaf_id);
  const auto keys = LeafKeys(ReadLeaf(guard));
  return std::binary_search(keys.begin(), keys.end(), key, Less);
}

// ---------------------------------------------------------------- insert ---

auto BPlusTree::Insert(const TripleKey& key) -> bool {
  if (IsEmpty()) {
    page_id_t root = INVALID_PAGE_ID;
    {
      WritePageGuard guard = bpm_->NewPageGuarded(&root);
      BPlusTreeLeafPage leaf(guard.GetPage());
      leaf.Init(leaf_max_);
      WriteLeafKeys(&leaf, {key});
    }
    SetRoot(root);
    Trace("new root leaf " + std::to_string(root));
    return true;
  }
  std::vector<page_id_t> path;
  const page_id_t leaf_id = FindLeaf(key, &path);
  page_id_t right_id = INVALID_PAGE_ID;
  TripleKey separator;
  {
    WritePageGuard guard = bpm_->FetchPageWrite(leaf_id);
    auto keys = LeafKeys(BPlusTreeLeafPage(const_cast<Page*>(guard.View())));
    const auto pos = std::lower_bound(keys.begin(), keys.end(), key, Less);
    if (pos != keys.end() && pos->Compare(key) == 0) {
      return false;  // duplicate: the guard never wrote, so the page stays clean
    }
    keys.insert(pos, key);
    BPlusTreeLeafPage leaf(guard.GetPage());
    if (static_cast<int>(keys.size()) <= leaf_max_) {
      WriteLeafKeys(&leaf, keys);
      return true;
    }
    // Split: the left half stays, the right half moves to a new leaf, and the
    // new leaf's first key is copied up as the separator.
    const size_t left_count = keys.size() / 2;
    WritePageGuard right_guard = bpm_->NewPageGuarded(&right_id);
    BPlusTreeLeafPage right(right_guard.GetPage());
    right.Init(leaf_max_);
    WriteLeafKeys(&right, {keys.begin() + static_cast<long>(left_count), keys.end()});
    right.SetNextPageId(leaf.GetNextPageId());
    keys.resize(left_count);
    WriteLeafKeys(&leaf, keys);
    leaf.SetNextPageId(right_id);
    separator = right.KeyAt(0);
  }
  Trace("split leaf " + std::to_string(leaf_id) + " -> " + std::to_string(right_id) + " at " +
        KeyText(separator));
  CrashPoint::Reach(kAfterLeafSplit);
  InsertIntoParent(std::move(path), leaf_id, separator, right_id);
  return true;
}

void BPlusTree::InsertIntoParent(std::vector<page_id_t> path, page_id_t left, const TripleKey& key,
                                 page_id_t right) {
  if (path.empty()) {
    page_id_t root = INVALID_PAGE_ID;
    {
      WritePageGuard guard = bpm_->NewPageGuarded(&root);
      BPlusTreeInternalPage node(guard.GetPage());
      node.Init(internal_max_);
      WriteEntries(&node, Entries{{TripleKey{}, key}, {left, right}});
    }
    SetRoot(root);
    Trace("new root " + std::to_string(root) + " over " + std::to_string(left) + ", " +
          std::to_string(right));
    CrashPoint::Reach(kAfterInternalSplit);
    return;
  }
  const page_id_t parent_id = path.back();
  path.pop_back();
  page_id_t sibling_id = INVALID_PAGE_ID;
  TripleKey push_up;
  {
    WritePageGuard guard = bpm_->FetchPageWrite(parent_id);
    BPlusTreeInternalPage parent(guard.GetPage());
    Entries e = ReadEntries(parent);
    const int at = IndexOf(e.children, left) + 1;
    e.keys.insert(e.keys.begin() + at, key);
    e.children.insert(e.children.begin() + at, right);
    if (e.KeyCount() <= internal_max_) {
      WriteEntries(&parent, e);
      return;
    }
    // Split: keys 1..m become left 1..k, the middle key k+1 moves up, and the
    // rest go right. Unlike a leaf split, the middle key leaves both halves.
    const int m = e.KeyCount();
    const int k = m / 2;
    push_up = e.keys[k + 1];
    Entries right_entries;
    right_entries.keys.emplace_back();
    right_entries.keys.insert(right_entries.keys.end(), e.keys.begin() + k + 2, e.keys.end());
    right_entries.children.assign(e.children.begin() + k + 1, e.children.end());
    e.keys.resize(k + 1);
    e.children.resize(k + 1);
    WritePageGuard sibling_guard = bpm_->NewPageGuarded(&sibling_id);
    BPlusTreeInternalPage sibling(sibling_guard.GetPage());
    sibling.Init(internal_max_);
    WriteEntries(&sibling, right_entries);
    WriteEntries(&parent, e);
  }
  Trace("split internal " + std::to_string(parent_id) + " -> " + std::to_string(sibling_id) + ", " +
        KeyText(push_up) + " moves up");
  CrashPoint::Reach(kAfterInternalSplit);
  InsertIntoParent(std::move(path), parent_id, push_up, sibling_id);
}

// ---------------------------------------------------------------- remove ---

auto BPlusTree::Remove(const TripleKey& key) -> bool {
  if (IsEmpty()) {
    return false;
  }
  std::vector<page_id_t> path;
  const page_id_t leaf_id = FindLeaf(key, &path);
  bool underflow = false;
  bool emptied_root = false;
  {
    WritePageGuard guard = bpm_->FetchPageWrite(leaf_id);
    auto keys = LeafKeys(BPlusTreeLeafPage(const_cast<Page*>(guard.View())));
    const auto pos = std::lower_bound(keys.begin(), keys.end(), key, Less);
    if (pos == keys.end() || pos->Compare(key) != 0) {
      return false;
    }
    keys.erase(pos);
    BPlusTreeLeafPage leaf(guard.GetPage());
    WriteLeafKeys(&leaf, keys);
    if (path.empty()) {
      emptied_root = keys.empty();
    } else {
      underflow = static_cast<int>(keys.size()) < leaf_max_ / 2;
    }
  }
  if (emptied_root) {
    SetRoot(INVALID_PAGE_ID);
    bpm_->DeletePage(leaf_id);
    Trace("tree is empty; deleted root leaf " + std::to_string(leaf_id));
  } else if (underflow) {
    FixUnderflow(std::move(path), leaf_id);
  }
  return true;
}

// `node_id` (not the root) has fewer keys than its minimum. Borrow from a
// sibling if the two cannot fit in one page; otherwise merge the right node
// of the pair into the left one and remove the separator from the parent,
// which may underflow in turn.
void BPlusTree::FixUnderflow(std::vector<page_id_t> path, page_id_t node_id) {
  const page_id_t parent_id = path.back();
  path.pop_back();
  page_id_t deleted = INVALID_PAGE_ID;
  bool parent_underflow = false;
  bool collapse_root = false;
  page_id_t new_root = INVALID_PAGE_ID;
  const char* point = kAfterRedistribute;
  {
    WritePageGuard parent_guard = bpm_->FetchPageWrite(parent_id);
    BPlusTreeInternalPage parent(parent_guard.GetPage());
    Entries pe = ReadEntries(parent);
    const int idx = IndexOf(pe.children, node_id);
    const bool sibling_is_left = idx > 0;
    const int right_idx = sibling_is_left ? idx : idx + 1;  // the pair's right node in the parent
    const page_id_t left_id = pe.children[right_idx - 1];
    const page_id_t right_id = pe.children[right_idx];
    WritePageGuard left_guard = bpm_->FetchPageWrite(left_id);
    WritePageGuard right_guard = bpm_->FetchPageWrite(right_id);

    if (PeekNode(left_guard).IsLeaf()) {
      BPlusTreeLeafPage left(left_guard.GetPage());
      BPlusTreeLeafPage right(right_guard.GetPage());
      auto lk = LeafKeys(left);
      auto rk = LeafKeys(right);
      if (static_cast<int>(lk.size() + rk.size()) <= leaf_max_) {
        lk.insert(lk.end(), rk.begin(), rk.end());
        WriteLeafKeys(&left, lk);
        left.SetNextPageId(right.GetNextPageId());
        pe.keys.erase(pe.keys.begin() + right_idx);
        pe.children.erase(pe.children.begin() + right_idx);
        deleted = right_id;
        point = kAfterMerge;
      } else if (sibling_is_left) {  // the right node is short: take the left's last key
        rk.insert(rk.begin(), lk.back());
        lk.pop_back();
        WriteLeafKeys(&left, lk);
        WriteLeafKeys(&right, rk);
        pe.keys[right_idx] = rk.front();
      } else {  // the left node is short: take the right's first key
        lk.push_back(rk.front());
        rk.erase(rk.begin());
        WriteLeafKeys(&left, lk);
        WriteLeafKeys(&right, rk);
        pe.keys[right_idx] = rk.front();
      }
    } else {
      BPlusTreeInternalPage left(left_guard.GetPage());
      BPlusTreeInternalPage right(right_guard.GetPage());
      Entries le = ReadEntries(left);
      Entries re = ReadEntries(right);
      const TripleKey separator = pe.keys[right_idx];
      if (le.KeyCount() + 1 + re.KeyCount() <= internal_max_) {
        // Pull the separator down between the two halves.
        le.keys.push_back(separator);
        le.keys.insert(le.keys.end(), re.keys.begin() + 1, re.keys.end());
        le.children.insert(le.children.end(), re.children.begin(), re.children.end());
        WriteEntries(&left, le);
        pe.keys.erase(pe.keys.begin() + right_idx);
        pe.children.erase(pe.children.begin() + right_idx);
        deleted = right_id;
        point = kAfterMerge;
      } else if (sibling_is_left) {  // rotate right through the parent
        re.keys.insert(re.keys.begin() + 1, separator);
        re.children.insert(re.children.begin(), le.children.back());
        pe.keys[right_idx] = le.keys.back();
        le.keys.pop_back();
        le.children.pop_back();
        WriteEntries(&left, le);
        WriteEntries(&right, re);
      } else {  // rotate left through the parent
        le.keys.push_back(separator);
        le.children.push_back(re.children.front());
        pe.keys[right_idx] = re.keys[1];
        re.keys.erase(re.keys.begin() + 1);
        re.children.erase(re.children.begin());
        WriteEntries(&left, le);
        WriteEntries(&right, re);
      }
    }
    WriteEntries(&parent, pe);
    if (path.empty()) {
      collapse_root = pe.KeyCount() == 0;
      new_root = left_id;
    } else {
      parent_underflow = pe.KeyCount() < internal_max_ / 2;
    }
  }
  if (deleted != INVALID_PAGE_ID) {
    bpm_->DeletePage(deleted);
    Trace("merged page " + std::to_string(deleted) + " into its left sibling");
  } else {
    Trace("redistributed with a sibling of page " + std::to_string(node_id));
  }
  CrashPoint::Reach(point);
  if (collapse_root) {
    SetRoot(new_root);
    bpm_->DeletePage(parent_id);
    Trace("root " + std::to_string(parent_id) + " collapsed; new root " + std::to_string(new_root));
  } else if (parent_underflow) {
    FixUnderflow(std::move(path), parent_id);
  }
}

// ------------------------------------------------------------------ scan ---

auto BPlusTree::Begin() -> IndexIterator {
  page_id_t id = GetRoot();
  if (id == INVALID_PAGE_ID) {
    return End();
  }
  while (true) {
    ReadPageGuard guard = bpm_->FetchPageRead(id);
    if (ReadNode(guard).IsLeaf()) {
      break;
    }
    id = ReadInternal(guard).ValueAt(0);
  }
  return {bpm_, id, 0};
}

auto BPlusTree::Begin(const TripleKey& key, int prefix_len) -> IndexIterator {
  const TripleKey lower = TripleKey::PrefixLower(key, prefix_len);
  const TripleKey upper = TripleKey::PrefixUpper(key, prefix_len);
  if (IsEmpty()) {
    return End();
  }
  const page_id_t leaf_id = FindLeaf(lower, nullptr);
  int index = 0;
  {
    ReadPageGuard guard = bpm_->FetchPageRead(leaf_id);
    const auto keys = LeafKeys(ReadLeaf(guard));
    index =
        static_cast<int>(std::lower_bound(keys.begin(), keys.end(), lower, Less) - keys.begin());
  }
  return {bpm_, leaf_id, index, upper};
}

auto BPlusTree::End() -> IndexIterator {
  return IndexIterator{};
}

// ------------------------------------------------------------- bulk load ---

// Fill leaves completely, left to right, then build each internal level from
// the first keys of the level below. If the last node of a level would be
// under its minimum, it and its left neighbour share their entries evenly.
void BPlusTree::BulkLoad(const std::vector<TripleKey>& keys) {
  if (!IsEmpty()) {
    throw std::logic_error("BulkLoad needs an empty tree");
  }
  for (size_t i = 1; i < keys.size(); i++) {
    if (keys[i - 1].Compare(keys[i]) >= 0) {
      throw std::invalid_argument("BulkLoad needs sorted, unique keys (position " +
                                  std::to_string(i) + ")");
    }
  }
  if (keys.empty()) {
    return;
  }
  auto chunk_sizes = [](size_t total, size_t max, size_t min) {
    std::vector<size_t> sizes(total / max, max);
    if (total % max != 0) {
      sizes.push_back(total % max);
    }
    if (sizes.size() > 1 && sizes.back() < min) {
      const size_t both = sizes[sizes.size() - 2] + sizes.back();
      sizes[sizes.size() - 2] = both - both / 2;
      sizes.back() = both / 2;
    }
    return sizes;
  };

  std::vector<std::pair<TripleKey, page_id_t>> level;  // first key and page of each node
  {
    WritePageGuard previous;
    size_t at = 0;
    for (const size_t n : chunk_sizes(keys.size(), leaf_max_, leaf_max_ / 2)) {
      page_id_t id = INVALID_PAGE_ID;
      WritePageGuard guard = bpm_->NewPageGuarded(&id);
      BPlusTreeLeafPage leaf(guard.GetPage());
      leaf.Init(leaf_max_);
      WriteLeafKeys(
          &leaf, {keys.begin() + static_cast<long>(at), keys.begin() + static_cast<long>(at + n)});
      if (previous.IsValid()) {
        BPlusTreeLeafPage(previous.GetPage()).SetNextPageId(id);
      }
      previous = std::move(guard);  // drops the leaf before it
      level.emplace_back(keys[at], id);
      at += n;
    }
  }
  while (level.size() > 1) {
    std::vector<std::pair<TripleKey, page_id_t>> above;
    size_t at = 0;
    const size_t max_children = static_cast<size_t>(internal_max_) + 1;
    const size_t min_children = static_cast<size_t>(internal_max_ / 2) + 1;
    for (const size_t n : chunk_sizes(level.size(), max_children, min_children)) {
      page_id_t id = INVALID_PAGE_ID;
      WritePageGuard guard = bpm_->NewPageGuarded(&id);
      BPlusTreeInternalPage node(guard.GetPage());
      node.Init(internal_max_);
      Entries e;
      for (size_t i = at; i < at + n; i++) {
        e.keys.push_back(i == at ? TripleKey{} : level[i].first);
        e.children.push_back(level[i].second);
      }
      WriteEntries(&node, e);
      above.emplace_back(level[at].first, id);
      at += n;
    }
    level = std::move(above);
  }
  SetRoot(level.front().second);
  Trace("bulk loaded " + std::to_string(keys.size()) + " keys; root " +
        std::to_string(level.front().second));
}

// ----------------------------------------------------------------- debug ---

auto BPlusTree::ToString() const -> std::string {
  const page_id_t root = GetRoot();
  if (root == INVALID_PAGE_ID) {
    return name_ + ": empty tree\n";
  }
  std::ostringstream out;
  out << name_ << ": root " << root << '\n';
  std::vector<page_id_t> level{root};
  for (int depth = 0; !level.empty(); depth++) {
    std::vector<page_id_t> below;
    out << "level " << depth << ':';
    for (const page_id_t id : level) {
      ReadPageGuard guard = bpm_->FetchPageRead(id);
      if (ReadNode(guard).IsLeaf()) {
        const auto keys = LeafKeys(ReadLeaf(guard));
        out << " [leaf " << id << ": " << keys.size() << " keys";
        if (!keys.empty()) {
          out << ' ' << KeyText(keys.front()) << ".." << KeyText(keys.back());
        }
        out << ']';
      } else {
        const Entries e = ReadEntries(ReadInternal(guard));
        out << " [internal " << id << ':';
        for (int i = 1; i <= e.KeyCount(); i++) {
          out << ' ' << KeyText(e.keys[i]);
        }
        out << ']';
        below.insert(below.end(), e.children.begin(), e.children.end());
      }
    }
    out << '\n';
    level = std::move(below);
  }
  return out.str();
}

auto BPlusTree::ToDot() const -> std::string {
  std::ostringstream out;
  out << "digraph " << name_ << " {\n  node [shape=record, fontname=\"monospace\"];\n";
  std::deque<page_id_t> queue;
  if (const page_id_t root = GetRoot(); root != INVALID_PAGE_ID) {
    queue.push_back(root);
  }
  while (!queue.empty()) {
    const page_id_t id = queue.front();
    queue.pop_front();
    ReadPageGuard guard = bpm_->FetchPageRead(id);
    if (ReadNode(guard).IsLeaf()) {
      const auto leaf = ReadLeaf(guard);
      const auto keys = LeafKeys(leaf);
      out << "  p" << id << " [label=\"leaf " << id;
      for (const auto& key : keys) {
        out << '|' << KeyText(key);
      }
      out << "\"];\n";
      if (leaf.GetNextPageId() != INVALID_PAGE_ID) {
        out << "  p" << id << " -> p" << leaf.GetNextPageId() << " [style=dashed];\n";
      }
    } else {
      const Entries e = ReadEntries(ReadInternal(guard));
      out << "  p" << id << " [label=\"internal " << id;
      for (int i = 1; i <= e.KeyCount(); i++) {
        out << '|' << KeyText(e.keys[i]);
      }
      out << "\"];\n";
      for (const page_id_t child : e.children) {
        out << "  p" << id << " -> p" << child << ";\n";
        queue.push_back(child);
      }
    }
  }
  out << "}\n";
  return out.str();
}

auto BPlusTree::CheckInvariants() const -> std::optional<std::string> {
  const page_id_t root = GetRoot();
  if (root == INVALID_PAGE_ID) {
    return std::nullopt;
  }
  int leaf_depth = -1;
  std::vector<std::pair<page_id_t, page_id_t>> leaves;  // page, next
  if (auto broken = CheckNode(root, std::nullopt, std::nullopt, 0, true, &leaf_depth, &leaves)) {
    return broken;
  }
  for (size_t i = 0; i < leaves.size(); i++) {
    const page_id_t expect = i + 1 < leaves.size() ? leaves[i + 1].first : INVALID_PAGE_ID;
    if (leaves[i].second != expect) {
      return "leaf " + std::to_string(leaves[i].first) + ": sibling link is " +
             std::to_string(leaves[i].second) + ", expected " + std::to_string(expect);
    }
  }
  return std::nullopt;
}

// Every key in this subtree must lie in [lower, upper). The node is read and
// released before its children are visited, so a check pins one page at a time.
auto BPlusTree::CheckNode(
    page_id_t page_id, const std::optional<TripleKey>& lower, const std::optional<TripleKey>& upper,
    int depth, bool is_root, int* leaf_depth,
    std::vector<std::pair<page_id_t, page_id_t>>* leaves) const -> std::optional<std::string> {
  const std::string where = "page " + std::to_string(page_id) + ": ";
  auto in_range = [&](const TripleKey& key) {
    return (!lower || lower->Compare(key) <= 0) && (!upper || key.Compare(*upper) < 0);
  };
  bool leaf_node = false;
  std::vector<TripleKey> keys;
  std::vector<page_id_t> children;
  page_id_t next = INVALID_PAGE_ID;
  int max_size = 0;
  {
    ReadPageGuard guard = bpm_->FetchPageRead(page_id);
    const auto node = ReadNode(guard);
    max_size = node.GetMaxSize();
    if (node.GetPageType() == BPlusTreePage::IndexPageType::kLeaf) {
      leaf_node = true;
      const auto leaf = ReadLeaf(guard);
      keys = LeafKeys(leaf);
      next = leaf.GetNextPageId();
    } else if (node.GetPageType() == BPlusTreePage::IndexPageType::kInternal) {
      const Entries e = ReadEntries(ReadInternal(guard));
      keys.assign(e.keys.begin() + 1, e.keys.end());
      children = e.children;
    } else {
      return where + "not an index page";
    }
  }
  const int size = static_cast<int>(keys.size());
  const int limit = leaf_node ? leaf_max_ : internal_max_;
  if (max_size != limit) {
    return where + "max size " + std::to_string(max_size) + ", tree expects " +
           std::to_string(limit);
  }
  if (size > limit) {
    return where + "size " + std::to_string(size) + " exceeds max " + std::to_string(limit);
  }
  const int minimum = is_root ? 1 : limit / 2;
  if (size < minimum) {
    return where + "size " + std::to_string(size) + " is below the minimum " +
           std::to_string(minimum);
  }
  for (int i = 0; i < size; i++) {
    if (i > 0 && keys[i - 1].Compare(keys[i]) >= 0) {
      return where + "keys out of order at " + std::to_string(i);
    }
    if (!in_range(keys[i])) {
      return where + "key " + KeyText(keys[i]) + " at " + std::to_string(i) +
             " is outside the range its parent's separators allow";
    }
  }
  if (leaf_node) {
    if (*leaf_depth == -1) {
      *leaf_depth = depth;
    } else if (*leaf_depth != depth) {
      return where + "leaf at depth " + std::to_string(depth) + ", others at " +
             std::to_string(*leaf_depth);
    }
    leaves->emplace_back(page_id, next);
    return std::nullopt;
  }
  for (size_t i = 0; i < children.size(); i++) {
    const std::optional<TripleKey> child_lower = i == 0 ? lower : std::optional(keys[i - 1]);
    const std::optional<TripleKey> child_upper =
        i == children.size() - 1 ? upper : std::optional(keys[i]);
    if (auto broken = CheckNode(children[i], child_lower, child_upper, depth + 1, false, leaf_depth,
                                leaves)) {
      return broken;
    }
  }
  return std::nullopt;
}

// --------------------------------------------------------------- helpers ---

auto BPlusTree::GetRoot() const -> page_id_t {
  ReadPageGuard guard = bpm_->FetchPageRead(header_page_id_);
  return BPlusTreeHeaderPage(const_cast<Page*>(guard.View())).GetRootPageId();
}

void BPlusTree::SetRoot(page_id_t root) {
  WritePageGuard guard = bpm_->FetchPageWrite(header_page_id_);
  BPlusTreeHeaderPage(guard.GetPage()).SetRootPageId(root);
}

// Descends to the leaf whose range holds `key`, recording the internal pages
// on the way. Each page is released before its child is read: the tree is
// single-threaded until latch crabbing (PLAN 7.1), and one pin at a time lets
// it run on a tiny pool.
auto BPlusTree::FindLeaf(const TripleKey& key, std::vector<page_id_t>* path) const -> page_id_t {
  page_id_t id = GetRoot();
  while (true) {
    ReadPageGuard guard = bpm_->FetchPageRead(id);
    if (ReadNode(guard).IsLeaf()) {
      return id;
    }
    if (path != nullptr) {
      path->push_back(id);
    }
    id = ChildFor(ReadInternal(guard), key);
  }
}

void BPlusTree::Trace(const std::string& message) const {
  if (trace_ != nullptr) {
    trace_->Event("btree", name_ + ": " + message);
  }
}

}  // namespace ontodb
