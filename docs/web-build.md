# The web build

OntoDB compiles to WebAssembly with Emscripten. The playground in the manual runs that build: the same planner, executors, B+ trees, buffer pool and semantic-search code as the native shell, in the browser.

```sh
git clone https://github.com/emscripten-core/emsdk ../emsdk      # needs Python 3.10 or later
(cd ../emsdk && ./emsdk install latest && ./emsdk activate latest)
web/build.sh          # writes web/out/ontodb_wasm.{js,wasm}, about 0.85 MB of wasm
node web/smoke.mjs    # loads Pizza, runs a query, switches to the B+ tree backend
```

`web/ontodb_wasm.cpp` exports three functions. `ontodb_query(sparql)` returns a SPARQL result as JSON, `ontodb_exec(line)` runs a statement or a `\` command and returns the text the shell prints, and `ontodb_reset()` starts an empty database.

## What had to change

- **Raptor.** The native loader uses the raptor2 C library, which the web build does not link. `src/loader/turtle_parser.cpp` parses Turtle and N-Triples (prefixes, `a`, predicate and object lists, blank-node property lists, collections, long strings, escapes, language tags, datatypes, numbers, booleans, `@base`). A test checks that it produces the same triples as raptor on `tiny.ttl` and `pizza.ttl`. RDF/XML stays native-only.
- **Threads.** The browser build is single-threaded. The disk scheduler runs each request inside `Schedule`, which keeps its ordering and promise contract; the deadlock detector and the shell's session workers are not started. A `\` command or statement runs on the calling thread through `Shell::Evaluate`.
- **`PAGE_SIZE`.** musl's `<limits.h>` defines a macro of that name, so `config.h` undefines it before declaring the constant.

## What is not there

No write-ahead log, no recovery, no disk-backed database (`Database::Open`), and no concurrency: one session. The playground keeps your updates in a journal in browser storage and replays them, which is persistence, not durability. A page in memory is not the same as a page on a disk that can lose power, so the recovery experiments belong to the native build.

## Not used: WebGPU

Search is a 384-float dot product per candidate. HNSW touches a few hundred vectors per query, and an exact scan of the playground's 834 vectors takes about half a millisecond, so a GPU round trip would cost more than the work. The wasm build enables 128-bit SIMD (`-msimd128`), which the dot product's eight accumulators vectorize. A GPU would matter for exact scans over millions of vectors, which this engine does not hold in memory.
