#!/usr/bin/env python3
"""Turn the per-query lines of both engines into the tables of results.md.

    summarize.py <ontodb.tsv> <oxigraph.tsv> [<oxigraph-memory.tsv>]

Prints the load line, the LUBM queries one per row, and the single-pattern queries by shape. Row
counts must agree between engines; any query where they do not is listed first."""
import math
import re
import sys
from collections import defaultdict


def read(path):
    load, rows = {}, {}
    for line in open(path):
        cols = line.rstrip("\n").split("\t")
        if cols[0] == "#load":
            load = dict(c.split("=") for c in cols[1:])
        elif len(cols) >= 3:
            rows[cols[0]] = (int(cols[1]), None if cols[2] in ("timeout",) or cols[2].startswith("error") else float(cols[2]), cols[2])
    return load, rows


def fmt(us):
    if us is None:
        return "timeout"
    return f"{us:.0f} µs" if us < 1000 else f"{us / 1000:.1f} ms"


def main():
    paths = sys.argv[1:]
    names = ["OntoDB", "Oxigraph (RocksDB)", "Oxigraph (memory)"][: len(paths)]
    data = [read(p) for p in paths]
    loads = [d[0] for d in data]
    rows = [d[1] for d in data]
    print("| Engine | triples | load (s) | on disk (MiB) | peak RSS (MiB) |\n|---|---|---|---|---|")
    for n, l in zip(names, loads):
        print(f"| {n} | {int(l['triples']):,} | {l['load_s']} | {l['file_mib']} | {l['peak_rss_mib']} |")
    print()
    bad = [q for q in rows[0] if any(q in r and r[q][0] != rows[0][q][0] for r in rows[1:])]
    if bad:
        print("ROW COUNTS DISAGREE:", ", ".join(bad), "\n")
    print("| Query | rows | " + " | ".join(names) + " | " + " | ".join(f"{n.split()[0]}/OntoDB" if i else "" for i, n in enumerate(names)).strip(" |") + " |")
    print("|---|---|" + "---|" * (len(names) * 2 - 1))
    for q in rows[0]:
        if not re.match(r"q\d+", q):
            continue
        cells = [fmt(r[q][1]) if q in r else "-" for r in rows]
        ratios = [f"{r[q][1] / rows[0][q][1]:.1f}x" if q in r and r[q][1] and rows[0][q][1] else "-" for r in rows[1:]]
        print(f"| {q} | {rows[0][q][0]} | " + " | ".join(cells) + " | " + " | ".join(ratios) + " |")
    print()
    print("| Pattern | queries | mean rows | " + " | ".join(f"{n} mean / p99" for n in names) + " |")
    print("|---|---|---|" + "---|" * len(names))
    shapes = defaultdict(list)
    for q in rows[0]:
        if not q.startswith("q"):
            shapes[q.split("#")[0]].append(q)
    for shape, qs in shapes.items():
        cells = []
        for r in rows:
            ts = sorted(r[q][1] for q in qs if q in r and r[q][1] is not None)
            cells.append(f"{sum(ts) / len(ts):.0f} / {ts[int(0.99 * (len(ts) - 1))]:.0f} µs")
        mean_rows = sum(rows[0][q][0] for q in qs) / len(qs)
        print(f"| {shape} | {len(qs)} | {mean_rows:.1f} | " + " | ".join(cells) + " |")


if __name__ == "__main__":
    main()
