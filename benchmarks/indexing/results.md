# Results: which permutations pay for themselves?

LUBM(1), 15 files, 103,104 triples parsed, 100,573 distinct. Release build on an Apple silicon Mac, 2026-10-15. Each cell is *answers / keys examined / page reads* (cold 64-frame pool), averaged over 20 sampled patterns.

| Permutations | Load (s) | File (MiB) | `---` | `s--` | `-p-` | `--o` | `sp-` | `s-o` | `-po` | `spo` |
|---|---|---|---|---|---|---|---|---|---|---|
| SPO | 1.38 | 8.2 | 100573 / 100573 / 1131 | 8 / 8 / 4 | 12228 / 100573 / 1131 | 1833 / 100573 / 1131 | 2 / 2 / 4 | 1 / 8 / 4 | 1805 / 100573 / 1131 | 1 / 1 / 4 |
| SPO POS OSP | 1.74 | 16.2 | 100573 / 100573 / 1131 | 8 / 8 / 4 | 12228 / 12228 / 128 | 1833 / 1833 / 25 | 2 / 2 / 4 | 1 / 1 / 4 | 1805 / 1805 / 25 | 1 / 1 / 4 |
| all six | 2.90 | 29.1 | 100573 / 100573 / 1131 | 8 / 8 / 4 | 12228 / 12228 / 143 | 1833 / 1833 / 25 | 2 / 2 / 4 | 1 / 1 / 4 | 1805 / 1805 / 25 | 1 / 1 / 4 |

## What it says

- **SPO alone fails every pattern that binds the predicate or the object without the subject.** `-p-`, `--o`, and `-po` read all 1,131 pages and examine all 100,573 keys to return 1,833 answers or fewer. `-po` (`?x rdf:type C`) is the most common pattern in LUBM's queries, so this is the case that matters.
- **POS and OSP fix exactly those shapes:** 9x fewer page reads for `-p-` (1131 to 128) and 45x fewer for `--o` and `-po` (1131 to 25). Every shape now examines only keys it returns. The cost is twice the space and 26% more load time.
- **SOP, PSO, and OPS change nothing.** Every shape already had a range scan; the extra three trees cost 80% more space and 67% more load time. (`-p-` reads a few more pages with six because a tie picks PSO, whose range for the predicate happens to span more leaves than POS's.) They would matter only to an operator that needs a second sort order, such as a merge join, which OntoDB does not have.

**Kept: SPO, POS, OSP**, the default in `IndexedStore::DefaultPermutations`. A store can still be built with any set.

Raw output: `benchmarks/indexing/raw/` (not committed). Rerun with `benchmarks/indexing/run.sh`.
