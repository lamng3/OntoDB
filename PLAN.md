# ontodb

This is the feature list, not a course. Each feature is something the store will do. Slices under a feature keep their ids (`1.4`, `8.6`) because the tests and the `not built yet (PLAN x.y)` messages use them.

`scripts/check shell` runs one feature. `scripts/check 1.4` runs one slice. `scripts/status` prints the same groups. A slice is done when its tests pass and the `DISABLED_` prefix is gone. The shell and the explain smoke test already pass and are not part of the percentage.

The shell is the interface. A later query window, if we build one, sends the same text and prints the same rows. It does not get its own parser. Method comparisons (eviction, indexes, joins, recovery) are benchmarks, described in `benchmarks/`, and the writeup that cites them goes in `writeups/`.

## Runs now

- Load Turtle, N-Triples, and RDF/XML. OWL is stored as triples. There is no reasoner.
- `PREFIX`, `SELECT` of variables or `*`, a basic graph pattern (`a`, `;`, `,`), and `FILTER` with `= != < > <= >=` and `&&`.
- `DISTINCT`, `LIMIT`, `OFFSET`, and `ORDER BY` parse and appear in the plan. Running them throws until query execution has those operators.
- `INSERT DATA` and `DELETE DATA` on the memory store.
- `\explain`. `\explain analyze` is part of the optimizer.
- `OPTIONAL`, `UNION`, property paths, aggregates, and `DELETE/INSERT WHERE` fail with a line and a column.

Opening a file path prints that recovery is not built and starts an empty memory database.

## Building

| Feature | Check | What it adds |
| --- | --- | --- |
| Triple key | `scripts/check key` | 24-byte big-endian key and prefix bounds. This is the next slice. |
| Buffer pool | `scripts/check buffer` | The database file, background I/O, LRU-K, pins, guards, `\bpm`. |
| B+ tree | `scripts/check index` | The triple index: layout, search, split, scan, bulk load, delete. |
| RDF storage | `scripts/check storage` | Term heap, disk dictionary, catalog, SPO/POS/OSP. |
| Query execution | `scripts/check execution` | Rebinding, index nested loop, hash join, distinct, limit, sort, insert. |
| Optimizer | `scripts/check optimizer` | Statistics, cardinality, join order, join method, explain analyze. |
| Bulk load | `scripts/check load` | Load by sorting permutations, then LUBM(1). |
| Concurrency | `scripts/check concurrency` | Crabbing, transactions, locks, isolation, deadlock, phantoms. Uses TSan. |
| Recovery | `scripts/check recovery` | Log, group commit, WAL, undo, checkpoints, ARIES, crash test. |

## Later, on purpose

These have no stubs. Add one by writing the design next to the code that implements it, then a benchmark, then a writeup.

- Another eviction policy beside LRU-K. Same `Evict` contract, compared in `benchmarks/eviction/`.
- A different permutation set. Measured in `benchmarks/indexing/`.
- MVCC beside strict two-phase locking. Say what a reader does not wait for.
- Leaf compression, delta-encoded keys. Say what happens to prefix bounds.
- RDFS `subClassOf` materialization, and which LUBM queries it completes.
- A query window around the shell. Same engine, a place to type and read rows.

## Triple key

**0.2.** `src/include/storage/index/triple_key.h`, `src/storage/index/triple_key.cpp`. `Encode` writes subject, predicate, and object as big-endian `uint64`. `Compare` agrees with `memcmp` on the 24 bytes. `PrefixLower(key, n)` is the smallest key with that prefix. `PrefixUpper(key, n)` is the smallest key strictly above every key with the prefix. `n` is 0, 1, 2, or 3. A scan of `[lower, upper)` is the prefix. For `n = 0`, lower is the zero key. Check: `scripts/check 0.2`.

## Buffer pool

Pages are 4096 bytes. The first 8 bytes are the pageLSN. The scaffold never increments `IoStats`; the call sites are named in `src/include/common/io_stats.h`.

**1.1 Disk file.** `DiskManager` owns the file. Page ids survive restart. `ReadPage` and `WritePage` move one page. A page never written reads back as zeros. `ShutDown` flushes and closes. Trace component `"disk"`.

**1.2 Scheduler.** One background thread drains `DiskRequest`. `Schedule` returns immediately. The promise is completed on that thread. Trace component `"io"`.

**1.3 LRU-K.** Fewer than `k` accesses means infinite backward k-distance, and that frame is evicted first. `Evict` returns false when nothing is evictable and does not write the frame id. `SetEvictable(false)` keeps history. Thread-safe. Trace component `"replacer"`.

**1.4 Pool.** Thread-safe. `NewPage` pins a zeroed frame. `FetchPage` reads on a miss. `UnpinPage` drops one pin. `FlushPage` writes a dirty frame only. If a log manager is installed, flush the log up to pageLSN, reach `BufferPoolManager::FlushPage::before_disk_write`, then write the page. `GetPinnedFrameCount` is safe during a query. Trace component `"bpm"`.

**1.5 Guards.** A read guard is a shared latch and one pin. A write guard is an exclusive latch and one pin. Both unpin once, in `Drop` or the destructor. Move leaves the source empty. Copying is forbidden.

**1.6 Inspection.** `\bpm`, `\page <id>`, and `\trace on|off` use the pool and `SwitchableTraceSink`.

## B+ tree

The index is a B+ tree. Keys live in the leaves. Internal pages hold separators and child pointers. Leaves point at the right sibling. The key is a `TripleKey`.

**2.1 Layout.** Payload starts after the pageLSN. An internal page stores n keys and n+1 children. `KeyAt(0)` is unused. `ValueAt(0)` is the leftmost child. A leaf keeps keys in order and a sibling page id. `MaxSize()` is at least 2. The header stores the root, or `INVALID_PAGE_ID` when the tree is empty. Page 0 is a legal page id, so do not use 0 as the empty sentinel.

**2.2 Invariants.** `CheckInvariants` describes the first broken rule, or returns nullopt. Check size, order, sibling links, and separators: keys in child i-1 are strictly less than `KeyAt(i)`, and keys in child i are greater than or equal to it. `ToDot` is Graphviz. `\tree spo` and `\tree spo dot` call these.

**2.3 Search.** `GetValue` is a point lookup. Missing and empty return false. Pins are zero on return.

**2.4 Insert.** A duplicate returns false. After a leaf split, reach `BPlusTree::Insert::after_leaf_split`. After an internal split, including a new root, reach `BPlusTree::Insert::after_internal_split`. A random insert matches a sorted vector.

**2.5 Scan.** `Begin()` holds one leaf guard and follows sibling pointers. `Begin(key, prefix_len)` stops at `PrefixUpper`. On a 3-frame pool the pinned count stays at most 1 during the scan and is 0 after the iterator dies. Drop the current leaf before pinning the sibling.

**2.6 Bulk load.** Build a packed tree from sorted unique keys. This is not a loop of `Insert`.

**2.7 Delete.** Merge or redistribute so every node except the root stays at least half full. Reach either `BPlusTree::Remove::after_redistribute` or `BPlusTree::Remove::after_merge`. Update the parent separator when a key moves.

## RDF storage

**3.1 Term heap.** A slotted page of byte strings. `Insert` returns nullopt when the record does not fit. A record larger than an empty page throws. `Get` is valid only while the page is pinned. `Delete` frees the slot for reuse. The iterator holds no pin after `operator*` returns.

**3.2 Disk dictionary.** Spelling to id and id to spelling both survive restart. `Insert` of an existing spelling returns the original id. Ids are dense and start at 1.

**3.3 Catalog.** Remember the root of `"spo"`, `"pos"`, `"osp"`, and the dictionary. A missing name is `INVALID_PAGE_ID`. `Flush` makes it durable. Initialize the page so a zeroed file is not mistaken for root 0.

**3.4 Indexed store.** `Insert` and `Delete` update every index the catalog has a root for, or leave the store unchanged. `Scan` returns an iterator that drops its leaf guard. `\set backend indexed` selects this store. The differential test stops skipping.

**3.5 Permutations.** All eight bound patterns return the right triples. Measure before adding a tree. SPO serves a bound subject. A bound object wants OSP or POS. Building all six is not the default. Write the set you kept, and why, in `benchmarks/indexing/question.md`.

**3.6 Clean shutdown.** Flush the pool and the catalog, close the file, open it again, and see the triples and the dictionary.

## Query execution

The memory nested-loop join already returns the right rows. These slices change the cost and where the work lives.

**4.1 Rebind.** The inner scan is closed and opened again with the outer row filled into the pattern. The test watches the pattern passed to `Scan`, because the rows are already correct.

**4.2 Index nested loop.** For each outer row, probe the index with the bound prefix. `\set join inlj`. Same rows as nested loop.

**4.3 Hash join.** Hash `term_id_t`. An unbound slot is not a key. `\set join hash`. Same rows as nested loop.

**4.4 Distinct, limit, offset.** Distinct is on the projected row. Offset skips, then limit counts, and the child order is kept.

**4.5 Sort.** `ORDER BY` in memory, then external merge sort through the buffer pool when the run does not fit. Compare the way `term_compare.cpp` does, not by raw id.

**4.6 Insert and delete.** Move `INSERT DATA` and `DELETE DATA` out of the engine's special case. Write the dictionary first, then the indexes. A duplicate insert adds nothing. A failed later index must not leave a half-written triple.

## Optimizer

`Optimizer::Optimize` rewrites a left-deep nested-loop plan. The new plan returns the same rows. `Config::join` forces the algorithm when it is not `kAuto`: nested loop stays nested loop, `kHash` becomes a hash join, `kIndexNestedLoop` becomes an index nested-loop join. `kAuto` may pick any of the three.

**5.1 Statistics.** `TripleCount`, `DistinctCount("s"|"p"|"o")`, `PredicateCount`, `PatternCount`. Collected at load.

**5.2 Cardinality.** Estimates are finite and never negative. An unbound pattern estimates `TripleCount`. Binding another position never increases the estimate.

**5.3 Join order.** Rewrite the left-deep tree. Greedy is enough to start.

**5.4 Join method.** The forced hash plan contains a hash join. The forced nested-loop plan does not. Swap the plan node. The factory already reads the node type.

**5.5 Explain analyze.** `\explain analyze` prints an estimated count and the actual count of rows `Next` returned.

## Bulk load

**6.1 Loader.** On the indexed backend, intern terms, encode, sort each permutation, and `BulkLoad`. Pins are zero at the end.

**6.2 LUBM(1).** `scripts/fetch-data lubm` writes about 100k triples. Load them through the indexed path with a small pool. Queries that need inference return a subset. That is expected. Record load time, page I/O, and per-query latency in `benchmarks/indexing/results.md` when that comparison starts. Query 7 in `data/lubm/queries/q07.sparql` is the repaired form.

## Concurrency

Checked under the `tsan` preset. Latches protect pages for one tree operation. Locks protect triples for a transaction.

**7.1 Crabbing.** Pessimistic crabbing holds a parent only while the child might split or merge. Optimistic crabbing latches the leaf and restarts the whole operation if the leaf splits.

**7.2 Transactions.** `Begin` ids start at 1. The younger transaction has the larger id. With a null log, `Abort` undoes the write set newest-first. A finished transaction accepts no more work.

**7.3 Locks.** Modes `IS`, `IX`, `S`, `X`, `SIX` on the database, an index, and a key. Grant order on one resource is FIFO. `Lock` blocks until granted or the transaction aborts. Before blocking, set the session wait reason, for example `X lock held by txn 5`. Take `IS` or `IX` on the database and the index before `S` or `X` on a key. An upgrade does not jump ahead of a waiter.

**7.4 Isolation.** Strict two-phase locking: exclusive locks are held until commit or abort at every level. Read uncommitted takes no shared locks. Read committed releases shared locks at the end of the statement. Repeatable read and serializable hold them until commit. The specs in `test/isolation/specs/` are the contract. `\begin` runs on the session worker so two sessions can block.

**7.5 Deadlock.** A background thread aborts the youngest transaction in each waits-for cycle.

**7.6 Phantoms.** Repeatable read is allowed to see the inserted triple on the second scan. Serializable blocks that insert until the reader commits. Key-range locks, gap locks, or predicate locks are all acceptable. Write the choice above the code. Do not take that lock at every isolation level.

## Recovery

Steal / no-force. Redo is page-oriented. Undo of a triple is logical. A B+ tree split or merge is redone, not undone as a triple operation. Log the split as a redo-only nested top action and log the triple insert separately.

**8.1 Records.** Types: begin, commit, abort, update, CLR, checkpoint. `SerializeTo` writes `Size()` bytes. A short buffer throws. An update carries the triple and whether it inserted. A CLR carries `undo_next`. A checkpoint lists the active transactions. Put the length in the record.

**8.2 Log manager.** `Append` assigns an LSN. `Flush(lsn)` blocks until that LSN is durable. Commit forces. Commits that overlap one flush share it. `\log` prints the tail, oldest first. The log file path is the `LogManager` constructor argument.

**8.3 WAL.** Every update sets pageLSN. The pool flushes the log before the page write, with the crash point between them. `\crash` exits without flushing.

**8.4 Abort.** When a log manager was passed in, abort undoes newest-first and writes a CLR per triple. The write-set path remains for a null log. A CLR is redone and is not undone.

**8.5 Checkpoint.** `\checkpoint` writes a checkpoint record and remembers its LSN. Blocking is enough. Say which kind in the comment above `Checkpoint`. This is a recovery bookmark, not a query snapshot.

**8.6 ARIES.** On open, analysis finds winners, losers, and the redo LSN. Redo repeats every page update whose LSN is greater than the pageLSN, including CLRs. Undo walks losers newest-first. `Summary` is one line per pass: `analysis`, `redo`, `undo`.

**8.7 Crash test.** `scripts/crashtest --seeds 200` is green. A committed insert is visible after restart. An uncommitted insert is not. The harness already detects a lost commit and a surviving uncommitted write on fake stores.
