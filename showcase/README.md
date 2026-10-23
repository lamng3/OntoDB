# Showcase

What the OntoDB shell printed, as a single page that needs no server. It is drawn in the style of [art-visualization](https://lamng3.com/art-visualization/), with the same base as the Agentic Ontologist and Aegis showcases.

- **Walkthrough.** LUBM query 1 on one university, recorded with `\trace on` and stepped one engine action at a time. The steps are the binder's dictionary lookups, the statistics pass the first query after a load makes over every SPO leaf, the plan, the POS range scan, and the SPO probes and id-to-string decodes of the index nested-loop join. Beside them are the three B+ trees with the pages each step fetched, the leaf's keys decoded to terms, and the 64 buffer-pool frames with hits, misses and LRU-K evictions.
- **Plans.** The fourteen LUBM queries under `\explain analyze`, with estimated and actual rows per operator.
- **Crash and restart.** A database directory through a load, inserts, a checkpoint and a delete. Then `\crash`, a log tail cut by 10 bytes, the restart that drops the torn transaction, and `\verify`.
- **Hierarchy search.** `\under` and `\similar … under …` on the Gene Ontology slice.
- **Against other stores.** The tables in `benchmarks/compare/results.md`, drawn.

Each key on the page is checked against the engine before it is written. `record.py` rebuilds every leaf from the logged triples and asserts that its first and last keys match `\tree`. It also asserts that the scan's matched keys match the rows the query returned. Term ids come from the write-ahead log of a persistent load of the same file.

```bash
scripts/fetch-data lubm && scripts/web-slice    # LUBM(1) and the GO slice
cmake --preset release && cmake --build --preset release
python3 showcase/record.py                      # runs build/release/ontodb, writes showcase/recording.json
python3 showcase/build.py                       # writes showcase/index.html
```

`base.css` and `base.js` are copies of the shared showcase base in `memcommit-docs/showcase`.
