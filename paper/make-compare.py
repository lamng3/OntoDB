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

# Concurrency: lookups per second against threads, and what writers do
conc_engines = [("ontodb", SYS, "blue!70!black", "mark=*"), ("oxigraph-rocksdb", "Ox.\\ RocksDB", "red!70!black", "mark=square*"),
                ("oxigraph-memory", "Ox.\\ memory", "orange!80!black", "mark=triangle*"), ("jena", "Jena TDB2", "green!50!black", "mark=diamond*"),
                ("sqlite", "SQLite", "black!60", "mark=pentagon*")]
conc = {}
for key, _, _, _ in conc_engines:
    d = defaultdict(dict)
    for line in (raw / f"{key.replace('oxigraph-rocksdb', 'oxigraph-rocksdb')}-conc.tsv").read_text().splitlines():
        c = line.split("\t")
        if len(c) == 5 and c[0] in ("read", "insert", "mixed") and c[1] != "range":
            d[c[0]][int(c[2])] = (float(c[3]), float(c[4]))
    conc[key] = d
plot = ["\\begin{tikzpicture}\n\\begin{axis}[width=\\columnwidth,height=5.6cm,xmode=log,ymode=log,log basis x=2,xtick={1,2,4,8},xticklabels={1,2,4,8},xlabel={threads},ylabel={lookups per second},grid=both,legend pos=south east,legend style={font=\\scriptsize},tick label style={font=\\scriptsize},label style={font=\\small}]"]
for key, name, color, mark in conc_engines:
    pts = " ".join(f"({t},{conc[key]['read'][t][0]:.0f})" for t in (1, 2, 4, 8))
    plot.append(f"\\addplot+[{mark},color={color},thick] coordinates {{{pts}}};\n\\addlegendentry{{{name}}}")
plot.append("\\end{axis}\n\\end{tikzpicture}")
write("conc-plot.tex", "\n".join(plot) + "\n")


def k(v):
    if v >= 1e6:
        return f"{v / 1e6:.2f}M"
    return f"{v / 1000:.0f}k" if v >= 10000 else f"{v:,.0f}"


rows = ["\\begin{table}[t]\n\\caption{Concurrency on LUBM(10): lookups per second with 1 and 8 reader threads, readers' lookups per second with one writer running, and inserts per second with 1 and 8 writer threads (one triple per transaction; \\sys's inserts are not logged, and Jena's commit forces the journal to disk). The machine has 8 performance cores.}\n\\label{tab-conc}\n\\small\n\\resizebox{\\columnwidth}{!}{%\n\\begin{tabular}{@{}lrrrrrr@{}}\n\\toprule\n & \\multicolumn{2}{c}{lookups/s} & \\multicolumn{2}{c}{readers + 1 writer} & \\multicolumn{2}{c}{inserts/s} \\\\\n\\cmidrule(lr){2-3}\\cmidrule(lr){4-5}\\cmidrule(lr){6-7}\nEngine & 1 & 8 & 1 & 8 & 1 & 8 \\\\\n\\midrule"]
for key, name, _, _ in conc_engines:
    d = conc[key]
    rows.append(f"{name} & {k(d['read'][1][0])} & {k(d['read'][8][0])} & {k(d['mixed'][1][0])} & {k(d['mixed'][8][0])} & {k(d['insert'][1][1])} & {k(d['insert'][8][1])} \\\\")
rows.append("\\bottomrule\n\\end{tabular}}\n\\end{table}\n")
write("compare-conc.tex", "\n".join(rows))
print("concurrency tables written")


# Results at a glance: the speedup of OntoDB over each other engine on selected workloads
def mean_shape(key, shape):
    qs = shapes[shape]
    return sum(data[key][1][q][1] for q in qs) / len(qs)


others = [("oxigraph", "Ox.\\ RocksDB"), ("oxigraph-mem", "Ox.\\ memory"), ("jena", "Jena TDB2"), ("sqlite", "SQLite")]
rows = []
for shape, label in (("s--", "pattern $(s,\\cdot,\\cdot)$"), ("sp-", "pattern $(s,p,\\cdot)$")):
    base = mean_shape("ontodb", shape)
    rows.append((label, [mean_shape(k_, shape) / base for k_, _ in others]))
base = data["ontodb"][1]["q01"][1]
rows.append(("LUBM query 1", [data[k_][1]["q01"][1] / base for k_, _ in others]))
type_keys = ["oxigraph-rocksdb", "oxigraph-memory", "jena", "sqlite"]
for b, label in (("10-99", "instances of a class, 10--99 below"), ("1000+", "instances of a class, 1000+ below")):
    own = sum(cls_on[r[0]][2] for r in tabs[0][b]) / len(tabs[0][b])
    ratios = []
    for tab in tabs:
        ratios.append((sum(r[2] for r in tab[b]) / len(tab[b])) / own)
    rows.append((label, ratios))
okeys = ["oxigraph-rocksdb", "oxigraph-memory", "jena", "sqlite"]
rows.append(("lookups per second, 8 threads", [conc["ontodb"]["read"][8][0] / conc[k_]["read"][8][0] for k_ in okeys]))
rows.append(("lookups per second, 8 readers + 1 writer", [conc["ontodb"]["mixed"][8][0] / conc[k_]["mixed"][8][0] for k_ in okeys]))
out = ["\\begin{table}[t]\n\\caption{Selected results: how many times faster \\sys{} is than each other engine (the time of the other engine divided by \\sys's; for throughput, \\sys's divided by the other's). All tables of this section give every measurement.}\n\\label{tab-glance}\n\\small\n\\resizebox{\\columnwidth}{!}{%\n\\begin{tabular}{@{}lrrrr@{}}\n\\toprule\n & Ox.\\ RocksDB & Ox.\\ memory & Jena TDB2 & SQLite \\\\\n\\midrule"]
for label, ratios in rows:
    out.append(label + " & " + " & ".join(f"{r:.1f}$\\times$" for r in ratios) + " \\\\")
out.append("\\bottomrule\n\\end{tabular}}\n\\end{table}\n")
write("compare-glance.tex", "\n".join(out))
print("glance written")
