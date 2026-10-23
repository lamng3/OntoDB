#!/usr/bin/env python3
"""A relational baseline for class-filtered vector search: DuckDB with a closure table.

    duckdb_baseline.py <name> <ontology.nt> <terms.ovec> <synonyms.ovec> <workload.tsv> <out dir>

The terms are a table (id, vector FLOAT[d]); the hierarchy is a closure table (class, member) built
here from the subClassOf triples; a query is one SQL statement that joins the class's members to
their vectors and orders by inner product, which is exact search over the members (DuckDB's
vector-similarity extension is not used, so there is no approximate index to lose recall). Each
query is one call from Python, single thread; the closure table is built outside the timer.
Replays the workload of ontodb_bench_vector, so recall is 1 by construction and only the time is
of interest."""
import sys
import time
from pathlib import Path

import duckdb
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from faiss_baseline import K, descendants, normalized, read_ovec  # noqa: E402


def main(name, nt, terms, syn, workload, out):
    names, vecs = read_ovec(terms)
    vecs = normalized(vecs)
    syn_names, syn_vecs = read_ovec(syn)
    syn_vecs = normalized(syn_vecs)
    row_of = {n: i for i, n in enumerate(names)}
    below = descendants(nt)
    n, d = vecs.shape
    con = duckdb.connect()
    con.execute("PRAGMA threads=1")
    con.execute(f"CREATE TABLE terms (id INTEGER, v FLOAT[{d}])")
    con.executemany("INSERT INTO terms VALUES (?, ?)", [(i, vecs[i].tolist()) for i in range(n)])
    t = time.time()
    classes = {}
    lines = list(open(workload))[1:]
    needed = sorted({line.split("\t")[2] for line in lines})
    pairs = []
    for ci, c in enumerate(needed):
        classes[c] = ci
        for m in below(c):
            if m in row_of:
                pairs.append((ci, row_of[m]))
    con.execute("CREATE TABLE closure (cls INTEGER, member INTEGER)")
    con.executemany("INSERT INTO closure VALUES (?, ?)", pairs)
    con.execute("CREATE INDEX closure_cls ON closure (cls)")
    build = time.time() - t
    sql = (f"SELECT t.id FROM closure c JOIN terms t ON t.id = c.member WHERE c.cls = ? "
           f"ORDER BY array_inner_product(t.v, ?::FLOAT[{d}]) DESC LIMIT {K + 1}")
    with open(Path(out) / f"{name}-duckdb.tsv", "w") as result:
        result.write("ontology\tbucket\tkind\tclass\tstrategy\trecall\tus\tbuild_s\n")
        for line in lines:
            bucket, kind, cls, ref = line.rstrip("\n").split("\t")
            tag, idx = ref.split(":")
            idx = int(idx)
            q = (vecs[idx] if tag == "row" else syn_vecs[idx]).tolist()
            ci = classes[cls]
            con.execute(sql, [ci, q]).fetchall()
            t0 = time.perf_counter()
            for _ in range(3):
                con.execute(sql, [ci, q]).fetchall()
            us = (time.perf_counter() - t0) / 3 * 1e6
            result.write(f"{name}\t{bucket}\t{kind}\t{cls}\tduckdb-exact\t1.0\t{us:.1f}\t{build:.1f}\n")


if __name__ == "__main__":
    main(*sys.argv[1:7])
