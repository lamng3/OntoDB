# OntoDB

A database management system for ontologies. Data is RDF. The query language is SPARQL.

OntoDB loads an ontology, stores each triple as three dictionary ids, and answers a query with a planner, an optimizer, and a B+ tree. A commit is a write-ahead log. A restart is ARIES.

## Features

- **SPARQL.** Basic graph patterns, filters, and `INSERT DATA` / `DELETE DATA`. Turtle, N-Triples, and RDF/XML load as triples. OWL is stored as RDF.
- **Indexes.** Subject, predicate, and object are 64-bit ids. A triple is a 24-byte key in a B+ tree, kept in more than one order so a bound position is a range scan.
- **Query engine.** Volcano operators, nested-loop join, hash join, index nested-loop join, and an optimizer that picks the join order and the algorithm.
- **Storage and recovery.** A buffer pool with LRU-K, strict two-phase locking, write-ahead logging, and ARIES.

The `ontodb` shell runs that SPARQL subset on an in-memory store today, and `ontodb <dir>` makes that store durable with a snapshot and a write-ahead log. The index, the optimizer, and page-level recovery are the rest of the system. `PLAN.md` is the build list.

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

## Keep a database on disk

Give the shell a directory. Every update statement and every `\load` is a transaction, and the commit is on disk before the prompt returns.

```text
./build/dev/ontodb mydb
\load data/tiny.owl
INSERT DATA { <http://example.org/materials#Ceramic> <http://www.w3.org/2000/01/rdf-schema#subClassOf> <http://example.org/materials#Material> . }
\checkpoint
\verify
\quit
```

Start `./build/dev/ontodb mydb` again and the data is there. `\log` prints the last log records, `\checkpoint` writes a snapshot and empties the log, and `\verify` rebuilds the database from the files and compares. If the log ends in a torn or corrupt record, the shell keeps every transaction before it and says what it dropped; `ontodb --strict mydb` refuses to open instead.

Manual: <https://lamng3.github.io/ontodb-docs/>. One query, followed through the code: `docs/architecture.md`.
