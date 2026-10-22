# Does a hierarchy help find meaning, and what do intervals and vectors each cost?

Five questions, one program (`tools/bench/semantic.cpp`), run by `benchmarks/semantic/run.sh`. Theory: `docs/semantic-search.md`.

**Data.**

- *Gene Ontology*, `go-basic.obo` release 2026-07-26, without obsolete terms: 38,092 classes, 57,456 `is_a` edges, 95,548 triples with labels (`scripts/fetch-data go`). A real DAG with multiple parents.
- *Embeddings*: the name of each term, embedded on the CPU by `all-MiniLM-L6-v2` (22M parameters, 384 dimensions, unit length) through `sentence-transformers` (`scripts/embed-terms`).
- *LUBM(1)* with its ontology (43 classes, shallow): 103,410 triples (`scripts/fetch-data lubm lubm-ontology`).
- *Synthetic instances*: 200,000 entities with one to three `rdf:type` triples each, the classes drawn uniformly from the Gene Ontology's classes. This is not a real annotation set; it exists to give the hierarchy something to be queried against.

**Experiments.**

1. `order`: does any sort order put a term's nearest neighbors next to it? For 2,000 random terms, the 10 nearest by cosine. Orders: dictionary id (first seen), the hierarchy's postorder under two spanning-forest rules, label alphabetical, random. Metric: the fraction of the 10 within ±W positions, and the median position gap to the nearest neighbor. Also the fraction within two `is_a` steps.
2. `labels`: size of the interval sets under two spanning-forest rules (deepest parent, first parent); the check that no class exceeds `1 + non-tree edges` intervals; build time; `IsDescendant` against a search up the parents; `Descendants(C)` by interval scans against one POS scan per class, on a cold 64-frame pool (page reads decide).
3. `types`: `?x rdf:type C` three ways (intervals, rewrite, materialized) on LUBM(1) and on the synthetic instances: store size, load time, and per query the answers, scans, keys read, page reads (cold 64-frame pool), and time, for classes sampled by subtree size.
4. `knn`: recall@10 against latency for flat float32, flat 8-bit, flat PQ (with and without exact re-ranking), IVF (three list counts and three encodings), and HNSW (three M). 1,000 random terms as queries, single thread, release build, ground truth by exact search.
5. `hybrid`: the 10 nearest terms under a class, six strategies, for classes sampled by subtree size, with a query from inside the class and a query from outside it. Recall against the exact answer over the class's members.

**Decision rules.**

- Keep ids first-seen if no id order matches the hierarchy's postorder at the same window; otherwise reconsider.
- Keep interval labels over materialized closures if a type query costs no more page reads, and the store is smaller.
- `kAuto` switches from pre-filter to a masked index search at the subtree size where the two cost the same in `hybrid`; set `pre_threshold` from that crossing.

**Machine.** Apple M2 Pro, 12 cores, 16 GiB. Release build (`-O3`), one thread.

Run: `benchmarks/semantic/run.sh`. Results: [results.md](results.md).
