# ontodb

ontodb is a standalone RDF/SPARQL database for learning storage, query execution, and recovery. Terms are dictionary-encoded, and triples are the records.

The memory backend loads Turtle and answers a read subset plus `INSERT DATA` and `DELETE DATA`. Everything else is a stub that throws `NotImplementedException` with a plan id. `PLAN.md` is the build order. `docs/architecture.md` is the map of one query and one insert.

Study notes for each component — the SPARQL engine, the optimizer, the B+ tree, write-ahead logging, and ARIES — are at <https://lamng3.github.io/ontodb-docs/>. They are meant to be read on a phone.

## Codespaces

Open the repository in a GitHub Codespace or a VS Code Dev Container. The image already has clang, CMake, Ninja, gdb, raptor2, and graphviz. When it finishes:

```bash
./build/dev/ontodb
\load data/tiny.ttl
```

`scripts/check 0.1` should pass with no other setup.

## Ubuntu

```bash
sudo apt-get update && sudo apt-get install -y clang cmake ninja-build pkg-config libraptor2-dev graphviz
cmake --preset dev && cmake --build --preset dev
./build/dev/ontodb
```

## macOS

```bash
brew install pkgconf raptor cmake ninja
cmake --preset dev && cmake --build --preset dev
./build/dev/ontodb
```

Presets: `dev` (Debug, ASan, UBSan), `tsan`, `release`.

## Shell

```text
\load data/tiny.ttl
SELECT ?name WHERE { ?s <http://example.edu/univ#name> ?name }
\explain SELECT ?s WHERE { ?s <http://ex/p> <http://ex/o> }
\stats
\set pool_size 8
\help
\quit
```

`\bpm`, `\tree`, `\page`, `\trace`, `\begin`, `\commit`, `\abort`, `\txns`, `\locks`, `\log`, `\checkpoint`, `\crash`, and `\set backend indexed` print `not built yet (PLAN x.y)`.

## Checks

```bash
scripts/check 0.1          # orientation, must pass
scripts/check 1.3          # one item, including its disabled tests
scripts/check 7            # phase 7, under TSan
scripts/status             # progress table; starts at 0%
scripts/crashtest --seeds 4
scripts/fetch-data pizza
scripts/fetch-data lubm    # needs Java; writes data/downloads/lubm/University0_0.owl
```

A plan item counts as done when you delete the `DISABLED_` prefix and the tests pass. Until then `scripts/status` stays at 0%.

Phase 9 in `PLAN.md` is open design. There are no stubs for it.
