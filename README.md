# ontodb

A SPARQL database. Triples are the records. The shell is how you load a file and run a query.

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

At the `ontodb>` prompt, load the sample graph and ask for student names. A query can span lines. It runs when the `{ }` group closes. `\quit` leaves.

```text
\load data/tiny.ttl
PREFIX ub: <http://example.edu/univ#>
SELECT ?name WHERE {
  ?s a ub:Student ; ub:name ?name .
}
\quit
```

That load reports 53 triples. The result is `Alice`, `Bob`, and `Cara`.

The same query on one line also works:

```text
SELECT ?name WHERE { ?s a <http://example.edu/univ#Student> ; <http://example.edu/univ#name> ?name . }
```

`\help` lists every command. `\explain` prints the plan and does not run it. Storage, transactions, and recovery commands answer `not built yet` until that feature exists.

`PLAN.md` is the feature list. `benchmarks/` is where a later comparison of eviction, indexing, joins, or recovery is written down. `docs/architecture.md` follows one query and one insert through the code.
