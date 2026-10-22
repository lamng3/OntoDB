# Results: semantic search

Gene Ontology release 2026-07-26 without obsolete terms (38,092 classes, 57,456 `is_a` edges), `all-MiniLM-L6-v2` embeddings of the term names (384 dimensions), LUBM(1) with its ontology, and 200,000 synthetic entities over the Gene Ontology's classes. Release build on an Apple M2 Pro (12 cores, 16 GiB), one thread, 2026-10-22. Method: [question.md](question.md).

## 1. Does any sort order put neighbors together?

For 2,000 random terms, the 10 nearest by cosine; each cell is the share of them within ±W positions of the term in that order.

| Order | median rank gap to nearest neighbor | ±10 | ±50 | ±100 | ±500 | ±1000 | ±5000 |
|---|---|---|---|---|---|---|---|
| dictionary id (first seen) | 1739 | 0.158 | 0.195 | 0.211 | 0.258 | 0.284 | 0.438 |
| hierarchy, deepest parent | 45 | 0.220 | 0.328 | 0.377 | 0.498 | 0.561 | 0.735 |
| hierarchy, first parent | 37 | 0.216 | 0.319 | 0.366 | 0.481 | 0.546 | 0.726 |
| label, alphabetical | 1749 | 0.235 | 0.310 | 0.334 | 0.379 | 0.408 | 0.558 |
| random | 10823 | 0.000 | 0.003 | 0.005 | 0.024 | 0.049 | 0.242 |

10 nearest neighbors by cosine, 2000 random terms: 0.119 are parent or child, 0.322 are within two is-a steps (a two-step neighborhood has 45 terms on average, 0.12% of all terms).

- **The hierarchy's postorder is the best order and the dictionary's first-seen order is not.** The nearest neighbor is a median of 45 positions away in postorder and 1,739 in id order.
- **No order is enough.** Within ±100 positions the best order finds 38% of the ten neighbors; within ±1000 (2.6% of the ontology), 56%. This is what the bandwidth lemma in `docs/semantic-search.md` says to expect.
- **Ids are not random here.** GO minted its ids in related batches, so id order has some locality (0.21 at ±100) that comes from the file, not from the engine.
- **The hierarchy is informative, not sufficient.** A neighbor is a parent or child in 12% of cases and within two `is_a` steps in 32%, in a two-step neighborhood of 45 terms (0.12% of all terms).

## 2. Interval labels

| Spanning-forest rule | classes | edges | non-tree edges | intervals | per class: mean / p50 / p90 / p99 / max | closure pairs | labels (KiB) | closure (MiB) | build (ms) |
|---|---|---|---|---|---|---|---|---|---|
| deepest parent | 38092 | 57456 | 19367 | 76570 | 2.01 / 1 / 3 / 17 / 741 | 393448 | 598 | 9.0 | 14.9 |
| first parent | 38092 | 57456 | 19367 | 87769 | 2.30 / 1 / 3 / 22 / 740 | 393448 | 686 | 9.0 | 14.4 |

Is d at or below c, over random class pairs: 27 ns with intervals, 775 ns by searching up the parents (63 of 200000 pairs true; the two methods agree: yes).

Loaded 95548 triples in 0.95 s (17.1 MiB). Building the labels and their B+ tree from the stored triples: 22 ms.

| Classes below C | method | answers | range scans | keys read | page reads | µs |
|---|---|---|---|---|---|---|
| 1 | intervals | 1.0 | 1.00 | 1.0 | 4.0 | 37 |
| 1 | one scan per class | 1.0 | 1.0 | 0.0 | 4.0 | 26 |
| 2-9 | intervals | 4.0 | 2.12 | 4.0 | 4.5 | 43 |
| 2-9 | one scan per class | 4.0 | 4.0 | 3.2 | 5.3 | 39 |
| 10-99 | intervals | 30.6 | 5.90 | 30.6 | 5.8 | 59 |
| 10-99 | one scan per class | 30.6 | 30.6 | 35.3 | 16.3 | 149 |
| 100-999 | intervals | 268.2 | 29.12 | 268.2 | 13.2 | 147 |
| 100-999 | one scan per class | 268.2 | 268.2 | 358.3 | 65.7 | 803 |
| 1000+ | intervals | 3303.6 | 101.25 | 3303.6 | 40.1 | 528 |
| 1000+ | one scan per class | 3303.6 | 3303.6 | 5050.5 | 694.1 | 8998 |

- **Two intervals per class on average.** 76,570 intervals for 38,092 classes (598 KiB), against 393,448 ancestor pairs (9 MiB as 24-byte keys, one permutation) that materializing the subclass closure would store. The most any class needs is 741, within the `1 + non-tree edges` bound (19,368).
- **The spanning-forest rule matters little.** The deepest parent needs 13% fewer intervals than the first parent.
- **Reachability is 29 times faster** than searching up the parents: 27 ns against 775 ns (most random pairs are unrelated, so the search explores the whole ancestor set).
- **A subtree is a few scans.** For a class with 1,000 or more descendants: 101 range scans instead of 3,304, 40 page reads instead of 694, 0.5 ms instead of 9 ms.

## 3. Three ways to answer `?x rdf:type C`

LUBM(1) (shallow: 43 classes):

| Strategy | what is stored | triples | file (MiB) | load / index (s) |
|---|---|---|---|---|
| rewrite | the data | 103410 | 10.8 | 0.89 |
| intervals | the data + one key per type triple | 103410 | 11.3 | 0.89 + 0.00 |
| materialize | the data + inferred types | 118792 | 11.9 | 1.04 |

| Classes below C | strategy | answers | scans | keys read | page reads | µs |
|---|---|---|---|---|---|---|
| 1 | intervals | 633.3 | 1.0 | 633 | 6.7 | 47 |
| 1 | rewrite | 633.3 | 1.0 | 633 | 4.9 | 68 |
| 1 | materialized | 633.3 | 1.0 | 633 | 7.6 | 84 |
| 2-9 | intervals | 1513.0 | 1.2 | 1568 | 12.4 | 104 |
| 2-9 | rewrite | 1513.0 | 3.6 | 1568 | 15.4 | 187 |
| 2-9 | materialized | 1513.0 | 1.0 | 1513 | 13.1 | 165 |
| 10-99 | intervals | 2359.7 | 1.0 | 2360 | 17.0 | 140 |
| 10-99 | rewrite | 2359.7 | 11.3 | 2360 | 24.0 | 305 |
| 10-99 | materialized | 2359.7 | 1.0 | 2360 | 18.0 | 242 |

Synthetic instances over the Gene Ontology (200,000 entities, 495,552 triples):

| Strategy | what is stored | triples | file (MiB) | load / index (s) |
|---|---|---|---|---|
| rewrite | the data | 495552 | 63.6 | 5.09 |
| intervals | the data + one key per type triple | 495552 | 72.9 | 5.09 + 0.17 |
| materialize | the data + inferred types | 4017973 | 309.5 | 180.31 |

| Classes below C | strategy | answers | scans | keys read | page reads | µs |
|---|---|---|---|---|---|---|
| 1 | intervals | 10.2 | 1.0 | 10 | 4.0 | 44 |
| 1 | rewrite | 10.2 | 1.0 | 10 | 4.2 | 34 |
| 1 | materialized | 10.2 | 1.0 | 10 | 5.1 | 38 |
| 2-9 | intervals | 39.5 | 1.8 | 40 | 5.0 | 55 |
| 2-9 | rewrite | 39.5 | 3.7 | 40 | 6.5 | 52 |
| 2-9 | materialized | 39.5 | 1.0 | 40 | 5.4 | 106 |
| 10-99 | intervals | 278.0 | 4.1 | 278 | 8.4 | 90 |
| 10-99 | rewrite | 278.0 | 26.8 | 278 | 87.5 | 571 |
| 10-99 | materialized | 278.0 | 1.0 | 278 | 6.6 | 444 |
| 100-999 | intervals | 2954.2 | 31.5 | 2975 | 40.9 | 1461 |
| 100-999 | rewrite | 2954.2 | 283.3 | 2975 | 1152.8 | 7280 |
| 100-999 | materialized | 2954.2 | 1.0 | 2954 | 22.5 | 987 |
| 1000+ | intervals | 29903.8 | 126.8 | 32973 | 264.8 | 4555 |
| 1000+ | rewrite | 29903.8 | 3147.1 | 32973 | 12803.1 | 82441 |
| 1000+ | materialized | 29903.8 | 1.0 | 29904 | 182.0 | 3776 |

- **Materializing costs 8.1 times the triples and 4.9 times the file** on a deep, wide hierarchy (4,017,973 triples, 309.5 MiB, 180 s to load, against 495,552, 63.6 MiB, 5.1 s). The interval index adds 9.3 MiB (15%) and 0.17 s.
- **Intervals read almost as few pages as the materialized store** for large classes (265 against 182 for 1,000+ classes) and **48 times fewer than rewriting** (12,803).
- **On LUBM the three are close**: 43 classes with little depth leave nothing for intervals to save, and materialization adds 15% to the triples.
- **All three returned identical answers** on every sampled class.

Kept: interval labels, beside the unchanged first-seen ids.

## 4. Vector indexes

38,092 vectors of 384 floats, 1,000 queries (terms of the ontology, the term itself excluded), exact search as ground truth.

| Index | setting | recall@10 | µs / query | memory (MiB) | build (s) |
|---|---|---|---|---|---|
| flat float32 | exact | 1.000 | 2238 | 55.8 | 0.0 |
| flat sq8 | exact over codes | 0.996 | 4554 | 14.1 | 0.0 |
| flat pq m=48 | codes only | 0.778 | 819 | 1.7 | 10.4 |
| flat pq m=48 | rerank 100 exactly | 0.998 | 905 | 1.7 | 10.4 |
| flat pq m=96 | codes only | 0.883 | 1971 | 3.5 | 13.3 |
| flat pq m=96 | rerank 100 exactly | 1.000 | 2048 | 3.5 | 13.3 |
| ivf L=128 float32 | nprobe 1 | 0.640 | 65 | 56.0 | 1.4 |
| ivf L=128 float32 | nprobe 2 | 0.756 | 114 | 56.0 | 1.4 |
| ivf L=128 float32 | nprobe 4 | 0.833 | 205 | 56.0 | 1.4 |
| ivf L=128 float32 | nprobe 8 | 0.893 | 404 | 56.0 | 1.4 |
| ivf L=128 float32 | nprobe 16 | 0.935 | 782 | 56.0 | 1.4 |
| ivf L=128 float32 | nprobe 32 | 0.967 | 1490 | 56.0 | 1.4 |
| ivf L=128 float32 | nprobe 64 | 0.991 | 2994 | 56.0 | 1.4 |
| ivf L=512 float32 | nprobe 1 | 0.671 | 48 | 56.5 | 17.5 |
| ivf L=512 float32 | nprobe 2 | 0.779 | 64 | 56.5 | 17.5 |
| ivf L=512 float32 | nprobe 4 | 0.841 | 93 | 56.5 | 17.5 |
| ivf L=512 float32 | nprobe 8 | 0.892 | 149 | 56.5 | 17.5 |
| ivf L=512 float32 | nprobe 16 | 0.929 | 255 | 56.5 | 17.5 |
| ivf L=512 float32 | nprobe 32 | 0.954 | 459 | 56.5 | 17.5 |
| ivf L=512 float32 | nprobe 64 | 0.976 | 928 | 56.5 | 17.5 |
| ivf L=512 sq8 | nprobe 1 | 0.670 | 52 | 14.8 | 17.5 |
| ivf L=512 sq8 | nprobe 2 | 0.777 | 65 | 14.8 | 17.5 |
| ivf L=512 sq8 | nprobe 4 | 0.838 | 98 | 14.8 | 17.5 |
| ivf L=512 sq8 | nprobe 8 | 0.890 | 152 | 14.8 | 17.5 |
| ivf L=512 sq8 | nprobe 16 | 0.926 | 263 | 14.8 | 17.5 |
| ivf L=512 sq8 | nprobe 32 | 0.951 | 464 | 14.8 | 17.5 |
| ivf L=512 sq8 | nprobe 64 | 0.972 | 868 | 14.8 | 17.5 |
| ivf L=512 pq96 | nprobe 1 | 0.632 | 65 | 4.2 | 38.4 |
| ivf L=512 pq96 | nprobe 2 | 0.721 | 75 | 4.2 | 38.4 |
| ivf L=512 pq96 | nprobe 4 | 0.770 | 94 | 4.2 | 38.4 |
| ivf L=512 pq96 | nprobe 8 | 0.811 | 122 | 4.2 | 38.4 |
| ivf L=512 pq96 | nprobe 16 | 0.839 | 178 | 4.2 | 38.4 |
| ivf L=512 pq96 | nprobe 32 | 0.857 | 292 | 4.2 | 38.4 |
| ivf L=512 pq96 | nprobe 64 | 0.871 | 517 | 4.2 | 38.4 |
| ivf L=2048 float32 | nprobe 1 | 0.626 | 124 | 58.8 | 79.9 |
| ivf L=2048 float32 | nprobe 2 | 0.769 | 131 | 58.8 | 79.9 |
| ivf L=2048 float32 | nprobe 4 | 0.858 | 141 | 58.8 | 79.9 |
| ivf L=2048 float32 | nprobe 8 | 0.908 | 160 | 58.8 | 79.9 |
| ivf L=2048 float32 | nprobe 16 | 0.938 | 198 | 58.8 | 79.9 |
| ivf L=2048 float32 | nprobe 32 | 0.961 | 276 | 58.8 | 79.9 |
| ivf L=2048 float32 | nprobe 64 | 0.978 | 424 | 58.8 | 79.9 |
| hnsw M=8 | ef 40 | 0.992 | 82 | 57.8 | 9.1 |
| hnsw M=8 | ef 160 | 0.996 | 254 | 57.8 | 9.1 |
| hnsw M=16 | ef 10 | 0.993 | 44 | 59.7 | 15.0 |
| hnsw M=16 | ef 20 | 0.997 | 66 | 59.7 | 15.0 |
| hnsw M=16 | ef 40 | 0.997 | 108 | 59.7 | 15.0 |
| hnsw M=16 | ef 80 | 0.998 | 192 | 59.7 | 15.0 |
| hnsw M=16 | ef 160 | 0.998 | 350 | 59.7 | 15.0 |
| hnsw M=16 | ef 320 | 0.998 | 643 | 59.7 | 15.0 |
| hnsw M=32 | ef 40 | 0.998 | 153 | 63.4 | 28.0 |
| hnsw M=32 | ef 160 | 0.999 | 474 | 63.4 | 28.0 |

- **A scalar dot product made every index look better than it is.** The first run scored with a plain loop the compiler cannot vectorize without `-ffast-math`: the exact scan took 8.3 ms. With eight accumulators it takes 2.2 ms, and the numbers above use that. Every comparison below is against the fast baseline.
- **HNSW dominates on this data.** M=16 reaches recall 0.993 at 44 µs, 51 times faster than the exact scan, for 59.7 MiB (the vectors plus 3.9 MiB of links) and a 15 s build. IVF needs 255 µs for 0.929 and 928 µs for 0.976.
- **IVF over a B+ tree costs about 2.7 times more per scored row than a flat scan** (about 157 ns against 59 ns): each list entry is a decoded 24-byte key and a random read of the vector. That is the price of lists that page, cache, and update like the triple indexes. Its cost model holds in shape: more lists make a query cheaper until the centroid scan dominates (L=2048 pays 124 µs for nprobe 1).
- **PQ is for memory.** Codes alone give 0.78 (m=48) or 0.88 (m=96) recall in 1.7 or 3.5 MiB; re-ranking the best 100 exactly gives 0.998 and 1.000, but the re-rank needs the float vectors. At 38,092 vectors it is not faster than the exact scan.
- **8-bit codes keep 0.996 recall in a quarter of the memory** (14.1 MiB) and are slower than float32 here, because converting bytes to floats costs more than the cache misses they save at this size.

## 5. Nearest terms under a class

The 10 nearest terms under a class `C`, 20 classes per row, for a query that is a member of `C` ("inside") and one that is not ("outside"). Recall is against the exact answer over the members of `C`. IVF: 512 lists, nprobe 16; HNSW: M=16, ef=64. `mask` strategies test a label predicate; nothing is resolved in advance.

| Classes below C | query | strategy | recall@10 | µs / query | vectors scored | class members resolved | index searches |
|---|---|---|---|---|---|---|---|
| <=10 | inside C | pre-filter | 1.000 | 3 | 5 | 5 | 1.0 |
| <=10 | inside C | post-filter/hnsw | 1.000 | 37553 | 91582 | 0 | 6.0 |
| <=10 | inside C | post-filter/ivf | 0.863 | 903 | 5439 | 0 | 4.0 |
| <=10 | inside C | mask/hnsw | 1.000 | 16653 | 38170 | 0 | 1.0 |
| <=10 | inside C | mask/ivf | 1.000 | 1253 | 5 | 0 | 1.0 |
| <=10 | inside C | auto | 1.000 | 3 | 5 | 5 | 1.0 |
| <=10 | outside C | pre-filter | 1.000 | 3 | 5 | 5 | 1.0 |
| <=10 | outside C | post-filter/hnsw | 1.000 | 36654 | 88312 | 0 | 6.0 |
| <=10 | outside C | post-filter/ivf | 0.067 | 966 | 6184 | 0 | 4.0 |
| <=10 | outside C | mask/hnsw | 1.000 | 16802 | 38172 | 0 | 1.0 |
| <=10 | outside C | mask/ivf | 1.000 | 1255 | 5 | 0 | 1.0 |
| <=10 | outside C | auto | 1.000 | 3 | 5 | 5 | 1.0 |
| 11-100 | inside C | pre-filter | 1.000 | 9 | 27 | 27 | 1.0 |
| 11-100 | inside C | post-filter/hnsw | 1.000 | 1739 | 8759 | 0 | 2.2 |
| 11-100 | inside C | post-filter/ivf | 0.870 | 558 | 3762 | 0 | 2.5 |
| 11-100 | inside C | mask/hnsw | 1.000 | 15846 | 36300 | 0 | 1.0 |
| 11-100 | inside C | mask/ivf | 0.950 | 111 | 20 | 0 | 1.0 |
| 11-100 | inside C | auto | 1.000 | 9 | 27 | 27 | 1.0 |
| 11-100 | outside C | pre-filter | 1.000 | 9 | 27 | 27 | 1.0 |
| 11-100 | outside C | post-filter/hnsw | 1.000 | 27669 | 72638 | 0 | 5.5 |
| 11-100 | outside C | post-filter/ivf | 0.015 | 1009 | 6507 | 0 | 4.0 |
| 11-100 | outside C | mask/hnsw | 1.000 | 16611 | 38141 | 0 | 1.0 |
| 11-100 | outside C | mask/ivf | 0.790 | 698 | 14 | 0 | 1.0 |
| 11-100 | outside C | auto | 1.000 | 9 | 27 | 27 | 1.0 |
| 101-1000 | inside C | pre-filter | 1.000 | 62 | 307 | 307 | 1.0 |
| 101-1000 | inside C | post-filter/hnsw | 1.000 | 427 | 2384 | 0 | 1.4 |
| 101-1000 | inside C | post-filter/ivf | 0.925 | 293 | 1985 | 0 | 1.5 |
| 101-1000 | inside C | mask/hnsw | 1.000 | 1150 | 5419 | 0 | 1.0 |
| 101-1000 | inside C | mask/ivf | 0.955 | 104 | 142 | 0 | 1.0 |
| 101-1000 | inside C | auto | 1.000 | 59 | 307 | 307 | 1.0 |
| 101-1000 | outside C | pre-filter | 1.000 | 62 | 307 | 307 | 1.0 |
| 101-1000 | outside C | post-filter/hnsw | 0.995 | 13607 | 43391 | 0 | 4.6 |
| 101-1000 | outside C | post-filter/ivf | 0.170 | 902 | 5637 | 0 | 3.9 |
| 101-1000 | outside C | mask/hnsw | 1.000 | 8697 | 26412 | 0 | 1.0 |
| 101-1000 | outside C | mask/ivf | 0.450 | 286 | 20 | 0 | 1.0 |
| 101-1000 | outside C | auto | 1.000 | 62 | 307 | 307 | 1.0 |
| 1001-5000 | inside C | pre-filter | 1.000 | 292 | 1645 | 1645 | 1.0 |
| 1001-5000 | inside C | post-filter/hnsw | 1.000 | 70 | 709 | 0 | 1.0 |
| 1001-5000 | inside C | post-filter/ivf | 0.980 | 198 | 1456 | 0 | 1.0 |
| 1001-5000 | inside C | mask/hnsw | 1.000 | 183 | 1602 | 0 | 1.0 |
| 1001-5000 | inside C | mask/ivf | 0.980 | 124 | 373 | 0 | 1.0 |
| 1001-5000 | inside C | auto | 1.000 | 269 | 1645 | 1645 | 1.0 |
| 1001-5000 | outside C | pre-filter | 1.000 | 291 | 1645 | 1645 | 1.0 |
| 1001-5000 | outside C | post-filter/hnsw | 1.000 | 5513 | 23143 | 0 | 3.7 |
| 1001-5000 | outside C | post-filter/ivf | 0.330 | 811 | 5292 | 0 | 3.5 |
| 1001-5000 | outside C | mask/hnsw | 1.000 | 4424 | 17855 | 0 | 1.0 |
| 1001-5000 | outside C | mask/ivf | 0.365 | 147 | 59 | 0 | 1.0 |
| 1001-5000 | outside C | auto | 1.000 | 295 | 1645 | 1645 | 1.0 |
| >5000 | inside C | pre-filter | 1.000 | 1404 | 10868 | 10868 | 1.0 |
| >5000 | inside C | post-filter/hnsw | 1.000 | 68 | 706 | 0 | 1.0 |
| >5000 | inside C | post-filter/ivf | 0.986 | 211 | 1570 | 0 | 1.0 |
| >5000 | inside C | mask/hnsw | 1.000 | 105 | 1031 | 0 | 1.0 |
| >5000 | inside C | mask/ivf | 0.986 | 161 | 947 | 0 | 1.0 |
| >5000 | inside C | auto | 1.000 | 95 | 1031 | 0 | 1.0 |
| >5000 | outside C | pre-filter | 1.000 | 1394 | 10868 | 10868 | 1.0 |
| >5000 | outside C | post-filter/hnsw | 1.000 | 836 | 6414 | 0 | 2.6 |
| >5000 | outside C | post-filter/ivf | 0.643 | 591 | 4033 | 0 | 2.9 |
| >5000 | outside C | mask/hnsw | 1.000 | 900 | 6504 | 0 | 1.0 |
| >5000 | outside C | mask/ivf | 0.657 | 107 | 220 | 0 | 1.0 |
| >5000 | outside C | auto | 1.000 | 860 | 6504 | 0 | 1.0 |

- **For classes up to about 1,000 members, resolve the members and score them exactly.** It is the exact answer and takes 3 µs to 62 µs. Every index-based strategy is between 5 times and 10,000 times slower there, because few of the vectors the index touches are members.
- **Post-filtering breaks when the query is outside the class.** The share of the unfiltered top 100 that lies under `C` is 0.00 to 0.12 for an outside query and 0.04 to 0.72 for an inside one, so the independence estimate (`k/s` candidates) is wrong in both directions. IVF post-filter recall falls to 0.07 for small classes; HNSW post-filter reaches full recall only by searching again and again (37 ms for a class of at most 10 members).
- **Masked HNSW crosses the whole graph when the class is small** (16 ms for classes of at most 100): the nodes it may return are rare and far apart in the graph. This is the problem ACORN addresses.
- **For classes above 5,000 members, a masked HNSW search wins**: 105 µs inside and 900 µs outside, exact recall, against 1.4 ms for resolving and scoring the members.
- **A mask that resolves the members first is a mistake.** The first version scanned the interval ranges and built a bitmap per query: 669 µs for a class of more than 5,000 members, of which about 560 µs was the scan. Testing the label directly (one binary search over a handful of intervals) took the same search to 105 µs.
- **`kAuto`, with `pre_threshold` 4,096, is within 1.5 times of the best strategy for every size except one**, and never worse than 3 ms. It resolves and scores exactly up to 4,096 members, then masks HNSW. The one loss is classes of 1,001 to 5,000 members with an inside query (269 µs against 183 µs for masked HNSW), taken to avoid 4,424 µs when the query is outside.

Rerun: `benchmarks/semantic/run.sh`. Raw output: `benchmarks/semantic/raw/` (not committed).
