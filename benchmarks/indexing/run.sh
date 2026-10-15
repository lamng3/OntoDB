#!/usr/bin/env bash
# Which permutations pay for themselves? Writes results to stdout as Markdown.
#   scripts/fetch-data lubm && benchmarks/indexing/run.sh > benchmarks/indexing/raw/run.md
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
cmake --preset release >/dev/null
cmake --build --preset release --target ontodb_bench_indexing >/dev/null
mkdir -p benchmarks/indexing/raw
./build/release/ontodb_bench_indexing benchmarks/indexing/raw/work data/downloads/lubm/*.owl
