# Architecture

OntoDB is a SPARQL store. A query is a pipeline of small types. A committed insert is the same pipeline plus the log. The files below are the ones each path actually touches.

## One query

Take this, typed at the `OntoDB>` prompt after `\load data/tiny.ttl`:

```sparql
PREFIX ub: <http://example.edu/univ#>
SELECT ?name WHERE {
  ?s a ub:Student ; ub:name ?name ; ub:age ?age .
  FILTER (?age > "21"^^xsd:integer)
}
```

1. `tools/shell/main.cpp` builds a `Database` and a `Shell` on stdin.
2. `tools/shell/shell.cpp` counts braces, so a query may span lines. `#` inside `<...>` is not a comment. A backslash line is looked up in the command table in `tools/shell/commands.cpp`; anything else is SPARQL. The line is handed to the session worker in `tools/shell/session.cpp`.
3. `src/common/database.cpp` `Database::Execute` copies the current `Config` and calls the parser.
4. `src/parser/lexer.cpp` and `src/parser/parser.cpp` produce an AST. `a` is `rdf:type`. `;` and `,` repeat the subject and predicate. `OPTIONAL`, `UNION`, property paths, aggregates, and `DELETE/INSERT WHERE` throw `ParseException` with a line and column.
5. `src/binder/binder.cpp` resolves prefixes, interns constants in `src/dictionary/mem_dictionary.cpp`, and assigns each variable a slot in first-seen order. `src/common/term_codec.cpp` is the one spelling both the loader and the binder use (`<iri>`, `"lex"`, `"lex"@en`, `"lex"^^<datatype>`).
6. `src/planner/planner.cpp` builds a left-deep tree: one `TripleScanPlan` per pattern, `NestedLoopJoinPlan` between them, then `FilterPlan`, then `ProjectionPlan`. `ORDER BY`, `DISTINCT`, and `LIMIT` become plan nodes whose executors are still phase 4.
7. `src/execution/execution_engine.cpp` asks `src/execution/executor_factory.cpp` for a volcano tree. `Init` / `Next` pull rows of `term_id_t`. The scan is `src/execution/executors/triple_scan_executor.cpp`. The join is `src/execution/executors/nested_loop_join_executor.cpp` (it does not yet rebind the inner scan; that is plan 4.1). The filter is `src/execution/executors/filter_executor.cpp`, using `src/execution/term_compare.cpp`. The projection is `src/execution/executors/projection_executor.cpp`.
8. The scan calls `src/store/mem_store.cpp`, which returns a `src/store/mem_triple_iterator.cpp` over the triples whose bound positions match. Rows are decoded through the dictionary only when the engine builds the result.
9. The shell prints the header and the rows. `\explain` prints the plan text from the same nodes and does not execute.

`\set backend indexed` is rejected until plan 3.4. The differential test in `test/queries/differential_test.cpp` skips until `DiskManager` exists, then runs every query file on both stores.

## One committed insert

Today `INSERT DATA` is applied in `execution_engine.cpp` straight to `MemStore::Insert`. It does not go through `InsertExecutor` (plan 4.6) and it does not touch the disk. This is the path that insert takes once the later phases are filled in. Each name is the file that owns that step.

1. The shell and `Database::Execute` parse and bind as above. `src/planner/planner.cpp` emits an `InsertPlan` (`src/include/execution/plans/stub_plans.h`).
2. Plan 4.6's insert executor writes every index the catalog knows, and the dictionary, or writes nothing if the triple is already present.
3. `src/dictionary/disk_dictionary.cpp` (plan 3.2) turns the three spellings into ids. Bytes live in `src/storage/term/term_heap.cpp` on `src/include/storage/page/term_heap_page.h` (plan 3.1).
4. `src/storage/index/triple_key.cpp` (plan 0.2, done) packs the three ids into 24 big-endian bytes. `memcmp` is the order.
5. `src/store/indexed_store.cpp` (plan 3.4) inserts that key into each permutation `src/catalog/catalog.cpp` (plan 3.3) has a root for. The tree is `src/storage/index/b_plus_tree.cpp`. Leaves and internal pages are `src/storage/page/b_plus_tree_leaf_page.h` and `b_plus_tree_internal_page.h` (plan 2.1, done; the byte layout is in those headers). The iterator is `src/storage/index/index_iterator.cpp`.
6. The tree pins pages with `src/storage/page/page_guard.cpp` (plan 1.5, done). Frames live in `src/buffer/buffer_pool_manager.cpp` (plan 1.4, done; one latch, I/O through the scheduler, no flush in the destructor). Eviction is `src/buffer/lru_k_replacer.cpp` (plan 1.3, done).
7. A dirty frame is written by `src/storage/disk/disk_scheduler.cpp` (plan 1.2, done; one thread, requests in order, errors through the promise) onto `src/storage/disk/disk_manager.cpp` (plan 1.1, done; the file length records allocation, and freed ids are not reused). Every page image starts with the pageLSN in `src/storage/page/page.cpp`.
8. Before the page is written, the buffer pool flushes the log up to that pageLSN (plan 8.3). The record is a `src/include/recovery/log_record.h` (plan 8.1) appended by `src/include/recovery/log_manager.h` (plan 8.2). Commit forces the log. The commit record is the acknowledgement the crash harness trusts.
9. On the next open, `src/include/recovery/log_recovery.h` (plan 8.6) runs analysis, redo, and undo. Undo of a triple is logical and writes a CLR (plan 8.4). A split or merge is redone, not logically undone.

`\crash` exits without flushing. `\checkpoint` is plan 8.5. Until those exist, opening a path prints `recovery not built yet (PLAN 8.6)` and starts an empty memory database.

## Deviations

- `src/include/storage/page/page.h` is a real frame type, not in the original file list. pageLSN has to live in one place so the buffer pool, the tree, and the log share it.
- `src/include/common/database.h` is the facade the shell and the harnesses call.
- `src/include/common/crash_point.h` is the named hook multi-page operations call. Tests arm those names.
- `src/include/common/term_codec.h` is the shared spelling of IRIs, literals, and blanks.
- `src/include/common/session_state.h` carries the lock-wait reason so the lock manager does not depend on the shell.
- `src/include/execution/plans/abstract_plan.h`, `abstract_executor.h`, and `stub_plans.h` are the volcano bases. Stub plans exist so `\explain` can print `Sort`, `Distinct`, `Limit`, `Insert`, and `Delete` before those executors exist.
- `Schema` lives in `src/include/common/types.h`. `lsn_t` is `int64_t` so a long benchmark does not wrap the page header.
- `MemStore` and `MemDictionary` take a mutex so two shell sessions can share them. That mutex is not isolation.
- `DiskManager`, `DiskScheduler`, and `BufferPoolManager` take optional `IoStats*` and `TraceSink*`.
- `INSERT DATA` / `DELETE DATA` run in `ExecutionEngine` until plan 4.6. The executors stay stubs.
- B+ tree page classes wrap a `Page*`. The byte layout (plan 2.1) is documented in their headers.
- `Transaction` accessors and `LogRecord` factories hold data. The manager and the serialization are stubs.
- `TransactionManager` takes an optional `LogManager*`. Null means write-set undo (plan 7.2). Non-null means log undo (plan 8.4). The pointer is in the signature now so plan 8.4 does not have to change it.
- `\trace on|off` switches `SwitchableTraceSink`, which prints component events to stderr. `\bpm` and `\page` report that the memory backend has no buffer pool.
- The devcontainer installs `default-jre-headless` so `scripts/fetch-data lubm` can run the official generator.
- Targets compile with `-Wno-unused-private-field` because stub objects store the pointers the student will use.
- `Page` carries a reader-writer latch (`RLatch`/`WLatch`) for the guards, so a `Page` is neither copyable nor movable.
- `.clang-tidy` enables `bugprone-*` and `clang-analyzer-*`. The broader `modernize` and `readability` sets are left off so the tidy job can stay green on a scaffold of intentional stubs.
- `data/pizza.ttl` is a small original subset used by the query tests. `scripts/fetch-data pizza` downloads the real ontology into `data/downloads/`, which is gitignored.

## One statement in a persistent database

This path runs today when the shell was started with a directory (`ontodb <dir>`).

1. `Database::Open` (`src/common/database.cpp`) builds the database and a `Durability` (`src/recovery/durability.cpp`) over the same dictionary and store. `Durability::Open` loads `snapshot` (`src/recovery/snapshot.cpp`), scans `wal` (`ScanLog` in `src/recovery/log_manager.cpp`), lets the recovery mode's damage policy decide about a bad tail, and replays the committed transactions.
2. `Database::Execute` parses and binds as above. An update runs inside `Durability::Run`, against a `RecordingStore` (`src/include/store/recording_store.h`) that remembers each triple it changed.
3. `Durability::LogTransaction` appends begin, any new dictionary terms, one update per change, and commit through `LogManager`, then forces the commit. If anything throws, `RecordingStore::Undo` takes the changes back.
4. `\checkpoint` writes a new snapshot by rename and empties the log. `\verify` runs four checks (snapshot, log, replay, invariants), one report line each.

## Design

The code uses a small set of standard patterns, so a new method plugs in at a known place.

| Pattern | Where |
| --- | --- |
| Iterator (volcano) | `TripleIterator`; every executor's `Init` / `Next` pulls one row at a time |
| Repository | `TripleStore` and `Dictionary` hide where triples and terms live (`MemStore`, `IndexedStore`; `MemDictionary`, `DiskDictionary`) |
| Factory | `ExecutorFactory::Create` maps a plan node to its executor; `Database::Open`; `LogRecord::Begin`, `Commit`, `Update`, and the other named constructors; `MakeDamagePolicy` |
| Builder | `Planner` builds the plan tree from a bound query |
| Strategy, replace conditional with polymorphism | `LogDamagePolicy` with one class per recovery mode; `\set join` selects the join operator; `TraceSink` implementations |
| Null object | `NullTraceSink` when tracing is off |
| Decorator | `RecordingStore` wraps any `TripleStore` and records its changes |
| Adapter | `RdfLoader` adapts raptor2's parser to `Dictionary` and `TripleStore`; an exception from either is caught in the C callback, the parse is aborted, and it is rethrown after raptor is freed |
| Command | The shell's command table (`tools/shell/commands.cpp`): one entry per backslash command, read by both dispatch and `\help`; log records are replayable commands |
| RAII | `ReadPageGuard` and `WritePageGuard` unpin on destruction |
| Dependency injection | `ExecutorContext` carries the store, dictionary, config, and stats into every executor; `LogManager`, `Durability`, and the buffer pool take their collaborators as constructor arguments |

Deliberate exceptions:

- **No retry on a failed fsync.** A log flush that fails is reported, not retried. After a failed fsync the kernel may have dropped the dirty pages, so a retry that succeeds can still have lost data; this is the lesson of PostgreSQL's 2018 fsync incident.
- **One process-wide test hook.** `CrashPoint` is global because a crash point has to be reachable from any depth of any call. It does nothing unless a test arms it.
- **Constants, not literals.** Sizes and intervals have names: `PAGE_SIZE` and the defaults in `src/include/common/config.h`, `LogRecord::kHeaderSize` and `kMaxSize`, `kFlushInterval` in the log manager, and `kDefaultLogTail` in the shell.
