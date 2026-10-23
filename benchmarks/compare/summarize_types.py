#!/usr/bin/env python3
"""Instances of a class and its subclasses: OntoDB's interval scans against Oxigraph's subclass path.

    summarize_types.py <oxigraph-rocksdb-types.tsv> <oxigraph-memory-types.tsv>

Each input line: bucket, class, OntoDB answers, Oxigraph answers, Oxigraph microseconds. The
OntoDB microseconds are column 4 of classes.tsv, joined by class."""
import sys
from collections import defaultdict

onto = {l.split("\t")[1]: (float(l.split("\t")[3]), float(l.split("\t")[4])) for l in open("raw/classes.tsv")}
order = ["1", "2-9", "10-99", "100-999", "1000+"]
tables = []
for path in sys.argv[1:]:
    by = defaultdict(list)
    for l in open(path):
        c = l.rstrip("\n").split("\t")
        if c[0] == "#load":
            continue
        by[c[0]].append((c[1], int(c[2]), int(c[3]), float(c[4])))
    tables.append(by)
bad = sum(1 for by in tables for v in by.values() for x in v if x[1] != x[2])
print(f"answer counts that disagree: {bad}")
print("| Below C | classes | answers | OntoDB ids | OntoDB strings | Oxigraph (RocksDB) | Oxigraph (memory) | RocksDB / OntoDB strings | memory / OntoDB strings |\n|---|---|---|---|---|---|---|---|---|")
for b in order:
    rows = tables[0][b]
    n = len(rows)
    ans = sum(r[1] for r in rows) / n
    oi = sum(onto[r[0]][0] for r in rows) / n
    os_ = sum(onto[r[0]][1] for r in rows) / n
    x = [sum(r[3] for r in t[b]) / n for t in tables]
    print(f"| {b} | {n} | {ans:.0f} | {oi:.0f} µs | {os_:.0f} µs | {x[0]:.0f} µs | {x[1]:.0f} µs | {x[0] / os_:.1f}x | {x[1] / os_:.1f}x |")
