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
(cd benchmarks/compare/jena && mvn -q -DskipTests package dependency:build-classpath -Dmdep.outputFile=cp.txt)
cd benchmarks/compare
raw=raw
mkdir -p "$raw"
py=.venv/bin/python
ox=oxigraph/target/release/oxigraph-bench
od=../../build/release/ontodb_bench_compare
jena() { java -Xmx4g -cp "jena/target/jena-bench-0.1.0.jar:$(cat jena/cp.txt)" Bench "$@"; }
for set in lubm1:lubm:9 lubm10:lubm10:5; do
  IFS=: read -r name dir runs <<<"$set"
  files=(../../data/downloads/$dir/University*.owl)
  $py prepare.py ../../data/downloads/$dir $raw/queries-$name.tsv
  $od $raw/work-ontodb-$name $raw/queries-$name.tsv $runs "${files[@]}" > $raw/ontodb-$name.tsv
  $ox rocksdb $raw/work-oxi-$name $raw/queries-$name.tsv $runs "${files[@]}" > $raw/oxigraph-$name.tsv
  $ox memory $raw/work-oxi-$name $raw/queries-$name.tsv $runs "${files[@]}" > $raw/oxigraph-mem-$name.tsv
  jena $raw/work-jena-$name $raw/queries-$name.tsv $runs "${files[@]}" > $raw/jena-$name.tsv 2>/dev/null
  $py sqlite_bench.py $raw/work-sql-$name $raw/queries-$name.tsv $runs "${files[@]}" > $raw/sqlite-$name.tsv
done
cp ../semantic/raw/work-gotypes/classes.tsv $raw/classes.tsv
for mode in rocksdb memory; do
  $ox $mode $raw/work-oxi-go types $raw/classes.tsv 5 ../../data/downloads/go/go.nt ../semantic/raw/work-gotypes/entities.nt > $raw/oxigraph-$mode-types.tsv
done
jena $raw/work-jena-go types $raw/classes.tsv 5 ../../data/downloads/go/go.nt ../semantic/raw/work-gotypes/entities.nt > $raw/jena-types.tsv 2>/dev/null
$py sqlite_bench.py $raw/work-sql-go types $raw/classes.tsv 5 ../../data/downloads/go/go.nt ../semantic/raw/work-gotypes/entities.nt > $raw/sqlite-types.tsv
# the decode cache off, for the ablation
ONTODB_TERM_CACHE=0 $od $raw/work-ontodb-nc $raw/queries-lubm10.tsv 5 ../../data/downloads/lubm10/University*.owl > $raw/ontodb-nocache-lubm10.tsv
(cd ../.. && ONTODB_TERM_CACHE=0 ./build/release/ontodb_bench_semantic types benchmarks/semantic/raw/work-gotypes go data/downloads/go/go.nt 200000 > /dev/null && cp benchmarks/semantic/raw/work-gotypes/classes.tsv benchmarks/compare/raw/classes-nocache.tsv)
(cd ../.. && ./build/release/ontodb_bench_semantic types benchmarks/semantic/raw/work-gotypes go data/downloads/go/go.nt 200000 > /dev/null && cp benchmarks/semantic/raw/work-gotypes/classes.tsv benchmarks/compare/raw/classes.tsv)
# concurrency: one to eight threads, three seconds each
lfiles=(../../data/downloads/lubm10/University*.owl)
cmake --build ../../build/release --target ontodb_bench_concurrency >/dev/null
../../build/release/ontodb_bench_concurrency $raw/work-conc $raw/queries-lubm10.tsv 3 "${lfiles[@]}" > $raw/ontodb-conc.tsv
$ox rocksdb $raw/work-oxi-conc conc $raw/queries-lubm10.tsv 3 "${lfiles[@]}" > $raw/oxigraph-rocksdb-conc.tsv
$ox memory $raw/work-oxi-conc conc $raw/queries-lubm10.tsv 3 "${lfiles[@]}" > $raw/oxigraph-memory-conc.tsv
jena $raw/work-jena-conc conc $raw/queries-lubm10.tsv 3 "${lfiles[@]}" > $raw/jena-conc.tsv 2>/dev/null
$py sqlite_bench.py $raw/work-sql-conc conc $raw/queries-lubm10.tsv 3 "${lfiles[@]}" > $raw/sqlite-conc.tsv
python3 summarize.py $raw/ontodb-lubm10.tsv $raw/oxigraph-lubm10.tsv $raw/oxigraph-mem-lubm10.tsv $raw/jena-lubm10.tsv $raw/sqlite-lubm10.tsv > $raw/summary-lubm10.md
python3 summarize.py $raw/ontodb-lubm1.tsv $raw/oxigraph-lubm1.tsv $raw/oxigraph-mem-lubm1.tsv $raw/jena-lubm1.tsv $raw/sqlite-lubm1.tsv > $raw/summary-lubm1.md
python3 summarize_types.py $raw/oxigraph-rocksdb-types.tsv $raw/oxigraph-memory-types.tsv $raw/jena-types.tsv $raw/sqlite-types.tsv > $raw/summary-types.md
cat $raw/summary-*.md
