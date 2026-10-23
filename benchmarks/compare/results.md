# Results: OntoDB against Oxigraph

Apple M2 Pro, 16 GiB, release builds, 2026-10-22. See [question.md](question.md). Rerun with `benchmarks/compare/run.sh`.

## LUBM(10)

| Engine | triples | load (s) | on disk (MiB) | peak RSS (MiB) |
|---|---|---|---|---|
| OntoDB | 1,316,700 | 13.83 | 133.7 | 637 |
| Oxigraph (RocksDB) | 1,272,953 | 1.95 | 176.3 | 87 |
| Oxigraph (memory) | 1,272,953 | 2.32 | 0.0 | 546 |

| Query | rows | OntoDB | Oxigraph (RocksDB) | Oxigraph (memory) | Oxigraph/OntoDB | Oxigraph/OntoDB |
|---|---|---|---|---|---|---|
| q01 | 4 | 12 µs | 38 µs | 16 µs | 3.1x | 1.3x |
| q02 | 28 | 8.0 ms | 253.6 ms | 30.5 ms | 31.7x | 3.8x |
| q03 | 6 | 17 µs | 41 µs | 17 µs | 2.5x | 1.0x |
| q04 | 0 | 4 µs | 120 µs | 41 µs | 27.8x | 9.5x |
| q05 | 0 | 1 µs | 1.5 ms | 186 µs | 1215.1x | 155.4x |
| q06 | 0 | 0 µs | 14 µs | 10 µs | 28.2x | 20.8x |
| q07 | 0 | 3 µs | 168 µs | 34 µs | 54.3x | 11.0x |
| q08 | 0 | 4 µs | 17.0 ms | 1.9 ms | 4144.9x | 470.0x |
| q09 | 0 | 5 µs | 38 µs | 25 µs | 7.6x | 5.0x |
| q10 | 0 | 2 µs | 30 µs | 14 µs | 19.9x | 9.2x |
| q11 | 0 | 21 µs | 55 µs | 17 µs | 2.6x | 0.8x |
| q12 | 0 | 3 µs | 1.4 ms | 261 µs | 450.7x | 84.3x |
| q13 | 0 | 1 µs | 21 µs | 13 µs | 16.1x | 9.8x |
| q14 | 75547 | 84.8 ms | 89.8 ms | 33.3 ms | 1.1x | 0.4x |

| Pattern | queries | mean rows | OntoDB mean / p99 | Oxigraph (RocksDB) mean / p99 | Oxigraph (memory) mean / p99 |
|---|---|---|---|---|---|
| s-- | 100 | 7.3 | 17 / 28 µs | 28 / 39 µs | 12 / 14 µs |
| sp- | 100 | 1.6 | 4 / 7 µs | 16 / 20 µs | 10 / 10 µs |
| --o | 100 | 16233.0 | 33396 / 162255 µs | 33619 / 159955 µs | 9579 / 47290 µs |
| -po | 100 | 16225.2 | 17286 / 82967 µs | 18602 / 91772 µs | 7083 / 35626 µs |
| -p- | 18 | 70719.6 | 148860 / 460805 µs | 146380 / 451148 µs | 40474 / 128548 µs |

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

## What it says

- **Selective patterns:** OntoDB is 1.6x (`s--`) to 4.0x (`sp-`) faster than Oxigraph on RocksDB. On patterns with thousands of rows they are equal. Oxigraph's memory store is 3.5x faster than both on large results.
- **Load:** Oxigraph loads LUBM(10) 7.1x faster and peaks at a seventh of the memory. OntoDB's file is 24% smaller.
- **LUBM queries:** q04 to q13 return no rows without inference; their ratios are two tiny times and mean nothing. q02 (the triangle) is 31.7x faster in OntoDB than on RocksDB, 3.8x faster than Oxigraph's memory store. q14 (a large scan) is equal.
- **Class instances:** with ids only, the interval scan returns 29,904 answers in 1.7 ms (58 ns each). Decoding them to strings takes it to 29 ms, 94% of the time: the dictionary costs about 1 us per term. At the string level OntoDB is 1.9x to 3.3x faster than Oxigraph on RocksDB and 1.3x to 2.5x slower than its memory store for classes with two or more instances.
