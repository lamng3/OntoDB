# OntoDB

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
- A durable memory database. `ontodb <dir>` keeps a snapshot and a write-ahead log in `<dir>`. Each update statement and each `\load` is one transaction, and reopening the directory replays the committed ones. `\checkpoint`, `\verify`, `\log`, and `\crash` work on it. See [Durable memory database](#durable-memory-database).
- Log records with checksums (8.1) and the log manager with group commit (8.2).
- The B+ tree (2.1 to 2.7): page layout, invariants, search, insert with splits, the prefix iterator, bulk load, and delete. Single-threaded until 7.1; nodes remember their path instead of storing parent pointers.
- The disk manager (1.1) and the I/O scheduler (1.2): allocate, read, and write pages of the database file on one background thread, the LRU-K replacer (1.3), the buffer pool (1.4), its page guards (1.5), and `\bpm`, `\page`, `\trace` (1.6); the pool flushes the log up to a page's pageLSN before writing it.
- The triple key (0.2): encode, decode, compare, and prefix bounds. The B+ tree is its first user.

Without a directory, the shell starts an empty memory database that keeps nothing. `\set backend indexed` moves it onto pages and B+ trees in a temporary file; `\set backend mem` moves it back.

## Building

| Feature | Check | What it adds |
| --- | --- | --- |
| Triple key | `scripts/check key` | 24-byte big-endian key and prefix bounds. Done. |
| Buffer pool | `scripts/check buffer` | The database file, background I/O, LRU-K, pins, guards, `\bpm`. Done. |
| B+ tree | `scripts/check index` | The triple index: layout, search, split, scan, bulk load, delete. Done. |
| RDF storage | `scripts/check storage` | Term heap, disk dictionary, catalog, SPO/POS/OSP. Done. |
| Query execution | `scripts/check execution` | Rebinding, index nested loop, hash join, distinct, limit, sort, insert. 4.1 to 4.5 are done. |
| Optimizer | `scripts/check optimizer` | Statistics, cardinality, join order, join method, explain analyze. |
| Bulk load | `scripts/check load` | Load by sorting permutations, then LUBM(1). |
| Concurrency | `scripts/check concurrency` | Crabbing, transactions, locks, isolation, deadlock, phantoms. Uses TSan. |
| Recovery | `scripts/check recovery` | Log, group commit, WAL, undo, checkpoints, ARIES, crash test. 8.1 and 8.2 are done. |
| Durable memory database | `scripts/check durable` | Done. Snapshot plus logical WAL for the memory store, recovery modes, verify. |

## Later, on purpose

These have no stubs. Add one by writing the design next to the code that implements it, then a benchmark, then a writeup.

- Another eviction policy beside LRU-K. Same `Evict` contract, compared in `benchmarks/eviction/`.
- A different permutation set. Measured in `benchmarks/indexing/`.
- MVCC beside strict two-phase locking. Say what a reader does not wait for.
- Leaf compression, delta-encoded keys. Say what happens to prefix bounds.
- RDFS `subClassOf` materialization, and which LUBM queries it completes.
- A query window around the shell. Same engine, a place to type and read rows.
- Page checksums on the disk store, as PostgreSQL's `data_checksums` and InnoDB do, once pages exist. Checked on read, and by `\verify`.
- Restore to a chosen LSN: replay the snapshot and the log up to an LSN and stop, to see the database as it was at that point.

## Triple key

**0.2.** `src/include/storage/index/triple_key.h`, `src/storage/index/triple_key.cpp`. `Encode` writes subject, predicate, and object as big-endian `uint64`. `Compare` agrees with `memcmp` on the 24 bytes. `PrefixLower(key, n)` is the smallest key with that prefix. `PrefixUpper(key, n)` is the smallest key strictly above every key with the prefix. `n` is 0, 1, 2, or 3. A scan of `[lower, upper)` is the prefix. For `n = 0`, lower is the zero key. When nothing is above the prefix (`n = 0`, or every prefix id is the maximum), upper saturates to the all-`0xFF` key, three `INVALID_TERM_ID`s, which is never stored. Done. Check: `scripts/check 0.2`.

## Buffer pool

Pages are 4096 bytes. The first 8 bytes are the pageLSN. The scaffold never increments `IoStats`; the call sites are named in `src/include/common/io_stats.h`.

**1.1 Disk file.** Done. `DiskManager` owns the file. Page ids survive restart: the file length is the allocation record, and a freed id is not reused. A write past the end grows the file; a read past the end returns zeros. `ReadPage` and `WritePage` move one page. A page never written reads back as zeros. `ShutDown` flushes and closes. Trace component `"disk"`.

**1.2 Scheduler.** Done. One background thread drains `DiskRequest`. `Schedule` returns immediately. The promise is completed on that thread. Trace component `"io"`.

**1.3 LRU-K.** Done. Fewer than `k` accesses means infinite backward k-distance, and that frame is evicted first. `Evict` returns false when nothing is evictable and does not write the frame id. `SetEvictable(false)` keeps history. Thread-safe. Trace component `"replacer"`. Time is a logical clock; among infinite distances, the earliest oldest access goes first.

**1.4 Pool.** Done. Thread-safe. `NewPage` pins a zeroed frame. `FetchPage` reads on a miss. `UnpinPage` drops one pin. `FlushPage` writes a dirty frame only. If a log manager is installed, flush the log up to pageLSN, reach `BufferPoolManager::FlushPage::before_disk_write`, then write the page. `GetPinnedFrameCount` is safe during a query. Trace component `"bpm"`. One latch covers the pool and is held across I/O. A full pool returns null. The destructor does not flush.

**1.5 Guards.** Done. A read guard is a shared latch and one pin. A write guard is an exclusive latch and one pin. Both unpin once, in `Drop` or the destructor. Move leaves the source empty. Copying is forbidden. Pin, then latch; unlatch, then unpin. A write guard dirties the page only if `GetData` was called.

**1.6 Inspection.** Done. `\bpm`, `\page <id>`, and `\trace on|off` use the pool and `SwitchableTraceSink`. With the memory backend there is no pool, and `\bpm` and `\page` say so.

## B+ tree

The index is a B+ tree. Keys live in the leaves. Internal pages hold separators and child pointers. Leaves point at the right sibling. The key is a `TripleKey`.

**2.1 Layout.** Done. Payload starts after the pageLSN. An internal page stores n keys and n+1 children. `KeyAt(0)` is unused. `ValueAt(0)` is the leftmost child. A leaf keeps keys in order and a sibling page id. `MaxSize()` is at least 2. The header stores the root, or `INVALID_PAGE_ID` when the tree is empty. Page 0 is a legal page id, so do not use 0 as the empty sentinel. The byte layout is written in the page headers: 169 keys per leaf, 144 per internal page.

**2.2 Invariants.** Done. `CheckInvariants` describes the first broken rule, or returns nullopt. Check size, order, sibling links, and separators: keys in child i-1 are strictly less than `KeyAt(i)`, and keys in child i are greater than or equal to it. `ToDot` is Graphviz. `\tree spo` and `\tree spo dot` call these.

**2.3 Search.** Done. `GetValue` is a point lookup. Missing and empty return false. Pins are zero on return.

**2.4 Insert.** Done. A duplicate returns false. After a leaf split, reach `BPlusTree::Insert::after_leaf_split`. After an internal split, including a new root, reach `BPlusTree::Insert::after_internal_split`. A random insert matches a sorted vector.

**2.5 Scan.** Done. `Begin()` holds one leaf guard and follows sibling pointers. `Begin(key, prefix_len)` stops at `PrefixUpper`. On a 3-frame pool the pinned count stays at most 1 during the scan and is 0 after the iterator dies. Drop the current leaf before pinning the sibling.

**2.6 Bulk load.** Done. Build a packed tree from sorted unique keys. This is not a loop of `Insert`.

**2.7 Delete.** Done. Merge or redistribute so every node except the root stays at least half full. Reach either `BPlusTree::Remove::after_redistribute` or `BPlusTree::Remove::after_merge`. Update the parent separator when a key moves.

## RDF storage

**3.1 Term heap.** Done. A slotted page of byte strings. `Insert` returns nullopt when the record does not fit. A record larger than an empty page throws. `Get` is valid only while the page is pinned. `Delete` frees the slot for reuse. The iterator holds no pin after `operator*` returns. Delete compacts the page; a record is at most 4,076 bytes; inserts append to the last page of the chain.

**3.2 Disk dictionary.** Done. Spelling to id and id to spelling both survive restart. `Insert` of an existing spelling returns the original id. Ids are dense and start at 1. `INVALID_TERM_ID` is never assigned; call `Dictionary::CheckAssignable` before handing out an id. Spellings live in the term heap; a B+ tree keyed (id, page, slot) and one keyed (FNV-1a hash, id, 0) index them.

**3.3 Catalog.** Done. Remember the root of `"spo"`, `"pos"`, `"osp"`, and the dictionary. A missing name is `INVALID_PAGE_ID`. `Flush` makes it durable. Initialize the page so a zeroed file is not mistaken for root 0. An index entry names the tree's header page. The catalog writes page 0 at once so its id is taken, and refuses a non-empty page without its magic number.

**3.4 Indexed store.** Done. `Insert` and `Delete` update every index the catalog has a root for, or leave the store unchanged. `Scan` returns an iterator that drops its leaf guard. `\set backend indexed` selects this store. The differential test stops skipping. Permutations are named by key order; the catalog decides which exist. `\set backend indexed` builds the backend in a temporary file and moves the triples by spelling; persistent databases stay on the memory backend until PLAN 8.

**3.5 Permutations.** Done. All eight bound patterns return the right triples. Measure before adding a tree. SPO serves a bound subject. A bound object wants OSP or POS. Building all six is not the default. Write the set you kept, and why, in `benchmarks/indexing/question.md`. Kept SPO, POS, OSP; the numbers are in `benchmarks/indexing/results.md`.

**3.6 Clean shutdown.** Done. Flush the pool and the catalog, close the file, open it again, and see the triples and the dictionary.

## Query execution

The memory nested-loop join already returns the right rows. These slices change the cost and where the work lives.

**4.1 Rebind.** Done. The inner scan is closed and opened again with the outer row filled into the pattern. The test watches the pattern passed to `Scan`, because the rows are already correct.

**4.2 Index nested loop.** Done. For each outer row, probe the index with the bound prefix. `\set join inlj`. Same rows as nested loop.

**4.3 Hash join.** Done. Hash `term_id_t`. An unbound slot is not a key. `\set join hash`. Same rows as nested loop.

**4.4 Distinct, limit, offset.** Done. Distinct is on the projected row. Offset skips, then limit counts, and the child order is kept.

**4.5 Sort.** Done. `ORDER BY` in memory, then external merge sort through the buffer pool when the run does not fit. Compare the way `term_compare.cpp` does, not by raw id. `TermOrder` is a total order on values; the budget is half the pool (at least four pages), and merges take up to budget minus two runs.

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

**8.1 Records.** Done. Types: begin, commit, abort, update, CLR, checkpoint. `SerializeTo` writes `Size()` bytes. A short buffer throws. An update carries the triple and whether it inserted. A CLR carries `undo_next`. A checkpoint lists the active transactions. Put the length in the record.

**8.2 Log manager.** Done. `Append` assigns an LSN. `Flush(lsn)` blocks until that LSN is durable. Commit forces. Commits that overlap one flush share it. `\log` prints the tail, oldest first. The log file path is the `LogManager` constructor argument.

**8.3 WAL.** Every update sets pageLSN. The pool flushes the log before the page write, with the crash point between them. `\crash` exits without flushing.

**8.4 Abort.** When a log manager was passed in, abort undoes newest-first and writes a CLR per triple. The write-set path remains for a null log. A CLR is redone and is not undone.

**8.5 Checkpoint.** `\checkpoint` writes a checkpoint record and remembers its LSN. Blocking is enough. Say which kind in the comment above `Checkpoint`. This is a recovery bookmark, not a query snapshot.

**8.6 ARIES.** On open, analysis finds winners, losers, and the redo LSN. Redo repeats every page update whose LSN is greater than the pageLSN, including CLRs. Undo walks losers newest-first. `Summary` is one line per pass: `analysis`, `redo`, `undo`.

**8.7 Crash test.** `scripts/crashtest --seeds 200` is green. A committed insert is visible after restart. An uncommitted insert is not. The harness already detects a lost commit and a surviving uncommitted write on fake stores.

## Durable memory database

The shell's memory store, made durable without pages. Redo is logical: an update record names a triple, not a page, and its `page_id` is `INVALID_PAGE_ID`. This uses the log records (8.1) and the log manager (8.2) as they are, and leaves page-oriented recovery (8.3 to 8.7) for the disk store. `src/recovery/durability.cpp`, `src/recovery/snapshot.cpp`. Scenario tests in `test/durable/`.

The directory holds two files. `snapshot` is a full image at one LSN. `wal` holds every record after it.

**9.1 Checksums.** Every log record carries a CRC-32C over its bytes, as PostgreSQL, RocksDB, and InnoDB do for theirs. A record whose bytes are all present but wrong throws `LogCorruptionException`. A short buffer throws a plain `StorageException`, because a torn final write is what a crash leaves behind.

**9.2 Log scan.** `ScanLog` returns the valid prefix of a log and says where and why it stopped. Opening a `LogManager` on an existing log continues its LSNs.

**9.3 Statements are transactions.** A statement runs against a `RecordingStore` that remembers each triple it changed. Then the log gets begin, any dictionary terms not logged yet, one update per changed triple, and commit, and the commit is forced before the statement returns. A statement that throws is undone in memory. A statement that changed nothing logs nothing. Term records replay whatever their transaction's outcome, because the dictionary only grows and a term no committed triple uses is invisible. Updates replay only for committed transactions, in log order.

**9.4 Checkpoint.** `\checkpoint` writes the snapshot to a temporary file, fsyncs it, renames it over the old one, and then empties the log. A crash between the rename and the truncate leaves the new snapshot with the old log; replay skips every record at or below the snapshot's LSN. The snapshot carries its own CRC-32C, and a bad one refuses to open.

**9.5 Recovery modes.** Named after RocksDB's WAL recovery modes. `kPointInTime`, the default, keeps every transaction before the first bad record, truncates the log there, and reports what it dropped. `kAbsoluteConsistency` (`ontodb --strict <dir>`) refuses to open instead.

**9.6 Verify.** `\verify` rebuilds the database from the files into a fresh store, compares it with the live one, and checks the invariants: no duplicate triples, and every id in the dictionary. It changes nothing. In the same spirit as PostgreSQL's `pg_amcheck`, MySQL's `CHECK TABLE`, and RocksDB's `VerifyChecksum`.

**9.7 Shell flow.** One shell session loads an OWL file, updates it, checkpoints, verifies, and a second session on the same directory sees the result.

Scenarios covered: Turtle and RDF/XML loads survive a reopen; inserts and deletes replay in order; a no-op statement logs nothing; a crash before commit leaves no trace; term ids are stable; snapshot plus log replays to the same state; a crash after the snapshot does not replay twice; a corrupt snapshot refuses to open; a torn tail keeps earlier commits; a corrupt record drops everything after it; a tampered log fails `\verify`.
