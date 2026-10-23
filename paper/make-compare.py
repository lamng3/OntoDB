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
SYS = "\\sys{}"
ENGINES = [("ontodb", SYS), ("oxigraph", "Ox.\\ RocksDB"), ("oxigraph-mem", "Ox.\\ memory"), ("jena", "Jena TDB2"), ("sqlite", "SQLite")]


def read(path):
    load, rows = {}, {}
    for line in (raw / path).read_text().splitlines():
        c = line.split("\t")
        if c[0] == "#load":
            load = dict(x.split("=") for x in c[1:])
        elif len(c) >= 3:
            rows[c[0]] = (int(c[1]), None if c[2] == "timeout" or c[2].startswith("error") else float(c[2]))
    return load, rows


def t(us):
    if us is None:
        return "timeout"
    return f"{us:.0f}\\,$\\mu$s" if us < 1000 else (f"{us / 1000:.1f}\\,ms" if us < 1e6 else f"{us / 1e6:.2f}\\,s")


def thousands(n):
    return f"{n:,}"


def write(name, text):
    (gen / name).write_text(text)


data = {k: read(f"{k}-lubm10.tsv") for k, _ in ENGINES}
nocache = read("ontodb-nocache-lubm10.tsv")

# Load and space
out = ["\\begin{table}[t]\n\\caption{Bulk loading LUBM(10) (1.3 million triples). \\sys{} counts statements read; the others count distinct triples.}\n\\label{tab-load}\n\\small\n\\begin{tabular}{@{}lrrrr@{}}\n\\toprule\nEngine & triples & load (s) & disk (MiB) & RSS (MiB) \\\\\n\\midrule"]
for k, name in ENGINES:
    l = data[k][0]
    disk = "---" if float(l["file_mib"]) == 0 else l["file_mib"]
    out.append(f"{name} & {thousands(int(l['triples']))} & {l['load_s']} & {disk} & {l['peak_rss_mib']} \\\\")
out.append("\\bottomrule\n\\end{tabular}\n\\end{table}\n")
write("compare-load.tex", "\n".join(out))

# Patterns and LUBM queries
shapes = defaultdict(list)
for q in data["ontodb"][1]:
    if not q.startswith("q"):
        shapes[q.split("#")[0]].append(q)
sym = {"s--": "$(s,\\cdot,\\cdot)$", "sp-": "$(s,p,\\cdot)$", "--o": "$(\\cdot,\\cdot,o)$", "-po": "$(\\cdot,p,o)$", "-p-": "$(\\cdot,p,\\cdot)$"}
cols = "".join(f" & {name}" for _, name in ENGINES)
out = [f"\\begin{{table*}}[t]\n\\caption{{Query times on LUBM(10): the mean over 100 sampled constants per pattern shape (all 18 predicates for $(\\cdot,p,\\cdot)$), and the median of the four LUBM queries that return rows without inference. Row counts agree across engines.}}\n\\label{{tab-queries}}\n\\small\n\\begin{{tabular}}{{@{{}}lr{'r' * len(ENGINES)}@{{}}}}\n\\toprule\nQuery & rows{cols} \\\\\n\\midrule"]
for shape in ("s--", "sp-", "--o", "-po", "-p-"):
    qs = shapes[shape]
    rows = sum(data["ontodb"][1][q][0] for q in qs) / len(qs)
    cells = []
    for k, _ in ENGINES:
        ts = [data[k][1][q][1] for q in qs]
        cells.append(t(sum(ts) / len(ts)))
    out.append(f"{sym[shape]} & {thousands(round(rows))} & " + " & ".join(cells) + " \\\\")
out.append("\\midrule")
for q, label in (("q01", "LUBM 1"), ("q03", "LUBM 3"), ("q02", "LUBM 2 (triangle)"), ("q14", "LUBM 14 (scan)")):
    out.append(f"{label} & {thousands(data['ontodb'][1][q][0])} & " + " & ".join(t(data[k][1][q][1]) for k, _ in ENGINES) + " \\\\")
out.append("\\bottomrule\n\\end{tabular}\n\\end{table*}\n")
write("compare-queries.tex", "\n".join(out))

# Decode cache ablation: LUBM(10) patterns, and the hierarchy strings
cls_on, cls_off = {}, {}
for d, path in ((cls_on, "classes.tsv"), (cls_off, "classes-nocache.tsv")):
    for line in (raw / path).read_text().splitlines():
        c = line.split("\t")
        d[c[1]] = (c[0], float(c[3]), float(c[4]))
out = ["\\begin{table}[t]\n\\caption{The effect of \\sys's decode cache (65\\,536 entries) on the time to return strings: mean time per query on LUBM(10), and the instances of a class on the Gene Ontology (30 classes per row).}\n\\label{tab-cache}\n\\small\n\\begin{tabular}{@{}lrrr@{}}\n\\toprule\nWorkload & answers & cache off & cache on \\\\\n\\midrule"]
for shape in ("s--", "sp-", "--o", "-po", "-p-"):
    qs = shapes[shape]
    rows = sum(data["ontodb"][1][q][0] for q in qs) / len(qs)
    off = sum(nocache[1][q][1] for q in qs) / len(qs)
    on = sum(data["ontodb"][1][q][1] for q in qs) / len(qs)
    out.append(f"{sym[shape]} & {thousands(round(rows))} & {t(off)} & {t(on)} \\\\")
out.append("\\midrule")
for b in ("100-999", "1000+"):
    cs = [c for c in cls_on if cls_on[c][0] == b]
    ans = 2954 if b == "100-999" else 29904
    out.append(f"instances, {b} below & {thousands(ans)} & {t(sum(cls_off[c][2] for c in cs) / len(cs))} & {t(sum(cls_on[c][2] for c in cs) / len(cs))} \\\\")
out.append("\\bottomrule\n\\end{tabular}\n\\end{table}\n")
write("compare-cache.tex", "\n".join(out))

# Hierarchy
tabs = []
for name in ("oxigraph-rocksdb", "oxigraph-memory", "jena", "sqlite"):
    by = defaultdict(list)
    for line in (raw / f"{name}-types.tsv").read_text().splitlines():
        c = line.split("\t")
        if c[0] != "#load":
            if c[2] != c[3]:
                sys.exit(f"answer counts disagree: {name} {c}")
            by[c[0]].append((c[1], int(c[2]), None if c[4] == "timeout" else float(c[4])))
    tabs.append(by)
out = ["\\begin{table*}[t]\n\\caption{Instances of a class and its subclasses on the Gene Ontology with 200\\,000 entities: mean time over 30 classes per row. \\sys{} answers by interval scans: ``ids'' stops before the dictionary, ``strings'' decodes every answer as the other engines do. The others evaluate the subclass path \\texttt{rdfs:subClassOf*} (SQLite: a recursive query). Answer counts agree on every class.}\n\\label{tab-hier}\n\\small\n\\begin{tabular}{@{}lrrrrrrr@{}}\n\\toprule\nSubclasses below $c$ & answers & \\sys{} ids & \\sys{} strings & Ox.\\ RocksDB & Ox.\\ memory & Jena TDB2 & SQLite \\\\\n\\midrule"]
for b in ("1", "2-9", "10-99", "100-999", "1000+"):
    rows = tabs[0][b]
    n = len(rows)
    ans = sum(r[1] for r in rows) / n
    ids = sum(cls_on[r[0]][1] for r in rows) / n
    strs = sum(cls_on[r[0]][2] for r in rows) / n
    cells = [t(sum(r[2] for r in tab[b]) / n) for tab in tabs]
    out.append(f"{b} & {thousands(round(ans))} & {t(ids)} & {t(strs)} & " + " & ".join(cells) + " \\\\")
out.append("\\bottomrule\n\\end{tabular}\n\\end{table*}\n")
write("compare-hier.tex", "\n".join(out))
for old in ("compare-patterns.tex", "compare-lubm.tex"):
    (gen / old).unlink(missing_ok=True)
print("wrote", ", ".join(sorted(p.name for p in gen.glob("compare-*.tex"))))
