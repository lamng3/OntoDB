#!/usr/bin/env bash
# Join order and join operator on LUBM(1). Writes a Markdown table to stdout.
#   scripts/fetch-data lubm && benchmarks/joins/run.sh > benchmarks/joins/raw/run.md
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
cmake --preset release >/dev/null
cmake --build --preset release --target ontodb_bench_joins >/dev/null
mkdir -p benchmarks/joins/raw
./build/release/ontodb_bench_joins benchmarks/joins/raw/work 20 data/lubm/queries data/downloads/lubm/*.owl
