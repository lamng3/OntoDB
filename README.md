<div align="center">

# 🗄️ OntoDB

**A database management system for ontologies**

[![CI](https://github.com/lamng3/OntoDB/actions/workflows/ci.yml/badge.svg)](https://github.com/lamng3/OntoDB/actions/workflows/ci.yml)
[![Manual](https://img.shields.io/badge/docs-manual-8b5cf6)](https://lamng3.github.io/ontodb-docs/)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-3b82f6)](CMakeLists.txt)
[![Progress](https://img.shields.io/badge/plan-10%2F53-f59e0b)](PLAN.md)
[![License: CC BY 4.0](https://img.shields.io/badge/license-CC%20BY%204.0-green.svg)](https://creativecommons.org/licenses/by/4.0/)

[⚡ Quick start](#-quick-start) · [✨ What it is](#-what-it-is) · [📊 Status](#-status) · [🔧 Usage](#-usage) · [📖 Manual](https://lamng3.github.io/ontodb-docs/) · [📚 Book](book/ontodb-book.pdf) · [📝 Citation](#-citation)

</div>

<p align="center"><b>Lam Nguyen</b></p>

> [!NOTE]
> **Data is RDF. The query language is SPARQL.** The shell answers a SPARQL
> subset today and keeps databases on disk with a checksummed write-ahead log;
> the indexes, the optimizer, and page-level recovery are being built.

## ⚡ Quick start

```bash
brew install pkgconf raptor cmake ninja        # Ubuntu: see Usage
cmake --preset dev && cmake --build --preset dev
./build/dev/ontodb mydb
```

```text
OntoDB> \load data/tiny.owl
loaded 14 triples in 5.2 ms
OntoDB> SELECT ?kind WHERE { ?kind <http://www.w3.org/2000/01/rdf-schema#subClassOf> <http://example.org/materials#Material> . }
?kind
<http://example.org/materials#Metal>
<http://example.org/materials#Polymer>
(2 rows)
OntoDB> \verify
...
verify: ok
```

Start `./build/dev/ontodb mydb` again and the data is there.

## ✨ What it is

OntoDB loads an ontology, stores each triple as three dictionary ids, and
answers a query with a planner, an optimizer, and a B+ tree. A commit is a
write-ahead log. A restart is ARIES.

- **SPARQL.** Basic graph patterns, filters, and `INSERT DATA` / `DELETE DATA`. Turtle, N-Triples, and RDF/XML load as triples. OWL is stored as RDF.
- **Persistent databases.** `ontodb <dir>` makes every update statement and every `\load` a transaction, forced to a checksummed log before the prompt returns. `\checkpoint` writes a snapshot, and `\verify` rebuilds from the files and compares.
- **Indexes.** Subject, predicate, and object are 64-bit ids. A triple is a 24-byte key in a B+ tree, kept in more than one order so a bound position is a range scan.
- **Query engine.** Volcano operators, nested-loop join, hash join, index nested-loop join, and an optimizer that picks the join order and the algorithm.
- **Storage and recovery.** A buffer pool with LRU-K, strict two-phase locking, write-ahead logging, and ARIES.

## 📊 Status

<div align="center">

| Feature | Status |
|:--|:--:|
| SPARQL shell | ✅ |
| Persistent databases (snapshot, WAL, recovery modes, verify) | ✅ |
| Log records and group commit | ✅ |
| Triple key | ✅ |
| Buffer pool, B+ tree | 🚧 |
| RDF storage, query execution, optimizer, bulk load | 🚧 |
| Concurrency, page-level recovery | 🚧 |

<sub>10 of 53 slices pass · `scripts/status` · the build list is [PLAN.md](PLAN.md)</sub>

</div>

Every push runs the test suite under AddressSanitizer and UndefinedBehaviorSanitizer and again under ThreadSanitizer, then checks formatting and clang-tidy.

## 🔧 Usage

<details>
<summary><b>Build on Ubuntu or macOS</b></summary>

Ubuntu:

```bash
sudo apt-get update && sudo apt-get install -y clang cmake ninja-build pkg-config libraptor2-dev
cmake --preset dev && cmake --build --preset dev
./build/dev/ontodb
```

macOS:

```bash
brew install pkgconf raptor cmake ninja
cmake --preset dev && cmake --build --preset dev
./build/dev/ontodb
```

Presets are `dev` (ASan and UBSan), `tsan`, and `release`. `./build/dev/ontodb_test` runs the tests.

</details>

<details>
<summary><b>Query</b></summary>

At the `OntoDB>` prompt, a query may span lines. It runs when the braces close.

```text
\load data/tiny.ttl
PREFIX ub: <http://example.edu/univ#>
SELECT ?name WHERE {
  ?s a ub:Student ; ub:name ?name .
}
\quit
```

The load reports 53 triples. The names are Alice, Bob, and Cara. `\help` lists the commands. `\explain` prints the plan.

</details>

<details>
<summary><b>Keep a database on disk</b></summary>

Give the shell a directory. Every update statement and every `\load` is a transaction, and the commit is on disk before the prompt returns.

```text
./build/dev/ontodb mydb
\load data/tiny.owl
INSERT DATA { <http://example.org/materials#Ceramic> <http://www.w3.org/2000/01/rdf-schema#subClassOf> <http://example.org/materials#Material> . }
\checkpoint
\verify
\quit
```

`\log` prints the last log records, `\checkpoint` writes a snapshot and empties the log, and `\verify` rebuilds the database from the files and compares. If the log ends in a torn or corrupt record, the shell keeps every transaction before it and says what it dropped; `ontodb --strict mydb` refuses to open instead.

</details>

<details>
<summary><b>Development</b></summary>

```bash
scripts/check durable        # build and run one feature, print the next slice
scripts/status               # every slice and the progress
scripts/crashtest --seeds 4  # the crash harness
```

One query and one committed statement, followed through the code: [docs/architecture.md](docs/architecture.md), which also lists the design patterns the code uses. Benchmarks go in `benchmarks/`, and the writeup that cites them in `writeups/`.

</details>

The book, [*OntoDB: Database Internals by Example*](book/ontodb-book.pdf), explains the theory behind each part: pages and the buffer pool, B+ trees and RDF indexing, query plans and joins, transactions and locking, the write-ahead log, ARIES, and durability.

The [manual](https://lamng3.github.io/ontodb-docs/) covers installation, a tutorial, the shell and SPARQL references, persistent databases, file formats, troubleshooting, and internals.

## 📝 Citation

```bibtex
@misc{nguyen2026ontodb,
  title  = {OntoDB: A Database Management System for Ontologies},
  author = {Nguyen, Lam},
  year   = {2026},
  url    = {https://github.com/lamng3/OntoDB}
}
```

## License

[CC BY 4.0](LICENSE). Copyright (c) 2026 Lam Nguyen.
