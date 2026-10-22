# Semantic search in OntoDB

This note is the theory behind plan 14: what a sorted index can and cannot say about meaning, how a class hierarchy can be made a range scan, and how vector indexes fit beside the B+ trees. The code is in `src/semantic/`; the measurements are in `benchmarks/semantic/`; the paper is in `paper/`.

## 1. What the B+ tree orders today

A triple is a 24-byte key of three dictionary ids, compared with `memcmp` (`triple_key.h`). The dictionary hands out ids densely from 1, in the order terms are first seen: `MemDictionary::Insert` takes `next_++`, and `DiskDictionary::Insert` takes `Size() + 1`. The loader interns subject, predicate, and object of each statement in file order (`rdf_loader.cpp`).

So the order of the B+ tree is **first-seen order**. Two classes sit next to each other in the index if their IRIs first appeared close together in the input file. Nothing in the engine sorts by hierarchy, by label, or by meaning. If a file happens to list a class right after its parent, the ids are close; that is a property of the file, not of OntoDB.

What a range scan does give is *structural* grouping: a bound prefix (`s--`, `-po`, ...) is one contiguous scan, because the key puts the bound positions first (`benchmarks/indexing`).

## 2. A sort order cannot encode similarity

Suppose we want a single order on terms such that a range scan around a term returns its semantic neighbors. For vectors in two or more dimensions this is impossible in general.

**Lemma 1 (bandwidth).** Let `G` be a graph on `n` vertices with diameter `D`. Every linear order of the vertices has an edge whose endpoints are at least `(n-1)/D` apart in the order.

*Proof.* Take the first and the last vertex in the order. They are at order distance `n-1` and, along a shortest path of at most `D` edges, some edge must cover at least `(n-1)/D` of that distance. ∎

Apply it to the neighbor graph of a `k × k × ... × k` grid in `d` dimensions (`n = k^d`, diameter `d(k-1)`): some pair of grid neighbors is at least `(k^d - 1) / (d(k-1)) ≥ k^(d-1) / d` positions apart. A range scan that reads `w` positions around a point cannot contain all of its grid neighbors unless `w` is that large, which is `Ω(n^((d-1)/d) / d)`. In 2 dimensions with a million points, some neighbors are 500 positions apart in any order; with 384 dimensions the bound says nothing is gained by ordering.

Space-filling curves (Z-order, Hilbert) keep neighbors close *on average* and need several range scans to cover a query ball; they do not escape the lemma. The consequence is the one in the plan: a B+ tree, and an ART, are the wrong structure for "words close in meaning". Neither is a vector index.

## 3. The exception: a hierarchy is a tree, and a tree is an interval

A class hierarchy (`rdfs:subClassOf`) *does* have an order in which "everything below `C`" is contiguous.

**Lemma 2 (tree).** Number the nodes of a rooted forest `1..n` in postorder. For a node `c` let `lo(c)` be the smallest number in its subtree. Then `d` is in the subtree of `c` iff `lo(c) ≤ po(d) ≤ po(c)`.

*Proof.* Postorder numbers a subtree as one contiguous block that ends at its root. ∎

So "all subclasses of `C`" is one range scan over an index keyed by `po`.

Real ontologies are DAGs: a class can have several parents (Gene Ontology, release 2026-07-26, without obsolete terms: 57,456 `is_a` edges over 38,092 terms). Following Agrawal, Borgida, and Jagadish (SIGMOD 1989), pick a spanning forest, number it in postorder, and give each class a **set** of intervals: its own tree interval, plus the intervals of every child, merged.

**Lemma 3 (interval sets).** With `intervals(c) = merge(own(c) ∪ ⋃_{c→v} intervals(v))`, `d` is a descendant of `c` iff `po(d)` lies in some interval of `c`.

*Proof.* By induction over a reverse topological order: the set of numbers covered is exactly `{po(d) : d descends from c}`. ∎

**Lemma 4 (size).** Let `E'(c)` be the non-tree edges whose tail is a descendant of `c`. Then `|intervals(c)| ≤ 1 + |E'(c)|`.

*Proof.* Any descendant `d` is reached from `c` by a path. If the path uses no non-tree edge, `d` is in the tree subtree of `c`. Otherwise let `v` be the head of the last non-tree edge on the path; `d` is in the tree subtree of `v`. So the covered set is `own(c) ∪ ⋃_{e ∈ E'(c)} own(head(e))`, a union of `1 + |E'(c)|` intervals, which has at most that many maximal intervals. ∎

For a tree `E' = ∅` and every class has one interval. How good the spanning forest is decides how many extra intervals a DAG needs; `docs/semantic-search.md` section 8 records what GO needed.

### Why this beats materializing the closure

`?x rdf:type C`, with RDFS semantics, means "`x` has a type in the subtree of `C`". Three ways to answer it:

| Strategy | Extra stored | One query costs |
| --- | --- | --- |
| Rewrite | nothing | one POS range scan per descendant of `C`: `|desc(C)|` seeks |
| Materialize (plan 13) | a type triple per ancestor of each type | one POS range scan |
| Interval index | one key per `rdf:type` triple | `|intervals(C)|` range scans |

The interval index stores `(po(type), instance)` once per type triple and answers with the few intervals of `C`. The price is that `po` changes when the hierarchy changes, so the labels are rebuilt (O(n + m)) instead of maintained; the table does not need updating for an instance insert. Section 7 lists the limits.

### Why ids stay first-seen

We could renumber terms so that dictionary ids themselves follow the hierarchy, and the existing SPO/POS/OSP trees would then give subtree scans for free. We do not: ids are stored in three permutations, in the log, and in snapshots; a hierarchy edit would renumber all of them. The label `po` is a separate, rebuildable column, so ids keep their one property (stable). The cost is one more small index.

## 4. Vector indexes

One embedding per term id, `d` floats. Distance is cosine, so vectors are stored normalized and the score is a dot product. Four index families, each a different trade between exactness, memory, and build cost:

- **Flat.** Score every vector: `O(n d)`, exact. With 8-bit scalar quantization (one scale per vector) the stored size is `n(d + 4)` bytes, a quarter of float32, and each coordinate of a stored vector is off by at most `s/2` with `s = max|x|/127`, so for a unit query `|q·x - q·x̂| ≤ (s/2)√d`.
- **IVF** (inverted file; Jégou, Douze, Schmid 2011). Cluster the vectors into `L` lists with k-means; a query scores the `L` centroids, then only the `p` nearest lists. Cost `L d + p (n/L) d`, minimized at `L = √(p n)`, giving `2d√(p n)`. Recall rises with `p/L`. **The lists are a B+ tree**: key `(list, term id, 0)`, so a list is a prefix scan and an insert is a tree insert.
- **PQ** (product quantization). Split a vector into `m` sub-vectors and replace each by the index of its nearest of 256 centroids: `m` bytes per vector. A query builds `m` tables of 256 distances (`256 d` work) and scores each vector with `m` table lookups (asymmetric distance computation).
- **HNSW** (Malkov and Yashunin). A multi-layer proximity graph searched greedily. Memory `O(n M)`, build is the slowest, recall at a given latency is usually the best, and there is no worst-case bound.

### Filtered search: where ontologies differ

A hybrid query is "the `k` terms nearest `t` that lie under class `C`". Let `s = |subtree(C)| / n` be the selectivity.

- **Pre-filter.** Resolve the interval scan first, score only those terms. Exact, `O(s n d)`. Wins when `s` is small.
- **Post-filter.** Run the vector index, drop what is outside `C`, retry with a larger candidate list until `k` survive. If the filter were independent of similarity one would need about `k/s` candidates, so it fails when `s` is small.
- **In-index filter.** Test the predicate while scanning IVF lists or walking the HNSW graph (the idea of ACORN, Patel et al., SIGMOD 2024).

In an ontology the filter is *not* independent of the vectors: the names of a class's descendants are semantically close to each other and to the class. A query inside `C` finds most of its neighbors inside `C` (post-filter works better than `k/s` says); a query outside `C` finds almost none (it works much worse). Section 8 measures both.

## 5. Why not an ART with sibling links

An ART orders keys by byte, so its range scan is a *lexical* range scan: all labels starting with `rub`. That is a real query (autocomplete) and the ART is good at it, but it is the same one-dimensional order as the B+ tree and Lemma 1 applies unchanged.

Sibling links on an ART leaf list are possible. Path compression and node growth never move a leaf. Lazy expansion does: an insert that meets a leaf turns it into an inner node and puts a *copy* of the leaf under it (`replace(node, new)` in the visualization's source), so a link that pointed at the old object now points at an inner node. Each insert must also find its sorted neighbors, a partial descent, and concurrent inserts must latch both. A B+ tree has the link for free because a leaf is a page and a split is local.

The stack-based iterator the visualization shows (`ARTIterator`) is the alternative: no link, climb and descend with a stack of `[node, keys, next child]`. It pays pointer chasing between leaves and costs nothing on insert. OntoDB's keys are three fixed-width integers, which gives an ART little prefix to compress, so the B+ tree stays.

## 6. Pessimistic and optimistic prefixes

Path compression stores a node's shared prefix. *Pessimistic* storage keeps the bytes in the node, so a descent compares them and fails at the first wrong byte; the cost is variable-length storage. *Optimistic* storage keeps only the length: the descent skips those bytes without comparing and the full key is checked once, at the leaf. Nodes are smaller; a wrong path is found late.

The difference matters for range scans. Seeking a lower bound `lo` needs to know whether a whole subtree lies below or above `lo`. With stored prefix bytes the seek decides at the node (`node.content != part`). With skipped bytes it must load a key from the subtree and compare. The upper bound needs no jumping or binary search: a second seek finds the first key past `hi`, and each step compares two leaf keys. The only binary search in a scan is over the sorted edge bytes of one node (`bisect_left` in the iterator).

## 7. Decisions and limits

- **Ids stay first-seen** (section 3). Labels are a rebuildable side index.
- **Labels are rebuilt, not maintained.** An edit to the hierarchy marks the labels stale; `\hindex build` rebuilds in `O(n + m)`. Incremental maintenance with gaps (Agrawal et al.) is not built.
- **Cycles** (`A ⊑ B ⊑ A`, an equivalence) are collapsed into one class before labelling, as RDFS says they denote the same class.
- **Only `rdfs:subClassOf`.** `subPropertyOf`, `domain`, `range`, and OWL restrictions are not interpreted. This is not a reasoner.
- **Vectors live in memory** and are saved to and loaded from a file; they are not pages in the buffer pool yet. The IVF lists are pages.
- **Embeddings come from outside.** OntoDB stores and searches vectors; a model makes them. The shell takes a vector file and can search from the vector of a term that is already stored. A query *text* needs a model in the process, which OntoDB does not link.
- **Single-threaded vector search.** The B+ tree is latch-safe; the vector structures are not.

## 8. Measurements

Full tables: `benchmarks/semantic/results.md`. On the Gene Ontology (38,092 classes, 57,456 `is_a` edges) with MiniLM embeddings of the term names:

- **Order.** The hierarchy's postorder is the best order (median rank gap to a term's nearest neighbor 45, against 1,739 for first-seen ids), and it still finds only 38% of the ten nearest neighbors within ±100 positions and 56% within ±1000. Lemma 1 is not an artifact of the grid.
- **Labels.** 2.01 intervals per class on average (598 KiB for the ontology), at most 741, always within the bound of Lemma 4. Reachability takes 27 ns, 29 times faster than a search up the parents. A subtree of 1,000+ classes is 101 range scans, 48 times fewer page reads than one scan per class.
- **Type queries.** On 200,000 synthetic entities, materializing the closure stores 8.1 times the triples (309.5 MiB against 63.6) and takes 180 s to load against 5; the interval index adds 15% and 0.17 s, and reads 265 pages against 182 (materialized) and 12,803 (rewrite) for the largest classes.
- **Vector indexes.** HNSW reaches recall 0.993 at 44 µs, 51 times faster than the exact scan (2.2 ms). IVF with lists in a B+ tree reaches 0.929 at 255 µs; its per-row cost is about 2.7 times that of a flat scan. PQ is for memory, and needs exact re-ranking for accuracy.
- **Filtering by a class.** Resolving the members and scoring them exactly is best up to about 1,000 members (3 to 62 µs, exact). The filter is correlated with the vectors: the share of the unfiltered top 100 under the class is 0.04 to 0.72 for a query inside it and 0.00 to 0.12 outside, so post-filtering fails for outside queries (IVF recall 0.07 on small classes). Beyond 5,000 members a masked HNSW search wins (105 µs inside, 900 µs outside).

## 9. Related systems

Which databases already pair a graph or ontology with vector search, as far as their documentation says:

| System | What it offers | Index |
| --- | --- | --- |
| Neo4j | vector index on node properties, queried from Cypher | HNSW |
| Amazon Neptune Analytics | vector search over graph nodes, `topK` calls in openCypher | managed, set at graph creation |
| GraphDB (Ontotext/Graphwise) | similarity indexes queried from SPARQL (random indexing, since 8.x); vector search through an Elasticsearch connector in 11.x | external |
| Fluree | inline similarity functions in queries, and HNSW vector indexes | HNSW |
| AllegroGraph | stores LLM embeddings and compares them to find similar fragments | see its docs |
| OxiRS Vec | Rust library for RDF with HNSW, flat, IVF, and filtered search (experimental) | HNSW, IVF, flat |
| pgvector, DuckDB `vss` | vector columns in a relational or embedded database | IVFFlat and HNSW (pgvector); HNSW (DuckDB `vss`) |
| Milvus, Qdrant | dedicated vector databases | many index types (Milvus); HNSW (Qdrant) |

The rows come from each system's public documentation and a survey (Pan, Wang, Li, VLDB Journal 2024), read in October 2026; the AllegroGraph and GraphDB 11.x details were not checked against their current manuals. In the sources checked, none describes the combination here: a hierarchy turned into range-scannable intervals, in the same B+ tree engine as the triples, used to filter a vector search. That is the part to evaluate, not the existence of a vector index.
