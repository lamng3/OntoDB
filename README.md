# OntoDB

A database management system for ontologies. Data is RDF. The query language is SPARQL.

OntoDB loads an ontology, stores each triple as three dictionary ids, and answers a query with a planner, an optimizer, and a B+ tree. A commit is a write-ahead log. A restart is ARIES.

## Features

- **SPARQL.** Basic graph patterns, filters, and `INSERT DATA` / `DELETE DATA`. Turtle, N-Triples, and RDF/XML load as triples. OWL is stored as RDF.
- **Indexes.** Subject, predicate, and object are 64-bit ids. A triple is a 24-byte key in a B+ tree, kept in more than one order so a bound position is a range scan.
- **Query engine.** Volcano operators, nested-loop join, hash join, index nested-loop join, and an optimizer that picks the join order and the algorithm.
- **Storage and recovery.** A buffer pool with LRU-K, strict two-phase locking, write-ahead logging, and ARIES.

The `ontodb` shell runs that SPARQL subset on an in-memory store today. The index, the optimizer, and recovery are the rest of the system. `PLAN.md` is the build list.

## Run

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

## Query

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

Study notes: <https://lamng3.github.io/ontodb-docs/>. One query, followed through the code: `docs/architecture.md`.
