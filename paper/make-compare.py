#!/usr/bin/env python3
"""Turn benchmarks/compare/raw into the tables of the paper (gen/compare-*.tex).
The raw outputs are not committed; the generated files are, so the paper builds alone.
Run benchmarks/compare/run.sh first."""
import sys
from collections import defaultdict
from pathlib import Path

root = Path(__file__).resolve().parents[1]
raw = root / "benchmarks/compare/raw"
gen = root / "paper/gen"


def read(path):
    load, rows = {}, {}
    for line in (raw / path).read_text().splitlines():
        c = line.split("\t")
        if c[0] == "#load":
            load = dict(x.split("=") for x in c[1:])
        elif len(c) >= 3 and c[2] not in ("timeout",) and not c[2].startswith("error"):
            rows[c[0]] = (int(c[1]), float(c[2]))
    return load, rows


def t(us):
    return f"{us:.0f}\\,$\\mu$s" if us < 1000 else (f"{us / 1000:.1f}\\,ms" if us < 1e6 else f"{us / 1e6:.2f}\\,s")


def write(name, text):
    (gen / name).write_text(text)


onto = {n: read(f"ontodb-{n}.tsv") for n in ("lubm1", "lubm10")}
rocks = {n: read(f"oxigraph-{n}.tsv") for n in ("lubm1", "lubm10")}
mem = {n: read(f"oxigraph-mem-{n}.tsv") for n in ("lubm1", "lubm10")}

# Load and space
out = ["\\begin{table}[t]\n\\caption{Loading LUBM in each engine (bulk loader, one thread of the loader, files on a local SSD). Oxigraph's count is distinct triples; \\sys's is statements read.}\n\\label{tab:load}\n\\small\n\\resizebox{\\columnwidth}{!}{%\n\\begin{tabular}{@{}llrrrr@{}}\n\\toprule\nData & Engine & triples & load (s) & disk (MiB) & RSS (MiB) \\\\\n\\midrule"]
SYS = "\\sys{}"
for n, label in (("lubm1", "LUBM(1)"), ("lubm10", "LUBM(10)")):
    for name, d in ((SYS, onto), ("Oxigraph, RocksDB", rocks), ("Oxigraph, memory", mem)):
        l = d[n][0]
        disk = "---" if float(l["file_mib"]) == 0 else l["file_mib"]
        first = label if name == SYS else ""
        count = f"{int(l['triples']):,}".replace(",", "\\,")
        out.append(f"{first} & {name} & {count} & {l['load_s']} & {disk} & {l['peak_rss_mib']} \\\\")
    if n == "lubm1":
        out.append("\\midrule")
out.append("\\bottomrule\n\\end{tabular}}\n\\end{table}\n")
write("compare-load.tex", "\n".join(out))

# Patterns, LUBM(10)
shapes = defaultdict(list)
for q in onto["lubm10"][1]:
    if not q.startswith("q"):
        shapes[q.split("#")[0]].append(q)
sym = {"s--": "$s\\cdot\\cdot$", "sp-": "$sp\\cdot$", "--o": "$\\cdot\\cdot o$", "-po": "$\\cdot po$", "-p-": "$\\cdot p\\cdot$"}
out = ["\\begin{table}[t]\n\\caption{Single-pattern queries on LUBM(10), 100 sampled constants per shape ($\\cdot p\\cdot$: every predicate): mean time per query (99th percentile in parentheses). Row counts agree between engines.}\n\\label{tab:patterns}\n\\small\n\\resizebox{\\columnwidth}{!}{%\n\\begin{tabular}{@{}lrrrr@{}}\n\\toprule\nPattern & rows & \\sys{} & Oxigraph, RocksDB & Oxigraph, memory \\\\\n\\midrule"]
for shape in ("s--", "sp-", "--o", "-po", "-p-"):
    qs = shapes[shape]
    cells = []
    for d in (onto, rocks, mem):
        ts = sorted(d["lubm10"][1][q][1] for q in qs)
        cells.append(f"{t(sum(ts) / len(ts))} ({t(ts[int(0.99 * (len(ts) - 1))])})")
    rows = sum(onto["lubm10"][1][q][0] for q in qs) / len(qs)
    out.append(f"{sym[shape]} & {rows:,.1f} & " + " & ".join(cells) + " \\\\")
out.append("\\bottomrule\n\\end{tabular}}\n\\end{table}\n")
write("compare-patterns.tex", "\n".join(out))

# LUBM queries with rows
out = ["\\begin{table}[t]\n\\caption{The LUBM queries that return rows without inference, on LUBM(10): median time.}\n\\label{tab:lubm}\n\\small\n\\resizebox{\\columnwidth}{!}{%\n\\begin{tabular}{@{}lrrrr@{}}\n\\toprule\nQuery & rows & \\sys{} & Oxigraph, RocksDB & Oxigraph, memory \\\\\n\\midrule"]
for q in ("q01", "q02", "q03", "q14"):
    out.append(f"{q} & {onto['lubm10'][1][q][0]:,} & " + " & ".join(t(d["lubm10"][1][q][1]) for d in (onto, rocks, mem)) + " \\\\")
out.append("\\bottomrule\n\\end{tabular}}\n\\end{table}\n")
write("compare-lubm.tex", "\n".join(out))

# Hierarchy: instances of a class and its subclasses
cls = {}
for line in (raw / "classes.tsv").read_text().splitlines():
    c = line.split("\t")
    cls[c[1]] = (float(c[3]), float(c[4]))
tabs = []
for name in ("rocksdb", "memory"):
    by = defaultdict(list)
    for line in (raw / f"oxigraph-{name}-types.tsv").read_text().splitlines():
        c = line.split("\t")
        if c[0] != "#load":
            if c[2] != c[3]:
                sys.exit(f"answer counts disagree: {c}")
            by[c[0]].append((c[1], int(c[2]), float(c[4])))
    tabs.append(by)
out = ["\\begin{table}[t]\n\\caption{Instances of a class and its subclasses, 200\\,000 entities over the Gene Ontology, 30 classes per row, warm: mean time. \\sys{} answers by interval scans; Oxigraph by \\texttt{?x a ?c . ?c rdfs:subClassOf* <C>}. \\sys{} ``ids'' stops before the dictionary; ``strings'' decodes every answer, as Oxigraph does.}\n\\label{tab:hier}\n\\small\n\\resizebox{\\columnwidth}{!}{%\n\\begin{tabular}{@{}lrrrrr@{}}\n\\toprule\nBelow $C$ & answers & \\sys{} ids & \\sys{} strings & Ox., RocksDB & Ox., memory \\\\\n\\midrule"]
for b in ("1", "2-9", "10-99", "100-999", "1000+"):
    rows = tabs[0][b]
    n = len(rows)
    ans = sum(r[1] for r in rows) / n
    ids = sum(cls[r[0]][0] for r in rows) / n
    strs = sum(cls[r[0]][1] for r in rows) / n
    ox = [sum(r[2] for r in tab[b]) / n for tab in tabs]
    out.append(f"{b} & {ans:,.0f} & {t(ids)} & {t(strs)} & {t(ox[0])} & {t(ox[1])} \\\\")
out.append("\\bottomrule\n\\end{tabular}}\n\\end{table}\n")
write("compare-hier.tex", "\n".join(out))
print("wrote", ", ".join(sorted(p.name for p in gen.glob("compare-*.tex"))))
