# Which permutations pay for themselves?

Each permutation of (subject, predicate, object) is a full copy of the triples in a B+ tree. A pattern whose bound positions are a prefix of some permutation is a range scan; otherwise it is a scan of a whole tree plus a filter. Hexastore keeps all six permutations. Three (SPO, POS, OSP) are enough to make every one of the eight bound/unbound shapes a prefix of one of them.

**Compared:** three permutation sets, chosen with the `IndexedStore` permutation list:

- SPO only
- SPO, POS, OSP
- all six

**Data:** LUBM(1), the 15 department files `scripts/fetch-data lubm` generates (103,104 triples parsed, 100,573 distinct).

**Workload:** each of the eight pattern shapes (`---`, `s--`, `-p-`, `--o`, `sp-`, `s-o`, `-po`, `spo`), with constants taken from 20 triples sampled from the data (the same 20 for every set); the unbound shape runs once.

**Metrics:** load time and file size; per shape, the average number of answers, index keys examined (answers plus keys the filter threw away), and page reads on a cold 64-frame pool. Page reads decide; space is the price.

**Decision rule:** keep a permutation if it turns some shape from a whole-tree scan into a range scan. A permutation that changes no shape's cost is not kept, whatever its space.

Run: `benchmarks/indexing/run.sh`. Results: [results.md](results.md).
