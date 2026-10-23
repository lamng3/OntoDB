#!/usr/bin/env bash
# OntoDB against Oxigraph. Needs LUBM(1) and LUBM(10) as RDF/XML, the Gene Ontology and the
# synthetic entities of the semantic benchmark, Rust (cargo), and Python 3 with pyoxigraph:
#   scripts/fetch-data lubm go
#   java -cp data/downloads/lubm/uba/classes edu.lehigh.swat.bench.uba.Generator -univ 10 -index 0 \
#        -seed 0 -onto http://www.lehigh.edu/~zhp2/2004/0401/univ-bench.owl   # in data/downloads/lubm10
#   python3 -m venv benchmarks/compare/.venv && benchmarks/compare/.venv/bin/pip install pyoxigraph==0.5.13
#   benchmarks/semantic/run.sh        # writes the entities and classes.tsv
#   benchmarks/compare/run.sh
# Everything lands in benchmarks/compare/raw; results.md is the table the paper cites.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
cmake --preset release >/dev/null
cmake --build --preset release --target ontodb_bench_compare >/dev/null
(cd benchmarks/compare/oxigraph && cargo build --release 2>&1 | tail -1)
cd benchmarks/compare
raw=raw
mkdir -p "$raw"
py=.venv/bin/python
ox=oxigraph/target/release/oxigraph-bench
od=../../build/release/ontodb_bench_compare
for set in lubm1:lubm:9 lubm10:lubm10:5; do
  IFS=: read -r name dir runs <<<"$set"
  files=(../../data/downloads/$dir/University*.owl)
  $py prepare.py ../../data/downloads/$dir $raw/queries-$name.tsv
  $od $raw/work-ontodb-$name $raw/queries-$name.tsv $runs "${files[@]}" > $raw/ontodb-$name.tsv
  $ox rocksdb $raw/work-oxi-$name $raw/queries-$name.tsv $runs "${files[@]}" > $raw/oxigraph-$name.tsv
  $ox memory $raw/work-oxi-$name $raw/queries-$name.tsv $runs "${files[@]}" > $raw/oxigraph-mem-$name.tsv
done
cp ../semantic/raw/work-gotypes/classes.tsv $raw/classes.tsv
for mode in rocksdb memory; do
  $ox $mode $raw/work-oxi-go types $raw/classes.tsv 5 ../../data/downloads/go/go.nt ../semantic/raw/work-gotypes/entities.nt > $raw/oxigraph-$mode-types.tsv
done
python3 summarize.py $raw/ontodb-lubm10.tsv $raw/oxigraph-lubm10.tsv $raw/oxigraph-mem-lubm10.tsv > $raw/summary-lubm10.md
python3 summarize.py $raw/ontodb-lubm1.tsv $raw/oxigraph-lubm1.tsv $raw/oxigraph-mem-lubm1.tsv > $raw/summary-lubm1.md
python3 summarize_types.py $raw/oxigraph-rocksdb-types.tsv $raw/oxigraph-memory-types.tsv > $raw/summary-types.md
cat $raw/summary-*.md
