# Question: how does OntoDB compare with another triple store?

**What is compared.** OntoDB (indexed store: SPO, POS, OSP B+ trees, 256 MiB buffer pool, `auto` joins), Oxigraph 0.5.13 through its Rust API (RocksDB on disk, and its in-memory store), Apache Jena TDB2 5.2.0 (Java 17), and SQLite 3.51 as a triple table with three indexes. Both run in-process, on one thread, with warm caches, on the same files and the same query text.

**Data.** LUBM(1) (103,104 statements) and LUBM(10) (1,316,700), RDF/XML as the generator writes it; the Gene Ontology (38,092 classes) with 200,000 synthetic entities of one to three types each (495,552 triples).

**Workloads.**
- Load: bulk loader, wall time, file size, peak RSS.
- The 14 LUBM queries, no inference.
- 100 single-pattern queries per shape (`s--`, `sp-`, `--o`, `-po`) with constants sampled from the data, and every predicate for `-p-`.
- Instances of a class and its subclasses for 30 classes in each of five subtree-size buckets: OntoDB by interval scans, Oxigraph by `?x a ?c . ?c rdfs:subClassOf* <C>`.

- Concurrency: one to eight threads of lookups, of inserts, and of readers beside one writer, three seconds each.

**Metric.** Median of 5 to 9 timed runs after one warm run; rows are decoded to strings in both engines. A query is parsed once outside the timer in both. Row counts must agree; the scripts stop or print a warning when they do not.

**What this cannot show.** OntoDB's SPARQL is a subset; Oxigraph implements SPARQL 1.1. Ten LUBM queries return no rows without inference, so their times are not compared. Oxigraph's plans were not inspected. RDF4J, QLever, and RDFox were not run.
