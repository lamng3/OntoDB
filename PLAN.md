# ontodb

Build a triple store one sitting at a time. Each item names the files, what they must do, and the command that checks them. The public signatures in the headers are the contract. You may add private members. If you must change a public signature, write the change down here.

A stub throws `NotImplementedException("PLAN x.y: Class::Method")`. When your item passes, delete the `DISABLED_` prefix on its tests so they run in CI. `scripts/status` counts an item only after that prefix is gone and the tests pass. Orientation items 0.1 and 0.3 are already enabled and do not count toward the percentage.

Reading, used throughout: Alex Petrov, *Database Internals*, chapters 2–6; Mohan et al., ARIES; Neumann and Weikum, RDF-3X; Weiss et al., Hexastore; the SPARQL 1.1 Query Language.

## Phase 0 — Orientation

### 0.1 Codespaces and the shell

**Files:** `.devcontainer/`, `tools/shell/`, `data/tiny.ttl`, `test/queries/`

**Depends on:** nothing

**Build:** Open a Codespace. Load `data/tiny.ttl` and `data/pizza.ttl`. Run the queries already in `test/queries/`. Add three queries of your own to a `.test` file: one join, one `FILTER`, one `INSERT DATA` followed by a read that sees the new triple.

**Check:** `scripts/check 0.1`

**Done when:** `scripts/check 0.1` passes in a fresh container with no manual installs.

**Hints:** The shell reads a query until braces and quotes balance. A `#` inside `<...>` is part of the IRI.

**Read:** `docs/architecture.md`

### 0.2 TripleKey

**Files:** `src/include/storage/index/triple_key.h`, `src/storage/index/triple_key.cpp`

**Depends on:** nothing

**Build:** `Encode` writes subject, predicate, object as big-endian `uint64`. `Decode` reads them back. `Compare` agrees with `memcmp` on the 24 bytes. `PrefixLower(key, n)` is the smallest key with that n-component prefix. `PrefixUpper(key, n)` is the smallest key strictly above every key with the prefix. `n` is 0, 1, 2, or 3. A scan of `[lower, upper)` is exactly the prefix.

**Check:** `scripts/check 0.2`

**Done when:** the `P0_2_TripleKey` tests pass with the `DISABLED_` prefix removed.

**Hints:** Big-endian is what makes integer order and byte order the same. Little-endian would sort 256 before 1. For `n = 0`, lower is the zero key.

**Read:** RDF-3X, the section on the order of triple permutations.

### 0.3 Trace a query

**Files:** `docs/architecture.md`, the shell, the planner

**Depends on:** 0.1

**Build:** Pick one multi-pattern query. Run `\explain`. Walk the printed tree against the files named in `docs/architecture.md` and write down, in `docs/journal/phase0.md`, the function that creates each node.

**Check:** `scripts/check 0.3`

**Done when:** `ExplainSmoke` passes and the journal paragraph exists.

**Hints:** The planner joins patterns left to right. The filter sits above the joins. Projection is last.

**Read:** `docs/architecture.md`

**Experiment:** Load `data/tiny.ttl`. Time the advisors query from `test/queries/tiny.test` with `\timing on`. Record the number.

**Questions:**

1. Why is the on-disk key big-endian rather than the machine's native order?
2. Why are terms interned to integers before a scan, instead of comparing strings in the index?
3. The nested-loop join does not rebind its inner scan. Which result is still correct, and which cost is paid?
4. `INSERT DATA` does not go through an executor yet. What would break if a later index update failed halfway?
5. Why does every page reserve a pageLSN before any logging exists?

**Journal:** `docs/journal/phase0.md`. What you ran, one measurement, one thing you would redo.

## Phase 1 — Disk and buffer

### 1.1 DiskManager

**Files:** `src/include/storage/disk/disk_manager.h`, `src/storage/disk/disk_manager.cpp`

**Depends on:** nothing

**Build:** Own the database file. `AllocatePage` ids are stable across process restart. `ReadPage` and `WritePage` move exactly `PAGE_SIZE` bytes, including the 8-byte pageLSN. A page never written reads back as zeros. `ShutDown` flushes and closes. When `stats` is non-null, count `PageRead` and `PageWrite`. Trace component `"disk"`.

**Check:** `scripts/check 1.1`

**Done when:** `P1_1_DiskManager` passes without `DISABLED_`.

**Hints:** Write the bytes you were given. Do not invent a second header.

**Read:** Petrov, chapter 2.

### 1.2 DiskScheduler

**Files:** `src/include/storage/disk/disk_scheduler.h`, `src/storage/disk/disk_scheduler.cpp`

**Depends on:** 1.1

**Build:** One background thread drains a queue of `DiskRequest`. `Schedule` returns immediately. The promise is set when the I/O finishes. The destructor finishes queued work and joins. `data` stays valid until then. Trace component `"io"`.

**Check:** `scripts/check 1.2`

**Done when:** `P1_2_DiskScheduler` passes without `DISABLED_`.

**Hints:** The promise is moved into the queue. Do not touch it on the caller thread after `Schedule`.

**Read:** Petrov, chapter 2, on moving I/O off the query thread.

### 1.3 LRUKReplacer

**Files:** `src/include/buffer/lru_k_replacer.h`, `src/buffer/lru_k_replacer.cpp`

**Depends on:** nothing

**Build:** `RecordAccess` appends a timestamp. Fewer than `k` accesses means infinite backward k-distance. `Evict` picks the evictable frame with the largest backward k-distance, and breaks ties by the older k-th access. It returns false when nothing is evictable and does not write `frame_id`. `SetEvictable(false)` keeps history. `Remove` drops history. `Size` is the evictable count. Thread-safe. Trace component `"replacer"`.

**Check:** `scripts/check 1.3`

**Done when:** `P1_3_LRUKReplacer` passes without `DISABLED_`.

**Hints:** Infinite distance is larger than every finite distance, so a frame touched once is thrown out before a frame touched `k` times.

**Read:** O'Neil, O'Neil, and Weikum, "The LRU-K Page Replacement Algorithm"; Petrov, chapter 3.

### 1.4 BufferPoolManager

**Files:** `src/include/buffer/buffer_pool_manager.h`, `src/buffer/buffer_pool_manager.cpp`

**Depends on:** 1.1, 1.3

**Build:** Thread-safe from the first version. `NewPage` allocates, pins, and returns a zeroed frame. `FetchPage` pins, reading from disk on a miss. `UnpinPage` drops one pin and marks dirty when asked. `FlushPage` writes a dirty frame only. If a log manager is installed, flush the log up to pageLSN first, then `CrashPoint::Reach("BufferPoolManager::FlushPage::before_disk_write")`, then the disk write. `DeletePage` fails when the page is pinned. `GetPinnedFrameCount` is safe to call concurrently. `BufferHit` on a memory fetch, `BufferMiss` when a frame is read from disk. Trace component `"bpm"`.

**Check:** `scripts/check 1.4`

**Done when:** `P1_4_BufferPoolManager` passes without `DISABLED_`, including the 3-frame eviction test and the concurrent fetch.

**Hints:** The replacer only sees unpinned frames. A pin count and a latch are different things; the latch arrives in 1.5.

**Read:** Petrov, chapter 3.

### 1.5 Page guards

**Files:** `src/include/storage/page/page_guard.h`, `src/storage/page/page_guard.cpp`, the guarded methods on the buffer pool

**Depends on:** 1.4

**Build:** `ReadPageGuard` and `WritePageGuard` unpin exactly once, in `Drop` or the destructor. Move transfers the pin and leaves the source empty. An empty guard is not dereferenceable. Copying is forbidden. A read guard takes a shared latch; a write guard takes an exclusive latch, held until drop. Two read guards on one page may coexist. A write guard excludes every other guard on that page.

**Check:** `scripts/check 1.5`

**Done when:** `P1_5_PageGuard` passes without `DISABLED_`, and the pinned-frame count is zero after the guards die.

**Hints:** The scaffold's `Drop` only clears pointers. Replace it. Unpinning twice is a bug the pin-count tests catch.

**Read:** Petrov, chapter 3, on pinning. A guard is one pin plus a latch, both released in the destructor.

### 1.6 Inspection

**Files:** `BufferPoolManager::DebugString`, `Database::BufferPoolDebug`, `Database::PageDebug`, `tools/shell/shell.cpp` (`\bpm`, `\page`, `\trace`)

**Depends on:** 1.4

**Build:** `\bpm` prints frames from your buffer pool. `\page <id>` prints one page. `\trace on` and `\trace off` flip the sink the components already hold, and traced events show up on the next fetch.

**Check:** `scripts/check 1.6`

**Done when:** `P1_6_Inspection` passes without `DISABLED_`. The shell test fails while those commands still say `not built yet`.

**Hints:** `Database::bpm_` is null until you construct a pool. `SwitchableTraceSink` is the object to toggle; the components should call `Event`, not print.

**Read:** `src/include/common/trace.h`, `src/include/common/io_stats.h`

**Experiment:** Scan the same query with pool size 4 and 64, and with `lru_k` 1 and 2. Record hit ratio from `\stats`.

**Questions:**

1. Why is a frame with fewer than k accesses treated as infinitely old?
2. What goes wrong if `FlushPage` writes the page before the log flush returns?
3. Why must `GetPinnedFrameCount` be safe to call from a test thread while a query runs?
4. A page guard's move constructor leaves the source empty. What bug appears if it does not?
5. Why does the scheduler thread, rather than the query thread, call `ReadPage`?

**Journal:** `docs/journal/phase1.md`

## Phase 2 — B+ tree, single-threaded

### 2.1 Page layouts

**Files:** `src/include/storage/page/b_plus_tree_page.h`, `b_plus_tree_header_page.h`, `b_plus_tree_internal_page.h`, `b_plus_tree_leaf_page.h`, `src/storage/page/b_plus_tree_pages.cpp`

**Depends on:** nothing beyond `Page`

**Build:** Document your byte layout in the header. Payload starts after the pageLSN. An internal page stores n keys and n+1 children. `KeyAt(0)` is unused. `ValueAt(0)` is the leftmost child. A leaf stores keys in increasing order and a right-sibling page id. `MaxSize()` is at least 2 and the page fits in `PAGE_SIZE`. The header page stores the root, or `INVALID_PAGE_ID` when the tree is empty. `GetLSN` / `SetLSN` are the page's pageLSN.

**Check:** `scripts/check 2.1`

**Done when:** `P2_1_PageLayout` passes without `DISABLED_`.

**Hints:** A zeroed page is not a valid header until you initialize the root to `INVALID_PAGE_ID`. Page 0 can be a real page id, so do not use 0 as the empty sentinel.

**Read:** Petrov, chapter 4; Comer, "The Ubiquitous B-Tree".

### 2.2 ToString, ToDot, invariants

**Files:** `BPlusTree::ToString`, `ToDot`, `CheckInvariants` in `src/storage/index/b_plus_tree.cpp`

**Depends on:** 2.1, 1.4

**Build:** `CheckInvariants` returns nullopt when the tree is well formed, otherwise a description of the first broken invariant. Check size, order, sibling links, and the separator rule: keys in child i-1 are strictly less than `KeyAt(i)`, and keys in child i are greater than or equal to it. `ToDot` is a Graphviz digraph. `\tree spo` and `\tree spo dot` call these.

**Check:** `scripts/check 2.2`

**Done when:** `P2_2_TreeDebug` passes without `DISABLED_`.

**Hints:** Write the checker before insert. A broken split is much easier to see as a failed invariant than as a wrong query.

**Read:** The Graphviz DOT language, enough to emit one node per page and one edge per child.

### 2.3 Point search

**Files:** `BPlusTree::IsEmpty`, `GetValue`

**Depends on:** 2.1, 2.2

**Build:** `GetValue` is a point lookup. An empty tree is empty. A missing key returns false. Leave the pinned-frame count at zero on return.

**Check:** `scripts/check 2.3`

**Done when:** `P2_3_PointSearch` passes without `DISABLED_`.

**Hints:** A freshly allocated header page is zeros. Treat that as empty only after you have defined how an empty root is stored (see 2.1).

**Read:** Petrov, chapter 4, the lookup walk from the root to a leaf.

### 2.4 Insert and split

**Files:** `BPlusTree::Insert`

**Depends on:** 2.2, 2.3

**Build:** Duplicate insert returns false. Split a full leaf and a full internal page, and keep separators consistent with the leaves. After a leaf split call `CrashPoint::Reach("BPlusTree::Insert::after_leaf_split")`. After an internal split, including the split that creates a new root, call `CrashPoint::Reach("BPlusTree::Insert::after_internal_split")`. Unpin before returning.

**Check:** `scripts/check 2.4`

**Done when:** `P2_4_Insert` passes without `DISABLED_`. The random insert matches a sorted `std::vector`. Both crash points fire.

**Hints:** Copy up, not copy down, or the opposite, but pick one and test the parent separator against the leaf. The crash point runs only after those pages agree.

**Read:** Petrov, chapter 4, splits and the separator copied to the parent.

### 2.5 Iterators

**Files:** `src/include/storage/index/index_iterator.h`, `src/storage/index/index_iterator.cpp`, `BPlusTree::Begin`

**Depends on:** 2.4

**Build:** `Begin()` walks every leaf through sibling pointers and holds one leaf guard. `Begin(key, prefix_len)` starts at the first key of that prefix and stops at `PrefixUpper`. Empty tree, empty prefix match, a prefix that crosses a leaf boundary, and a pool of 3 frames are all valid. Destroying the iterator drops its guard. `operator*` on an end iterator is undefined.

**Check:** `scripts/check 2.5`

**Done when:** `P2_5_Iterator` passes without `DISABLED_`. During the scan the pinned-frame count stays at most 1, and it is 0 after the iterator dies.

**Hints:** Drop the current leaf before pinning the sibling, or a 3-frame pool deadlocks on a tall tree. Do not hold the parent.

**Read:** Petrov, chapter 4, leaf sibling pointers.

### 2.6 Bulk load

**Files:** `BPlusTree::BulkLoad`

**Depends on:** 2.4

**Build:** Build a packed tree from keys that are already sorted and unique. Leaves are filled to your max size, then parents are built bottom-up. `CheckInvariants` passes. A later `GetValue` finds the first and last key.

**Check:** `scripts/check 2.6`

**Done when:** `P2_6_BulkLoad` passes without `DISABLED_`.

**Hints:** Bulk load is not a loop of `Insert`. One pass over the sorted array builds full leaves and links them, then one pass builds each level above.

**Read:** RDF-3X, bulk construction of the permutations.

### 2.7 Delete

**Files:** `BPlusTree::Remove`

**Depends on:** 2.4, 2.5

**Build:** Removing a missing key returns false. Merge or redistribute so every node except the root stays at least half full, and the invariants still hold. Call `CrashPoint::Reach("BPlusTree::Remove::after_redistribute")` or `CrashPoint::Reach("BPlusTree::Remove::after_merge")` when that operation has updated more than one page. A merge-only tree is fine; a redistribute-only tree is fine; the test accepts either crash point.

**Check:** `scripts/check 2.7`

**Done when:** `P2_7_Delete` passes without `DISABLED_`.

**Hints:** Update the parent separator when you redistribute. A stolen key that does not move the separator fails `CheckInvariants`.

**Read:** Petrov, chapter 4.

**Experiment:** Insert 100k keys in random order, in sorted order, and through bulk load. Record page count, fill factor, and height.

**Questions:**

1. Why does the iterator hold one leaf instead of a stack of latches, even in the single-threaded tree?
2. A root split creates a new internal page. Why is that an internal split for the crash point?
3. What invariant fails if a leaf split copies the middle key up but also leaves it in the left leaf?
4. Why is bulk load faster than sorted `Insert` even when `Insert` never splits an internal node more than once?
5. After a merge, which page id should the parent keep, and what happens to the page you freed?

**Journal:** `docs/journal/phase2.md`

## Phase 3 — RDF storage

### 3.1 Term heap

**Files:** `src/include/storage/page/term_heap_page.h`, `src/include/storage/term/term_heap.h`, `term_heap_iterator.h`, `src/storage/term/term_heap.cpp`

**Depends on:** 1.4

**Build:** A slotted page of byte strings. `Insert` returns nullopt when the record does not fit. `Get`'s view is valid only while the page is pinned. `Delete` frees the slot; a later insert may reuse it. `FreeSpace` includes the slot-directory entry. The heap chains pages. A record larger than an empty page throws `StorageException`. `Begin` / `End` visit every live record once and do not hold a pin after `operator*` returns.

**Check:** `scripts/check 3.1`

**Done when:** `P3_1_TermHeap` passes without `DISABLED_`.

**Hints:** Store the length with the bytes. An empty string is a real record.

**Read:** Petrov, chapter 3, slotted pages. The records here are byte strings.

### 3.2 Disk dictionary

**Files:** `src/include/dictionary/disk_dictionary.h`, `src/dictionary/disk_dictionary.cpp`

**Depends on:** 3.1, 3.3

**Build:** Both directions survive restart: spelling to id, and id to spelling. `Insert` of an existing spelling returns the original id. Ids are dense and start at 1. The on-disk shape is yours. `TermHeap` is available if you want it.

**Check:** `scripts/check 3.2`

**Done when:** `P3_2_DiskDictionary` passes without `DISABLED_`.

**Hints:** A hash index from spelling to id plus the heap from id to bytes is enough. Do not intern a spelling twice after restart.

**Read:** RDF-3X dictionary.

### 3.3 Catalog

**Files:** `src/include/catalog/catalog.h`, `src/catalog/catalog.cpp`

**Depends on:** 1.4

**Build:** Remember the root page id of `"spo"`, `"pos"`, and `"osp"`, and the dictionary root. A missing name returns `INVALID_PAGE_ID`. `Flush` makes the metadata durable. A later `Catalog` on the same file sees the values.

**Check:** `scripts/check 3.3`

**Done when:** `P3_3_Catalog` passes without `DISABLED_`.

**Hints:** Page 0 is a reasonable home. Initialize it before the first read so a zeroed page is not mistaken for root 0.

**Read:** A metadata page that maps a name to a root page id. Initialize it so a zeroed page is not root 0.

### 3.4 Indexed store, SPO only

**Files:** `src/include/store/indexed_store.h`, `src/store/indexed_store.cpp`, `indexed_triple_iterator.h`

**Depends on:** 2.5, 3.2, 3.3

**Build:** `Insert` and `Delete` update every index the catalog has a root for, or leave the store unchanged if the triple was already present or absent. `Scan` returns an `IndexedTripleIterator`. With only SPO, a pattern that is not a prefix of SPO may be a scan plus a filter. `\set backend indexed` switches the shell to this store.

**Check:** `scripts/check 3.4`

**Done when:** `P3_4_IndexedStore` passes without `DISABLED_`, and the differential test no longer skips.

**Hints:** Create the SPO root on first insert and record it in the catalog. The iterator must drop its leaf guard.

**Read:** Hexastore; RDF-3X, the permutation section.

### 3.5 Which permutations

**Files:** the indexed store, your notes in `docs/journal/phase3.md`

**Depends on:** 3.4

**Build:** The eight bound/unbound patterns in `P3_5_BoundPatterns` all return the right triples. Measure `?s rdf:type ?o` and `?s ?p <x>` with SPO only. Then add the permutations those numbers justify, and write down why you do not build all six.

**Check:** `scripts/check 3.5`

**Done when:** `P3_5_BoundPatterns` passes without `DISABLED_`.

**Hints:** SPO serves a bound subject. It does not serve a bound object with an unbound subject. POS or OSP is the usual second index, not all six.

**Read:** RDF-3X; Hexastore.

### 3.6 Clean shutdown

**Files:** `FlushAllPages`, `Catalog::Flush`, `DiskManager::ShutDown`, whatever your store needs to close

**Depends on:** 3.4

**Build:** After a clean close, a new process opens the same file and sees the triples and the dictionary.

**Check:** `scripts/check 3.6`

**Done when:** `P3_6_Restart` passes without `DISABLED_`.

**Hints:** Flush the pool and the catalog before closing the file. An unflushed dirty frame is a lost triple even with no crash.

**Read:** Petrov, chapter 3, on the difference between a clean close and a crash.

**Experiment:** With SPO only, time `?s rdf:type ?o` and `?s ?p <x>` on the pizza subset or on LUBM if you already generated it. Add the permutations you chose and time them again.

**Questions:**

1. Why is a string dictionary not stored inside the triple leaf?
2. A pattern with only the object bound cannot use SPO. What does the scan do, and why is the result still correct?
3. Why start ids at 1?
4. What does a zeroed catalog page look like if you forget to initialize it, and which root do you then open?
5. Why is "all six permutations" the wrong default even though each pattern would then be a prefix?

**Journal:** `docs/journal/phase3.md`

## Phase 4 — Execution

### 4.1 Rebindable scans

**Files:** `src/execution/executors/triple_scan_executor.cpp`, `nested_loop_join_executor.cpp`

**Depends on:** the memory executors that already run

**Build:** The inner scan of a join is closed and opened again with the outer row's bindings filled into the pattern. `Scan` on the store then sees those constants. Results stay the same. The test watches the pattern, not the rows, because the current nested loop is already correct.

**Check:** `scripts/check 4.1`

**Done when:** `P4_1_Rebind` passes without `DISABLED_`.

**Hints:** `Init` on the inner executor is the moment you have the outer row. Pass that row in. Do not change the store interface.

**Read:** Graefe, "Query Evaluation Techniques for Large Databases", index nested-loop join. The inner scan is reopened with the outer row bound.

### 4.2 Index nested-loop join

**Files:** `IndexNestedLoopJoinPlan`, a new executor, `ExecutorFactory`

**Depends on:** 4.1, 3.4

**Build:** For each outer row, probe the index with the bound prefix instead of scanning the inner side. `\set join inlj` selects it. Same rows as nested loop.

**Check:** `scripts/check 4.2`

**Done when:** `P4_2_IndexNLJ` passes without `DISABLED_`.

**Hints:** This is 4.1 plus a store that uses the prefix. On the memory backend the probe can still be a filtered scan.

**Read:** RDF-3X, index nested loops over a permutation prefix.

### 4.3 Hash join

**Files:** `HashJoinPlan`, a new executor, `ExecutorFactory`

**Depends on:** the existing nested loop

**Build:** Build a hash table on one side and probe with the other. Shared variables are the key. `\set join hash` selects it. Same rows as nested loop.

**Check:** `scripts/check 4.3`

**Done when:** `P4_3_HashJoin` passes without `DISABLED_`.

**Hints:** Hash `term_id_t`, not spellings. A slot that is `INVALID_TERM_ID` is not a key.

**Read:** Graefe, hash join. Hash `term_id_t`, not spellings.

### 4.4 Distinct, limit, offset

**Files:** `DistinctPlan`, `LimitPlan`, new executors, `ExecutorFactory`

**Depends on:** the scan executor

**Build:** `DISTINCT` drops duplicate output rows. `LIMIT` / `OFFSET` return that window and preserve the child order.

**Check:** `scripts/check 4.4`

**Done when:** `P4_4_DistinctLimit` passes without `DISABLED_`.

**Hints:** Distinct is on the projected row, not on the triple. Offset skips before limit counts.

**Read:** Distinct is a set of projected rows. Offset skips, then limit counts.

### 4.5 Sort

**Files:** `SortPlan`, a new executor

**Depends on:** 1.4, 4.4

**Build:** `ORDER BY` in memory for a small run. Then an external merge sort that uses the buffer pool when the run does not fit. Ascending and descending. Pinned-frame count is zero after `Next` returns false.

**Check:** `scripts/check 4.5`

**Done when:** `P4_5_Sort` passes without `DISABLED_`.

**Hints:** Compare the way `term_compare.cpp` does: numeric when both sides are numeric, otherwise the canonical spelling. Do not sort raw ids unless the ids were assigned in order, which they are not.

**Read:** Petrov, chapter 5, external sort.

### 4.6 Insert and delete executors

**Files:** `InsertPlan`, `DeletePlan`, new executors, `ExecutionEngine`

**Depends on:** 3.4

**Build:** Move `INSERT DATA` and `DELETE DATA` out of the engine's special case and into executors. Every index and the dictionary stay consistent. A duplicate insert does not allocate a second id and does not add a second triple. Delete of a missing triple deletes nothing.

**Check:** `scripts/check 4.6`

**Done when:** `P4_6_InsertDelete` passes without `DISABLED_`.

**Hints:** Write the dictionary first, then the indexes. If an index insert fails, the earlier indexes and the dictionary must not show a half-written triple.

**Read:** `docs/architecture.md`, the insert section.

**Experiment:** `\set join nlj`, then `inlj`, then `hash`, on the same multi-join query. Record `\stats` and the time.

**Questions:**

1. Why is rebinding tested by watching `Scan`'s pattern instead of by the output rows?
2. When is a hash join the wrong choice even if it is asymptotically faster?
3. External sort can spill. Why must those runs go through the buffer pool instead of `fwrite`?
4. Distinct after projection and distinct before projection disagree on which query? Give one.
5. What is the pinned-frame count after a hash join that built a table of term ids, and why is the table not a set of pins?

**Journal:** `docs/journal/phase4.md`

## Phase 5 — Planning

### 5.1 Statistics

**Files:** `src/include/stats/statistics.h`, `src/stats/statistics.cpp`

**Depends on:** a store and a dictionary

**Build:** `Collect` at load. `TripleCount`, `DistinctCount("s"|"p"|"o")`, `PredicateCount`, and `PatternCount` match the store. You may add counters. Do not remove these four.

**Check:** `scripts/check 5.1`

**Done when:** `P5_1_Statistics` passes without `DISABLED_`.

**Hints:** `PatternCount` can scan. It does not have to be clever. The estimator in 5.2 is what has to be fast.

**Read:** Selinger et al., "Access Path Selection in a Relational Database Management System", the catalog statistics.

### 5.2 Cardinality

**Files:** `src/include/optimizer/cardinality_estimator.h`, `src/optimizer/optimizer.cpp`

**Depends on:** 5.1

**Build:** Estimates are finite and never negative. A fully unbound pattern estimates `TripleCount`. Adding a bound position never increases the estimate. The formula is yours.

**Check:** `scripts/check 5.2`

**Done when:** `P5_2_Cardinality` passes without `DISABLED_`.

**Hints:** Independence is the usual lie: `|p| * |o| / |T|` for two bound positions. Check it against `PatternCount` on a few patterns and write down the worst q-error.

**Read:** RDF-3X, selectivity; any database textbook on selectivity.

### 5.3 Join order

**Files:** `src/include/optimizer/optimizer.h`, `src/optimizer/optimizer.cpp`

**Depends on:** 5.2

**Build:** Rewrite a left-deep nested-loop plan. Greedy first, then dynamic programming if you want it. The rewritten plan returns the same rows. `Config::join` of `kNestedLoop` stays a nested loop.

**Check:** `scripts/check 5.3`

**Done when:** `P5_3_JoinOrder` passes without `DISABLED_`.

**Hints:** Enumerate bushy plans only after left-deep works. The test checks rows, not which order you picked. The experiment checks the order.

**Read:** Selinger et al., "Access Path Selection in a Relational Database Management System".

### 5.4 Join algorithm

**Files:** the optimizer

**Depends on:** 5.3, 4.2, 4.3

**Build:** `Config::join` forces the algorithm when it is not `kAuto`: nested loop, hash, or index nested loop. `kAuto` may pick any. The forced hash plan contains a hash join. The forced nested-loop plan does not.

**Check:** `scripts/check 5.4`

**Done when:** `P5_4_JoinAlgorithm` passes without `DISABLED_`.

**Hints:** Swap the plan node, not the executor, in this item. The factory already knows the node type.

**Read:** Selinger et al., choosing among nested loop, index nested loop, and hash join.

### 5.5 Explain analyze

**Files:** `ExecutionEngine::ExplainAnalyze`, the shell command

**Depends on:** 5.2

**Build:** `\explain analyze` prints estimated and actual row counts for each node. The actual count is what `Next` returned.

**Check:** `scripts/check 5.5`

**Done when:** `P5_5_ExplainAnalyze` passes without `DISABLED_`.

**Hints:** Run the plan, count rows, then print. Do not estimate the actual.

**Read:** PostgreSQL `EXPLAIN ANALYZE`, as a picture of the output. Your format can be plainer.

**Experiment:** A table of q-error on the LUBM queries, before and after each change to the estimator.

**Questions:**

1. Why can a more accurate estimator still pick a slower plan?
2. `PatternCount` is exact and expensive. When is it worth calling during planning?
3. A fully unbound pattern estimates `TripleCount`. What estimate is dishonest for `?s ?p ?o` joined with itself?
4. Why does forcing `kHash` belong in the optimizer rather than in the executor?
5. Estimated 10 and actual 10,000. Which node do you look at first, and what statistic is missing?

**Journal:** `docs/journal/phase5.md`

## Phase 6 — Scale

### 6.1 Bulk load in the loader

**Files:** `src/loader/rdf_loader.cpp`, `BPlusTree::BulkLoad`

**Depends on:** 2.6, 3.4

**Build:** When the backend is indexed, sort each permutation and bulk-load it instead of inserting one triple at a time. The loaded triples match the file. Invariants hold. Pins are zero at the end.

**Check:** `scripts/check 6.1`

**Done when:** `P6_1_BulkLoader` passes without `DISABLED_`.

**Hints:** Sort the encoded keys, not the strings. Dictionary ids have to exist before the keys can be encoded, so intern first, then sort, then load.

**Read:** RDF-3X build procedure.

### 6.2 LUBM(1)

**Files:** `docs/benchmarks.md`, `data/lubm/queries/`, the indexed path

**Depends on:** 6.1, and phase 5 if you want the "after" numbers

**Build:** `scripts/fetch-data lubm` writes about 100k triples. Load them through the indexed path with a small pool. Record load time, page I/O, and per-query latency in `docs/benchmarks.md`, before and after phase 5. Queries that need inference (4, 5, 6, 11–13, and parts of the others) return a subset. That is expected. There is no reasoner.

**Check:** `scripts/check 6.2`

**Done when:** `P6_2_Lubm` passes without `DISABLED_`.

**Hints:** Query 7 in the official text is not a triple. The file in `data/lubm/queries/q07.sparql` is the repaired form. The parse test already accepts all 14.

**Read:** Guo, Pan, and Heflin, LUBM.

**Experiment:** The benchmark table is the experiment. One paragraph on which query got faster and which did not.

**Questions:**

1. Why does bulk load beat one-by-one insert on a tree that is still small?
2. A 32-frame pool and a 100k-triple SPO index. Where do the misses go?
3. Query 6 asks for every `ub:Student`. LUBM stores `GraduateStudent` and `UndergraduateStudent`. Why is the result short, and why is that not a bug in the scan?
4. Which permutation does query 1 actually use?
5. What would you measure to decide that a third permutation is worth its pages?

**Journal:** `docs/journal/phase6.md`

## Phase 7 — Concurrency

Phase 7 checks run under the `tsan` preset.

### 7.1 Latch crabbing

**Files:** `src/storage/index/b_plus_tree.cpp`

**Depends on:** 2.4, 2.7, 2.5

**Build:** Pessimistic crabbing first: latch the path, and hold parent latches only while a child might split or merge. Then optimistic: latch the leaf, and retry with the pessimistic path if the leaf splits. Concurrent insert, delete, and scan under TSan. Invariants hold. Pins are zero after the threads join.

**Check:** `scripts/check 7.1`

**Done when:** `P7_1_Crabbing` passes without `DISABLED_` under TSan.

**Hints:** The optimistic attempt that splits must release the leaf and start over. Two threads splitting the same leaf is the bug TSan and the invariant checker both catch.

**Read:** Bayer and Schkolnick, "Concurrency of Operations on B-Trees"; Graefe, "A Survey of B-Tree Locking Techniques".

### 7.2 Transactions

**Files:** `src/include/concurrency/transaction.h`, `transaction_manager.h`, `src/concurrency/transaction.cpp`

**Depends on:** a store

**Build:** `Begin` returns a growing transaction. Ids increase from 1. The younger transaction has the larger id. `Commit` releases locks and moves to committed. `Abort` undoes the write set newest-first when the log pointer is null, releases locks, and moves to aborted. A committed or aborted transaction accepts no further work. `DebugString` lists id, state, and isolation.

**Check:** `scripts/check 7.2`

**Done when:** `P7_2_Transaction` passes without `DISABLED_`.

**Hints:** The write set is temporary. Plan 8.4 replaces it. Keep the undo order newest-first or an insert-then-delete pair restores the wrong triple.

**Read:** Gray and Reuter, *Transaction Processing*, the transaction chapter.

### 7.3 Lock manager

**Files:** `src/include/concurrency/lock_manager.h`, `src/concurrency/transaction.cpp`

**Depends on:** 7.2

**Build:** Hierarchical modes `IS`, `IX`, `S`, `X`, `SIX` on the database, an index, and a key. Grant order on one resource is FIFO. `Lock` blocks until granted or the transaction is aborted. Before blocking, if `session` is non-null, `SetWaiting` with a reason like `X lock held by txn 5`, and `ClearWaiting` when the wait ends. Compatibility is the matrix in the header. An upgrade does not jump ahead of a waiter. `UnlockAll` drops every lock the transaction holds.

**Check:** `scripts/check 7.3`

**Done when:** `P7_3_LockManager` passes without `DISABLED_`.

**Hints:** Shared locks are compatible with each other. Exclusive is compatible with nothing, including the other transaction's intention locks. The waiting thread is the one inside `Lock`, so the reason must be set before you wait.

**Read:** Gray, Lorie, Putzolu, and Traiger, "Granularity of Locks and Degrees of Consistency in a Shared Data Base".

### 7.4 Strict 2PL and isolation

**Files:** the executors, `LockManager`, the shell's `\begin` / `\commit` / `\abort`

**Depends on:** 7.3, 4.1

**Build:** Strict 2PL: exclusive locks are held until commit or abort. Read uncommitted takes no shared locks. Read committed releases shared locks at the end of the statement. Repeatable read holds shared locks until commit. Serializable holds them until commit and adds the phantom mechanism from 7.6. Take `IS` or `IX` on the database and the index before `S` or `X` on a key. The specs in `test/isolation/specs/` are the contract. Wire `\begin` to the session's worker thread so two sessions can block each other.

**Check:** `scripts/check 7.4`

**Done when:** `P7_4_Isolation` passes without `DISABLED_`.

**Hints:** The harness starts a step and polls. A step that should block must still be running after a short wait. The shell already prints `session N waiting: ...` from `SessionState`.

**Read:** PostgreSQL isolation docs, as a description of the anomalies. The mechanism here is locks, not snapshots.

### 7.5 Deadlock detection

**Files:** `LockManager::StartDeadlockDetection`, `StopDeadlockDetection`

**Depends on:** 7.3

**Build:** A background thread builds the waits-for graph and aborts the youngest transaction in each cycle by throwing `TransactionAbortException` out of its `Lock` call. Younger means the larger id.

**Check:** `scripts/check 7.5`

**Done when:** `P7_5_Deadlock` passes without `DISABLED_`. Both the two-key cycle and the upgrade cycle abort session 2.

**Hints:** Aborting the younger one is a choice you can explain. Aborting a random one will fail the spec, which starts session 1 first so it has the smaller id.

**Read:** Gray and Reuter, waits-for deadlock detection.

### 7.6 Phantoms

**Files:** your serializable locking, `test/isolation/specs/phantom-rr.spec`, `phantom-ser.spec`

**Depends on:** 7.4

**Build:** Run the repeatable-read spec and watch the second scan see the insert. That failure of the isolation guarantee is the point. Then pick a fix for serializable: key-range locks, gap locks, or predicate locks. Write the choice in the header comment above the code that implements it. Under serializable the insert blocks until the reader commits.

**Check:** `scripts/check 7.6`

**Done when:** `P7_6_Phantom` passes without `DISABLED_`.

**Hints:** Repeatable read is supposed to allow the phantom. Do not "fix" that spec by holding a predicate lock in every isolation level. Only serializable gets the extra mechanism.

**Read:** Eswaran et al., "The Notions of Consistency and Predicate Locks"; next-key locks in Gray and Reuter.

**Experiment:** Two sessions, `\session`. A dirty read under `ru`, then the same schedule blocked under `rc`. Plot throughput against thread count for a mixed read/insert load, with and without optimistic crabbing.

**Questions:**

1. Why does optimistic crabbing have to restart the whole operation, not just the split?
2. Read uncommitted takes no shared locks. Which anomaly is that, in one sentence and one schedule?
3. Why are exclusive locks held until commit under every isolation level, including read uncommitted?
4. The youngest transaction is the one with the larger id. Why is "youngest" a better abort victim than "the one that noticed the cycle"?
5. A predicate lock on `?s rdf:type ub:Student` prevents a phantom. What does it cost a transaction that inserts a course?

**Journal:** `docs/journal/phase7.md`

## Phase 8 — Logging and recovery

### 8.1 Log records

**Files:** `src/include/recovery/log_record.h`, `src/recovery/recovery.cpp`

**Depends on:** nothing

**Build:** Your byte layout. `SerializeTo` writes `Size()` bytes. `Deserialize` reads one record and throws `StorageException` when the buffer is short. Types: begin, commit, abort, update, CLR, checkpoint. An update carries the triple and whether it inserted. A CLR carries `undo_next`. A checkpoint lists the active transactions. The factories already fill the fields.

**Check:** `scripts/check 8.1`

**Done when:** `P8_1_LogRecord` passes without `DISABLED_`.

**Hints:** Put the length in the record so a torn tail can be detected. `Size()` on the deserialized record must equal the number of bytes consumed.

**Read:** ARIES, section on log records; Petrov, chapter 5.

### 8.2 Log manager

**Files:** `src/include/recovery/log_manager.h`, `src/recovery/recovery.cpp`

**Depends on:** 8.1

**Build:** An in-memory buffer and a background flush thread. `Append` assigns an LSN and returns it. `Flush(lsn)` blocks until every record up to that LSN is durable. Commit calls `Flush` with force. Group commit: commits that arrive while a flush is running share that flush, so `GetFlushCount` grows slower than the number of commits. `StartFlushThread` starts the thread. `StopFlushThread` flushes the rest and joins. Each physical flush calls `stats->LogFlush()` when stats is non-null. `DebugTail` returns the last n records, decoded, oldest first. `\log` prints that.

**Check:** `scripts/check 8.2`

**Done when:** `P8_2_LogManager` passes without `DISABLED_`.

**Hints:** The group-commit test starts the threads together. If every commit flushes alone, the count will not be smaller.

**Read:** Mohan et al., ARIES, the log and group commit; Petrov, chapter 5.

### 8.3 WAL

**Files:** the buffer pool flush path, `Database::Crash`

**Depends on:** 1.4, 8.2

**Build:** Every update sets pageLSN. The buffer pool flushes the log up to pageLSN before writing a dirty page, and reaches `BufferPoolManager::FlushPage::before_disk_write` in between. `\crash` exits without flushing.

**Check:** `scripts/check 8.3`

**Done when:** `P8_3_Wal` passes without `DISABLED_`.

**Hints:** Run `scripts/crashtest` once with the log flush commented out and read the failures. Then put it back. The test expects the new bytes to be absent from the file when the crash point fires.

**Read:** ARIES, the WAL rule; Petrov, chapter 5.

### 8.4 Abort through the log

**Files:** `TransactionManager::Abort`, CLR records

**Depends on:** 7.2, 8.2

**Build:** When a log manager was passed in, abort undoes by the log, newest first, and writes a CLR for each undone triple. The write-set path remains for a null log. After abort the triple is gone and the log contains a CLR.

**Check:** `scripts/check 8.4`

**Done when:** `P8_4_Undo` passes without `DISABLED_`.

**Hints:** A CLR's `undo_next` is the previous LSN of the record you just undid. Undo stops when it hits a CLR's `undo_next` of invalid, or the begin record.

**Read:** ARIES, compensation log records.

### 8.5 Checkpoints

**Files:** `src/include/recovery/checkpoint_manager.h`, `src/recovery/recovery.cpp`

**Depends on:** 8.2

**Build:** `Checkpoint` writes a checkpoint record and remembers its LSN. Blocking or fuzzy is your choice; say which in the comment above `Checkpoint`. After it returns, `LastCheckpointLsn` is that record and the record is durable. `\checkpoint` calls this.

**Check:** `scripts/check 8.5`

**Done when:** `P8_5_Checkpoint` passes without `DISABLED_`.

**Hints:** A blocking checkpoint is enough. A fuzzy one needs the dirty-page table and the transaction table in the record. Do not start with fuzzy.

**Read:** ARIES, checkpoints.

### 8.6 Recovery

**Files:** `src/include/recovery/log_recovery.h`, `src/recovery/recovery.cpp`

**Depends on:** 8.3, 8.4, 8.5, 2.4

**Build:** Analysis finds winners, losers, and the redo LSN. Redo repeats every page update whose LSN is greater than the pageLSN, including CLRs. Undo walks losers newest-first and writes a CLR for each logical triple undo. A B+ tree split or merge is not undone as a logical triple operation. Those pages come back by redo only. `Summary` is one line per pass: `analysis ...`, `redo ...`, `undo ...`. Opening a database runs this and prints the summary.

**Check:** `scripts/check 8.6`

**Done when:** `P8_6_Recovery` passes without `DISABLED_`.

**Hints:** Read about nested top actions before you design split logging. A split that is undone logically will delete a key the user committed, or resurrect one they deleted. Log the split as a redo-only nested top action, and log the logical triple insert separately.

**Read:** ARIES, nested top actions; Mohan et al., the original paper.

### 8.7 Crash test

**Files:** the whole stack, `scripts/crashtest`, `test/recovery/`

**Depends on:** 8.6

**Build:** `scripts/crashtest --seeds 200` is green. A committed insert is visible after restart. An uncommitted insert is not. The harness's self-test already proves it can see a lost commit and a surviving uncommitted write; your store has to satisfy the same oracle.

**Check:** `scripts/check 8.7`

**Done when:** `P8_7_CrashTest` passes without `DISABLED_`, and `scripts/crashtest --seeds 200` exits 0.

**Hints:** The fork uses `posix_spawn` and `SIGKILL` because a raw `fork` under ASan is a bad time. Your crash points are the places the kill is allowed to land inside a multi-page operation.

**Read:** ARIES, the three passes end to end.

**Experiment:** `\crash` in the middle of a transaction, reopen, read the recovery summary and `\log`. Measure recovery time against checkpoint interval, and commit throughput with and without group commit.

**Questions:**

1. Why is redo page-oriented and undo logical?
2. A CLR is redone. Why is it not undone?
3. Group commit improves throughput. What latency does a single client pay?
4. The crash point fires after the log flush and before the disk write. What bug remains if it fires before the log flush?
5. Why must a B+ tree split not be undone as "delete the inserted key"?

**Journal:** `docs/journal/phase8.md`

## Phase 9 — Open design

No stubs. Pick any of these, write the design in `docs/journal/phase9.md`, and build the one you can finish.

- MVCC instead of strict 2PL. Say what a reader does not wait for.
- Leaf compression: delta-encoded keys, the RDF-3X way.
- RDFS `subClassOf` materialization, and which LUBM queries it completes.
- All six permutations, with the space and the query time next to the set you chose in 3.5.
- A query workload generator.

**Experiment:** One comparison against the system you had at the end of phase 8.

**Questions:**

1. Which page of phase 8 would you rewrite first, and what did it cost you?
2. MVCC removes a lock wait. Which anomaly do you now have to define a rule for?
3. Compression changes `Compare`. What happens to prefix bounds?
4. Materializing `subClassOf` makes query 6 right. What does it do to insert?
5. If you could keep only two permutations, which two, and which query did you give up?

**Journal:** `docs/journal/phase9.md`
