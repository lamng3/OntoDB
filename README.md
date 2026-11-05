# OntoDB

[![CI](https://github.com/lamng3/OntoDB/actions/workflows/ci.yml/badge.svg)](https://github.com/lamng3/OntoDB/actions/workflows/ci.yml)
[![License: BSD 3-Clause](https://img.shields.io/badge/license-BSD--3--Clause-green.svg)](LICENSE)

A database management system for ontologies. Data is RDF and the query language is SPARQL. The shell answers a SPARQL subset over B+ tree indexes with a cost-based optimizer, runs statements under strict two-phase locking, and recovers pages with ARIES after a crash. It also does semantic search: a class hierarchy becomes integer intervals in a B+ tree, and vectors are searched by exact, IVF, or HNSW indexes, optionally limited to the terms under a class.

## Build

```bash
# Ubuntu
sudo apt-get install -y clang cmake ninja-build pkg-config libraptor2-dev
# macOS
brew install pkgconf raptor cmake ninja

cmake --preset dev && cmake --build --preset dev
```

Presets are `dev` (AddressSanitizer and UndefinedBehaviorSanitizer), `tsan`, and `release`. `./build/dev/ontodb_test` runs the tests. Every push runs the suite under ASan/UBSan and ThreadSanitizer, then checks formatting and clang-tidy.

## Run

```text
$ ./build/dev/ontodb mydb
OntoDB> \load data/tiny.owl
loaded 14 triples in 5.2 ms
OntoDB> SELECT ?kind WHERE { ?kind <http://www.w3.org/2000/01/rdf-schema#subClassOf> <http://example.org/materials#Material> . }
?kind
<http://example.org/materials#Metal>
<http://example.org/materials#Polymer>
(2 rows)
OntoDB> \verify
verify: ok
```

Pass a directory (`ontodb mydb`) to keep the database on disk: every update statement and every `\load` is a transaction, forced to a checksummed write-ahead log before the prompt returns, and reopening the directory replays the committed ones. Without a directory the shell is an in-memory database. A query may span lines and runs when its braces close; `\help` lists the commands and `\explain` prints the plan.

## Documentation

- **Manual** — <https://lamng3.com/ontodb/> (installation, tutorial, shell and SPARQL reference, internals), with an in-browser [playground](https://lamng3.com/ontodb/playground.html).
- **Design notes and roadmap** — the [project wiki](https://github.com/lamng3/OntoDB/wiki).

The book, *OntoDB: Database Internals by Example* ([PDF](docs/ontodb-book.pdf)), and two papers — [a fast and persistent query engine](docs/ontodb-query-engine.pdf) and [hierarchy-filtered vector search](docs/ontodb-vector-search.pdf) — are in [`docs/`](docs/).

## License

[BSD 3-Clause](LICENSE). Copyright (c) 2026 Lam Nguyen.
