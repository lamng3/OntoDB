#!/usr/bin/env bash
# Hierarchy-filtered vector search on four ontologies, against FAISS and hnswlib.
#   scripts/fetch-data go hpo uberon chebi     (one at a time)
#   scripts/embed-terms data/downloads/<o>/<o>.terms.tsv data/downloads/<o>/<o>.ovec
#   scripts/embed-terms data/downloads/<o>/<o>.synonyms.sample.tsv data/downloads/<o>/<o>.syn.ovec
#   python3 -m venv benchmarks/vector/.venv && benchmarks/vector/.venv/bin/pip install faiss-cpu hnswlib numpy
#   benchmarks/vector/run.sh
# Besides the four ontologies, the robustness runs: a second embedding model on GO and HPO
# (scripts/embed-terms ... sentence-transformers/all-mpnet-base-v2 -> <o>.mpnet.ovec, <o>.syn.mpnet.ovec; run the same
# filtered, index, and faiss_baseline commands with the name <o>-mpnet), and VEC_K=1 and VEC_K=50 on GO (names go-k1, go-k50).
# The relational baseline: duckdb_baseline.py; why a selector-IVF loses recall: faiss_baseline.py mechanism.
# Everything lands in benchmarks/vector/raw.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
cmake --preset release >/dev/null
cmake --build --preset release --target ontodb_bench_vector ontodb_bench_semantic >/dev/null
raw=benchmarks/vector/raw
mkdir -p "$raw"
py=benchmarks/vector/.venv/bin/python
for o in ${ONTOLOGIES:-go hpo uberon chebi}; do
  d=data/downloads/$o
  ./build/release/ontodb_bench_semantic order $d/$o.nt $d/$o.ovec > $raw/$o-order.md
  ./build/release/ontodb_bench_vector filtered $o $d/$o.nt $d/$o.ovec $d/$o.syn.ovec $raw
  ./build/release/ontodb_bench_vector index $o $d/$o.nt $d/$o.ovec $raw
  $py benchmarks/vector/faiss_baseline.py filtered $o $d/$o.nt $d/$o.ovec $d/$o.syn.ovec $raw/$o-workload.tsv $raw
  $py benchmarks/vector/faiss_baseline.py index $o $d/$o.ovec $raw
  $py benchmarks/vector/duckdb_baseline.py $o $d/$o.nt $d/$o.ovec $d/$o.syn.ovec $raw/$o-workload.tsv $raw
  $py benchmarks/vector/faiss_baseline.py mechanism $o $d/$o.nt $d/$o.ovec $d/$o.syn.ovec $raw/$o-workload.tsv $raw
  $py benchmarks/vector/faiss_baseline.py scale $o $d/$o.nt $d/$o.ovec $d/$o.syn.ovec $raw/$o-workload.tsv $raw
done
