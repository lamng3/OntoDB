# Benchmarks

Comparisons live here, one directory per question. The engine stays one codebase. A method is a knob or a small replaceable piece (eviction, which permutations exist, which join runs, how often a checkpoint is taken). Record the knob in the result table. Do not fork the repository for a method.

```text
benchmarks/<question>/
  question.md    what is compared, on which data, and which metric decides
  run.sh         the commands, with the dataset and the knobs
  results.md     the table a writeup will cite
  raw/           logs and traces; git ignores this directory
```

Start a directory when the feature it measures exists. The first questions this store is set up to answer:

| Directory | Question | Waits on |
| --- | --- | --- |
| `eviction` | How do pool size and LRU-K change the hit ratio on one query mix? | Buffer pool |
| `indexing` | Which permutations pay for themselves on LUBM? | RDF storage, bulk load |
| `joins` | Nested loop, index nested loop, hash, and the optimizer's own choice | Query execution, optimizer |
| `recovery` | Group commit and checkpoint interval against throughput and restart time | Recovery |

`scripts/fetch-data lubm` builds the LUBM(1) file used by the indexing and join questions. It needs Java. `data/tiny.ttl` is the smoke dataset, not a benchmark.

A writeup lives in `writeups/` and links to `results.md`. Leave the raw logs out of the writeup.
