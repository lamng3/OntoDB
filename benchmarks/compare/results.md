# Results: OntoDB against Oxigraph

Apple M2 Pro, 16 GiB, release builds, 2026-10-22. See [question.md](question.md). Rerun with `benchmarks/compare/run.sh`.

## LUBM(10)

| Engine | triples | load (s) | on disk (MiB) | peak RSS (MiB) |
|---|---|---|---|---|
| OntoDB | 1,316,700 | 11.41 | 133.7 | 584 |
| Oxigraph (RocksDB) | 1,272,953 | 2.04 | 176.3 | 78 |
| Oxigraph (memory) | 1,272,953 | 2.44 | 0.0 | 563 |
| Jena TDB2 | 1,272,953 | 6.13 | 363.0 | 1447 |
| SQLite | 1,272,953 | 8.08 | 706.1 | 32 |

| Query | rows | OntoDB | Oxigraph (RocksDB) | Oxigraph (memory) | Jena TDB2 | SQLite |
|---|---|---|---|---|---|---|
| q01 | 4 | 9 µs | 32 µs | 12 µs | 508 µs | 18 µs |
| q02 | 28 | 8.2 ms | 275.1 ms | 34.1 ms | 120.1 ms | 4.0 ms |
| q03 | 6 | 11 µs | 37 µs | 13 µs | 234 µs | 18 µs |
| q04 | 0 | 4 µs | 116 µs | 34 µs | 250 µs | 41 µs |
| q05 | 0 | 1 µs | 1.5 ms | 185 µs | 524 µs | 12 µs |
| q06 | 0 | 0 µs | 11 µs | 8 µs | 163 µs | 8 µs |
| q07 | 0 | 3 µs | 166 µs | 28 µs | 220 µs | 32 µs |
| q08 | 0 | 4 µs | 20.6 ms | 2.0 ms | 3.7 ms | 43 µs |
| q09 | 0 | 5 µs | 28 µs | 18 µs | 186 µs | 77 µs |
| q10 | 0 | 1 µs | 24 µs | 10 µs | 161 µs | 15 µs |
| q11 | 0 | 21 µs | 51 µs | 14 µs | 170 µs | 934 µs |
| q12 | 0 | 3 µs | 1.3 ms | 255 µs | 446 µs | 336 µs |
| q13 | 0 | 1 µs | 16 µs | 9 µs | 154 µs | 16 µs |
| q14 | 75547 | 46.0 ms | 88.1 ms | 40.8 ms | 138.7 ms | 29.3 ms |

| Pattern | queries | mean rows | OntoDB mean / p99 | Oxigraph (RocksDB) mean / p99 | Oxigraph (memory) mean / p99 | Jena TDB2 mean / p99 | SQLite mean / p99 |
|---|---|---|---|---|---|---|---|
| s-- | 100 | 7.3 | 3 / 4 µs | 27 / 38 µs | 11 / 14 µs | 117 / 264 µs | 13 / 19 µs |
| sp- | 100 | 1.6 | 2 / 2 µs | 14 / 19 µs | 9 / 10 µs | 52 / 81 µs | 11 / 12 µs |
| --o | 100 | 16233.0 | 11004 / 55483 µs | 35727 / 179360 µs | 10395 / 52825 µs | 23688 / 187940 µs | 8418 / 43421 µs |
| -po | 100 | 16225.2 | 10248 / 56297 µs | 19370 / 94018 µs | 7771 / 38847 µs | 24752 / 171594 µs | 6208 / 32452 µs |
| -p- | 18 | 70719.6 | 70768 / 253050 µs | 161178 / 488041 µs | 41237 / 126185 µs | 107494 / 309161 µs | 35525 / 108409 µs |

## LUBM(1)

| Engine | triples | load (s) | on disk (MiB) | peak RSS (MiB) |
|---|---|---|---|---|
| OntoDB | 103,104 | 1.02 | 10.8 | 322 |
| Oxigraph (RocksDB) | 100,573 | 0.13 | 17.8 | 60 |
| Oxigraph (memory) | 100,573 | 0.18 | 0.0 | 53 |

| Query | rows | OntoDB | Oxigraph (RocksDB) | Oxigraph (memory) | Oxigraph/OntoDB | Oxigraph/OntoDB |
|---|---|---|---|---|---|---|
| q01 | 4 | 12 µs | 52 µs | 16 µs | 4.3x | 1.3x |
| q02 | 0 | 70 µs | 18.6 ms | 2.4 ms | 266.8x | 34.5x |
| q03 | 6 | 16 µs | 41 µs | 17 µs | 2.6x | 1.1x |
| q04 | 0 | 4 µs | 122 µs | 38 µs | 29.9x | 9.2x |
| q05 | 0 | 1 µs | 1.5 ms | 180 µs | 1262.8x | 149.8x |
| q06 | 0 | 0 µs | 14 µs | 11 µs | 28.4x | 21.4x |
| q07 | 0 | 3 µs | 174 µs | 34 µs | 60.1x | 11.9x |
| q08 | 0 | 4 µs | 17.6 ms | 3.7 ms | 4628.7x | 983.2x |
| q09 | 0 | 5 µs | 36 µs | 27 µs | 7.8x | 5.9x |
| q10 | 0 | 1 µs | 28 µs | 15 µs | 21.4x | 11.5x |
| q11 | 0 | 19 µs | 52 µs | 18 µs | 2.7x | 0.9x |
| q12 | 0 | 3 µs | 1.3 ms | 256 µs | 461.1x | 91.4x |
| q13 | 0 | 1 µs | 18 µs | 13 µs | 14.6x | 11.2x |
| q14 | 5916 | 6.3 ms | 7.2 ms | 1.7 ms | 1.1x | 0.3x |

| Pattern | queries | mean rows | OntoDB mean / p99 | Oxigraph (RocksDB) mean / p99 | Oxigraph (memory) mean / p99 |
|---|---|---|---|---|---|
| s-- | 100 | 7.6 | 17 / 28 µs | 29 / 40 µs | 13 / 16 µs |
| sp- | 100 | 1.6 | 3 / 6 µs | 16 / 20 µs | 10 / 11 µs |
| --o | 100 | 1355.7 | 2811 / 12856 µs | 2980 / 13506 µs | 648 / 3079 µs |
| -po | 100 | 1345.2 | 1431 / 6465 µs | 1578 / 7109 µs | 426 / 2497 µs |
| -p- | 31 | 3244.3 | 6781 / 37819 µs | 6485 / 36722 µs | 1838 / 11302 µs |

## Instances of a class and its subclasses (Gene Ontology, 200,000 entities)

answer counts that disagree: 0
| Below C | classes | answers | OntoDB ids | OntoDB strings | Oxigraph (RocksDB) | Oxigraph (memory) | RocksDB / OntoDB strings | memory / OntoDB strings |
|---|---|---|---|---|---|---|---|---|
| 1 | 30 | 10 | 1 µs | 11 µs | 37 µs | 13 µs | 3.3x | 1.2x |
| 2-9 | 30 | 40 | 3 µs | 42 µs | 91 µs | 23 µs | 2.2x | 0.6x |
| 10-99 | 30 | 278 | 10 µs | 275 µs | 539 µs | 110 µs | 2.0x | 0.4x |
| 100-999 | 30 | 2954 | 127 µs | 2891 µs | 5421 µs | 1135 µs | 1.9x | 0.4x |
| 1000+ | 30 | 29904 | 1742 µs | 29077 µs | 60578 µs | 21815 µs | 2.1x | 0.8x |

## Concurrency (LUBM(10), 3 s per cell, 1 / 2 / 4 / 8 threads)

| Engine | lookups/s, 1 / 2 / 4 / 8 threads | readers + 1 writer, 1 / 8 | inserts/s, 1 / 2 / 4 / 8 |
|---|---|---|---|
| OntoDB | 416,050 / 767,222 / 1,250,348 / 1,464,779 | 397,595 / 1,391,161 | 264,382 / 450,459 / 693,411 / 950,481 |
| Oxigraph (RocksDB) | 43,594 / 91,866 / 137,110 / 208,977 | 38,227 / 136,026 | 90,566 / 51,494 / 99,445 / 105,118 |
| Oxigraph (memory) | 96,048 / 175,432 / 256,917 / 459,776 | 69,370 / 287,999 | 678,869 / 415,591 / 195,710 / 185,828 |
| Jena TDB2 | 78,280 / 147,564 / 251,154 / 241,599 | 22,142 / 74,018 | 16 / 15 / 15 / 15 |
| SQLite | 141,488 / 256,339 / 391,720 / 375,194 | 111,420 / 321,855 | 14,949 / 24,719 / 23,373 / 14,418 |

Lookups are the `(s,.,.)` and `(s,p,.)` queries. Inserts are one new triple with a scattered subject per transaction; OntoDB's go through the store without the write-ahead log, Jena's commit forces its journal. SQLite uses one process and connection per worker (WAL, `synchronous=NORMAL`) because Python threads share a lock. The machine has 8 performance cores.

## What it says

See the paper's Section 8 (Experimental Study) and `paper/make-compare.py` for how every number is derived from `raw/`.
