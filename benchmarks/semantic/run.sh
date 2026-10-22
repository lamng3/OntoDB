#!/usr/bin/env bash
# Semantic search measurements. Needs the Gene Ontology, its embeddings, and LUBM(1):
#   scripts/fetch-data go && scripts/embed-terms data/downloads/go/go.terms.tsv data/downloads/go/go.ovec
#   scripts/fetch-data lubm && scripts/fetch-data lubm-ontology
#   benchmarks/semantic/run.sh
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
cmake --preset release >/dev/null
cmake --build --preset release --target ontodb_bench_semantic >/dev/null
raw=benchmarks/semantic/raw
mkdir -p "$raw"
bench=./build/release/ontodb_bench_semantic
go=data/downloads/go
$bench order "$go/go.nt" "$go/go.ovec" > "$raw/order.md"
$bench labels "$raw/work" "$go/go.nt" > "$raw/labels.md"
$bench types "$raw/work-lubm" lubm data/downloads/lubm/univ-bench.owl data/downloads/lubm/University*.owl > "$raw/types-lubm.md"
$bench types "$raw/work-gotypes" go "$go/go.nt" 200000 > "$raw/types-go.md"
$bench knn "$go/go.nt" "$go/go.ovec" > "$raw/knn.md"
$bench hybrid "$go/go.nt" "$go/go.ovec" > "$raw/hybrid.md"
cat "$raw"/*.md
