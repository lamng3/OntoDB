#!/usr/bin/env python3
"""Turn benchmarks/vector/raw into the tables and figures of the paper (gen/*.tex).
The raw outputs are not committed; the generated files are, so the paper builds alone.
Run benchmarks/vector/run.sh first."""
import csv
import re
import statistics as st
from collections import defaultdict
from pathlib import Path

root = Path(__file__).resolve().parents[1]
raw = root / "benchmarks/vector/raw"
gen = Path(__file__).resolve().parent / "gen"
ONT = [("go", "GO"), ("hpo", "HPO"), ("uberon", "Uberon"), ("chebi", "ChEBI")]
ONT = [(k, n) for k, n in ONT if (raw / f"{k}-hybrid.tsv").exists() and (raw / f"{k}-faiss-index.tsv").exists()]
BUCKETS = ["<=10", "11-100", "101-1000", "1001-5000", ">5000"]
BUCKET_TEX = {"<=10": "$\\le$10", "11-100": "11--100", "101-1000": "101--1,000", "1001-5000": "1,001--5,000", ">5000": "$>$5,000"}
SYS = "\\sys{}"


def tsv(path):
    return list(csv.DictReader(open(path), delimiter="\t"))


def write(name, text):
    (gen / name).write_text(text)


def us(v):
    if v < 1000:
        return f"{v:.0f}" if v >= 10 else f"{v:.1f}"
    return f"{v / 1000:.1f}\\,k" if v < 1e6 else f"{v / 1e6:.1f}\\,M"


hyb = defaultdict(list)  # (ontology, bucket, kind, strategy) -> [(recall, us, under)]
for k, _ in ONT:
    extra = tsv(raw / f"{k}-duckdb.tsv") if (raw / f"{k}-duckdb.tsv").exists() else []
    for r in tsv(raw / f"{k}-hybrid.tsv") + tsv(raw / f"{k}-faiss-hybrid.tsv") + extra:
        hyb[(k, r["bucket"], r["kind"], r["strategy"])].append((float(r["recall"]), float(r["us"]), float(r.get("under_share") or 0)))


def pooled(bucket, kind, strategy):
    rows = [x for k, _ in ONT for x in hyb.get((k, bucket, kind, strategy), [])]
    return (st.mean(x[0] for x in rows), st.mean(x[1] for x in rows)) if rows else None


# --- order locality
def order_rows(k):
    out = {}
    for line in (raw / f"{k}-order.md").read_text().splitlines():
        c = [x.strip() for x in line.strip("|").split("|")]
        if len(c) >= 8 and re.match(r"^[0-9.]+$", c[1] or ""):
            out[c[0]] = (int(c[1]), [float(x) for x in c[2:8]])
    return out


lines = ["\\begin{table}[t]\n\\caption{Share of a term's ten nearest neighbors (cosine) found within $\\pm$100 positions, and the median position gap to the nearest neighbor, by order of the terms. 2,000 queries.}\n\\label{tab-order}\n\\small\n\\resizebox{\\columnwidth}{!}{%\n\\begin{tabular}{@{}lrrrrrrrr@{}}\n\\toprule\n & & \\multicolumn{4}{c}{share within $\\pm$100} & \\multicolumn{3}{c}{median gap} \\\\\n\\cmidrule(lr){3-6}\\cmidrule(lr){7-9}\nOntology & terms & hierarchy & first-seen id & alphabetical & random & hierarchy & id & random \\\\\n\\midrule"]
for k, name in ONT:
    o = order_rows(k)
    n = int(re.search(r"(\d+) vectors", (raw / f"{k}-order.md").read_text()).group(1))
    h, i, a, r = (o["hierarchy, deepest parent"], o["dictionary id (first seen)"], o["label, alphabetical"], o["random"])
    shares = [h[1][2], i[1][2], a[1][2], r[1][2]]
    best = max(shares)
    cells = [f"\\textbf{{{v:.2f}}}" if v == best else (f"{v:.2f}" if j < 3 else f"{v:.3f}") for j, v in enumerate(shares)]
    lines.append(f"{name} & {n:,} & " + " & ".join(cells) + f" & {h[0]:,} & {i[0]:,} & {r[0]:,} \\\\")
lines.append("\\bottomrule\n\\end{tabular}}\n\\end{table}\n")
write("order.tex", "\n".join(lines))

# --- the filtered search table: recall / microseconds, pooled over the ontologies
strategies = [("duckdb-exact", "DuckDB join"), ("faiss-flat", "FAISS exact"), ("faiss-ivf", "FAISS IVF"), ("faiss-hnsw", "FAISS HNSW"), ("pre-filter", "pre-filter"), ("mask/hnsw", "masked HNSW"), ("auto", "auto")]
head = " & ".join(n for _, n in strategies)
out = [f"\\begin{{table*}}[t]\n\\caption{{Ten nearest terms under a class: recall@10 / microseconds per query, by number of classes below $c$ and by query kind, pooled over {len(ONT)} ontologies. FAISS runs each query with an ID selector of the class's members (their lookup is not timed). Bold marks the fastest strategy with recall at least 0.99; blue marks \\sys{{}}.}}\n\\label{{tab-filtered}}\n\\small\n\\begin{{tabular}}{{@{{}}ll{'c' * len(strategies)}@{{}}}}\n\\toprule\nBelow $c$ & query & {head} \\\\\n\\midrule"]
for kind, label in (("inside", "inside $c$"), ("outside", "outside $c$"), ("synonym", "synonym of a member")):
    for b in BUCKETS:
        cells = {s: pooled(b, kind, s) for s, _ in strategies}
        good = {s: v[1] for s, v in cells.items() if v and v[0] >= 0.99}
        best = min(good, key=good.get) if good else None
        shown = []
        for s, _ in strategies:
            v = cells[s]
            if v is None:
                shown.append("---")
                continue
            text = f"{v[0]:.2f} / {us(v[1])}"
            if s == best:
                text = f"\\textbf{{{text}}}"
                if s in ("pre-filter", "mask/hnsw", "auto"):
                    text = f"\\cellcolor{{ours}}{text}"
            shown.append(text)
        out.append(f"{BUCKET_TEX[b] if kind == 'inside' or True else ''} & {label if b == BUCKETS[0] else ''} & " + " & ".join(shown) + " \\\\")
    if kind != "synonym":
        out.append("\\midrule")
out.append("\\bottomrule\n\\end{tabular}\n\\end{table*}\n")
write("filtered.tex", "\n".join(out))

# --- the budget curve: what more search effort buys FAISS on queries outside small and medium classes
def pool_small(strategy):
    rows = [x for k, _ in ONT for b in ("<=10", "11-100", "101-1000") for x in hyb.get((k, b, "outside", strategy), [])]
    return (st.mean(x[0] for x in rows), st.mean(x[1] for x in rows)) if rows else None


curves = [("FAISS IVF (nprobe 16, 64, 256, half)", "red!70!black", "square*", ["faiss-ivf", "faiss-ivf-np64", "faiss-ivf-np256", "faiss-ivf-wide"]),
          ("FAISS HNSW (beam 64, 256, 1,024, 4,096)", "orange!80!black", "triangle*", ["faiss-hnsw", "faiss-hnsw-ef256", "faiss-hnsw-wide", "faiss-hnsw-ef4096"])]
plot = ["\\begin{tikzpicture}\n\\begin{axis}[width=\\columnwidth,height=5.0cm,xmode=log,xlabel={$\\mu$s per query (log)},ylabel={recall@10},ymin=0,ymax=1.05,grid=both,legend pos=south east,legend style={font=\\scriptsize},tick label style={font=\\scriptsize},label style={font=\\small}]"]
for name_, color, mark, names_ in curves:
    pts = [pool_small(s_) for s_ in names_]
    pts = [p for p in pts if p]
    if pts:
        plot.append(f"\\addplot+[mark={mark},color={color},thick] coordinates {{{' '.join(f'({p[1]:.1f},{p[0]:.3f})' for p in pts)}}};\n\\addlegendentry{{{name_}}}")
for s_, label_, mark in (("pre-filter", "pre-filter", "diamond*"), ("auto", "\\sys{} auto", "*")):
    p = pool_small(s_)
    if p:
        plot.append(f"\\addplot[only marks,mark={mark},mark size=3pt,color=blue!70!black] coordinates {{({p[1]:.1f},{p[0]:.3f})}};\n\\addlegendentry{{{label_}}}")
plot.append("\\end{axis}\n\\end{tikzpicture}")
write("budget-plot.tex", "\n".join(plot) + "\n")

# --- the recall figure: outside queries
series = [("faiss-ivf", "FAISS IVF", "red!70!black", "square*"), ("faiss-hnsw", "FAISS HNSW", "orange!80!black", "triangle*"), ("auto", "\\sys{} auto", "blue!70!black", "*")]
plot = ["\\begin{tikzpicture}\n\\begin{axis}[width=\\columnwidth,height=4.8cm,symbolic x coords={<=10,11-100,101-1000,1001-5000,>5000},xtick=data,xticklabels={$\\le$10,11--100,101--1k,1--5k,$>$5k},xlabel={classes below $c$},ylabel={recall@10},ymin=0,ymax=1.05,grid=both,legend pos=south east,legend style={font=\\scriptsize},tick label style={font=\\scriptsize},label style={font=\\small}]"]
for s, name, color, mark in series:
    pts = " ".join(f"({b},{pooled(b, 'outside', s)[0]:.3f})" for b in BUCKETS)
    plot.append(f"\\addplot+[mark={mark},color={color},thick] coordinates {{{pts}}};\n\\addlegendentry{{{name}}}")
plot.append("\\end{axis}\n\\end{tikzpicture}")
write("recall-plot.tex", "\n".join(plot) + "\n")

# --- how much of the unfiltered top 100 lies under the class
out = ["\\begin{table}[t]\n\\caption{Share of a query's unfiltered top 100 neighbors that lie under the class, by query kind and number of classes below $c$ (pooled mean; the independence estimate would be the fraction of terms below $c$).}\n\\label{tab-correlation}\n\\small\n\\begin{tabular}{@{}lrrrr@{}}\n\\toprule\nBelow $c$ & independent & inside & outside & synonym \\\\\n\\midrule"]
for b in BUCKETS:
    cells = []
    for kind in ("inside", "outside", "synonym"):
        rows = [x[2] for k, _ in ONT for x in hyb.get((k, b, kind, "auto"), [])]
        cells.append(f"{st.mean(rows):.2f}" if rows else "---")
    frac = []
    for k, _ in ONT:
        n = int(re.search(r"(\d+) vectors", (raw / f"{k}-order.md").read_text()).group(1))
        frac += [float(r["members"]) / n for r in tsv(raw / f"{k}-hybrid.tsv") if r["bucket"] == b and r["strategy"] == "pre-filter"]
    out.append(f"{BUCKET_TEX[b]} & {st.mean(frac):.3f} & " + " & ".join(cells) + " \\\\")
out.append("\\bottomrule\n\\end{tabular}\n\\end{table}\n")
write("correlation.tex", "\n".join(out))

# --- resolving the filter against testing it
out = ["\\begin{table}[t]\n\\caption{Masked HNSW with the filter tested per row against the class's intervals, and with the filter resolved first into a bitmap by range scans (resolving is timed): microseconds per query, pooled over the ontologies.}\n\\label{tab-resolve}\n\\small\n\\begin{tabular}{@{}lrrr@{}}\n\\toprule\nBelow $c$ & interval test & resolve, then bitmap & ratio \\\\\n\\midrule"]
for b in BUCKETS:
    a = [x[1] for k, _ in ONT for kind in ("inside", "outside") for x in hyb.get((k, b, kind, "mask/hnsw"), [])]
    r = [x[1] for k, _ in ONT for kind in ("inside", "outside") for x in hyb.get((k, b, kind, "mask-hnsw/bitmap"), [])]
    if a and r:
        ma, mr = st.mean(a), st.mean(r)
        out.append(f"{BUCKET_TEX[b]} & {us(ma)} & {us(mr)} & {mr / ma:.1f}$\\times$ \\\\")
out.append("\\bottomrule\n\\end{tabular}\n\\end{table}\n")
write("resolve.tex", "\n".join(out))

# --- the threshold of the auto rule
thr = [256, 1024, 4096, 16384, 65536]
out = [f"\\begin{{table}}[t]\n\\caption{{The auto rule at other thresholds: mean microseconds per query over every class size and query kind (all strategies return recall at least 0.99 except as noted).}}\n\\label{{tab-threshold}}\n\\small\n\\begin{{tabular}}{{@{{}}l{'r' * len(ONT)}@{{}}}}\n\\toprule\nThreshold & " + " & ".join(n for _, n in ONT) + " \\\\\n\\midrule"]
means = {}
for t_ in thr:
    for k, _ in ONT:
        rows = [x for b in BUCKETS for kind in ("inside", "outside", "synonym") for x in hyb.get((k, b, kind, f"auto@{t_}"), [])]
        means[(t_, k)] = st.mean(x[1] for x in rows) if rows else None
for t_ in thr:
    cells = []
    for k, _ in ONT:
        v = means[(t_, k)]
        best = min(m for (tt, kk), m in means.items() if kk == k and m is not None)
        text = us(v) if v is not None else "---"
        if v is not None and v == best:
            text = f"\\cellcolor{{ours}}\\textbf{{{text}}}"
        cells.append(text)
    out.append(f"{t_:,} & " + " & ".join(cells) + " \\\\")
out.append("\\bottomrule\n\\end{tabular}\n\\end{table}\n")
write("threshold.tex", "\n".join(out))

# --- the indexes: ours against FAISS and hnswlib at matched settings, and what they cost
idx = {}
for k, _ in ONT:
    for r in tsv(raw / f"{k}-index.tsv") + tsv(raw / f"{k}-faiss-index.tsv"):
        idx[(k, r["index"], r["setting"])] = r
out = ["\\begin{table*}[t]\n\\caption{Unfiltered ten-nearest-neighbor search, 1,000 queries: recall@10 / microseconds per query. FAISS is called once per query from Python, like \\sys{} (its call overhead, about 10\\,$\\mu$s, is included); batched numbers are in the appendix. IVF: nprobe 16; HNSW: $M{=}16$, $\\mathit{ef}{=}40$. Build seconds in parentheses.}\n\\label{tab-index}\n\\small\n\\begin{tabular}{@{}lccccccc@{}}\n\\toprule\nOntology & terms & flat (\\sys{}) & flat (FAISS) & IVF (\\sys{}) & IVF (FAISS) & HNSW (\\sys{}) & HNSW (FAISS) \\\\\n\\midrule"]
for k, name in ONT:
    n = int(re.search(r"(\d+) vectors", (raw / f"{k}-order.md").read_text()).group(1))
    def cell(index, setting):
        r = idx.get((k, index, setting))
        if r is None:
            return "---"
        build = f" ({float(r['build_s']):.0f}\\,s)" if r.get("build_s") and float(r["build_s"]) > 0 else ""
        return f"{float(r['recall']):.2f} / {us(float(r['us']))}{build}"
    out.append(f"{name} & {n:,} & {cell('flat', 'exact')} & {cell('faiss-flat-1', 'exact')} & {cell('ivf', 'nprobe=16')} & {cell('faiss-ivf-1', 'nprobe=16')} & {cell('hnsw', 'ef=40')} & {cell('faiss-hnsw-1', 'ef=40')} \\\\")
out.append("\\bottomrule\n\\end{tabular}\n\\end{table*}\n")
write("index.tex", "\n".join(out))
# the same, batched, with hnswlib
out = ["\\begin{table*}[t]\n\\caption{Unfiltered search with FAISS and hnswlib answering the 1,000 queries in one batch on one thread: recall@10 / microseconds per query. The flat scan is a matrix product, which one-query-at-a-time search cannot use.}\n\\label{tab-index-batched}\n\\small\n\\begin{tabular}{@{}lccccc@{}}\n\\toprule\nOntology & flat & IVF (nprobe 16) & HNSW ($\\mathit{ef}{=}40$), FAISS & HNSW ($\\mathit{ef}{=}40$), hnswlib & IVF (nprobe 64) \\\\\n\\midrule"]
for k, name in ONT:
    def cell2(index, setting):
        r = idx.get((k, index, setting))
        return f"{float(r['recall']):.2f} / {us(float(r['us']))}" if r else "---"
    out.append(f"{name} & {cell2('faiss-flat', 'exact')} & {cell2('faiss-ivf', 'nprobe=16')} & {cell2('faiss-hnsw', 'ef=40')} & {cell2('hnswlib', 'ef=40')} & {cell2('faiss-ivf', 'nprobe=64')} \\\\")
out.append("\\bottomrule\n\\end{tabular}\n\\end{table*}\n")
write("index-batched.tex", "\n".join(out))
# --- another embedding model: pooled over classes of every size, queries of every kind
mp = [(k, n) for k, n in (("go-mpnet", "GO"), ("hpo-mpnet", "HPO")) if (raw / f"{k}-hybrid.tsv").exists() and (raw / f"{k}-faiss-hybrid.tsv").exists()]
if mp:
    rows_by = defaultdict(list)
    for k, _ in mp:
        for r in tsv(raw / f"{k}-hybrid.tsv") + tsv(raw / f"{k}-faiss-hybrid.tsv"):
            rows_by[(k, r["strategy"])].append((float(r["recall"]), float(r["us"]), r["bucket"], r["kind"], float(r.get("under_share") or 0)))
    out = ["\\begin{table}[t]\n\\caption{A second embedding model, \\texttt{all-mpnet-base-v2} (768 dimensions): recall@10 / mean microseconds over every class size and query kind, and the share of the unfiltered top 100 under the class for queries inside and outside.}\n\\label{tab-model}\n\\small\n\\resizebox{\\columnwidth}{!}{%\n\\begin{tabular}{@{}lrrrrrr@{}}\n\\toprule\nOntology & FAISS IVF & FAISS HNSW & pre-filter & auto & top-100 under $c$, inside & outside \\\\\n\\midrule"]
    for k, name in mp:
        cells = []
        for s_ in ("faiss-ivf", "faiss-hnsw", "pre-filter", "auto"):
            v = rows_by[(k, s_)]
            cells.append(f"{st.mean(x[0] for x in v):.2f} / {us(st.mean(x[1] for x in v))}")
        auto = rows_by[(k, "auto")]
        ins = st.mean(x[4] for x in auto if x[3] == "inside")
        outs = st.mean(x[4] for x in auto if x[3] == "outside")
        out.append(f"{name} & " + " & ".join(cells) + f" & {ins:.2f} & {outs:.2f} \\\\")
    out.append("\\bottomrule\n\\end{tabular}}\n\\end{table}\n")
    write("model.tex", "\n".join(out))
# --- why an ID-selector IVF fails: members of the class in the lists it probes
mech = defaultdict(list)
for k, _ in ONT:
    p = raw / f"{k}-mechanism.tsv"
    if p.exists():
        for r in tsv(p):
            mech[(r["bucket"], r["kind"])].append((int(r["members"]), int(r["members_in_probed_lists"])))
if mech:
    out = ["\\begin{table}[t]\n\\caption{Why an ID-selector IVF loses recall: the members of the class that lie in the 16 lists it probes, against the ten the answer needs (mean over the queries, pooled over the four ontologies).}\n\\label{tab-mechanism}\n\\small\n\\begin{tabular}{@{}lrrr@{}}\n\\toprule\nBelow $c$ & members & in probed lists, inside & outside \\\\\n\\midrule"]
    for b in BUCKETS:
        ins = mech[(b, "inside")]
        outs = mech[(b, "outside")]
        out.append(f"{BUCKET_TEX[b]} & {st.mean(x[0] for x in ins):,.0f} & {st.mean(x[1] for x in ins):,.1f} & {st.mean(x[1] for x in outs):,.1f} \\\\")
    out.append("\\bottomrule\n\\end{tabular}\n\\end{table}\n")
    write("mechanism.tex", "\n".join(out))
# --- the number of neighbors asked for, on the Gene Ontology
ks = [(1, "go-k1"), (10, "go"), (50, "go-k50")]
if all((raw / f"{k}-faiss-hybrid.tsv").exists() for _, k in ks):
    out = ["\\begin{table}[t]\n\\caption{The number of neighbors asked for, on the Gene Ontology: recall / mean microseconds over every class size and query kind.}\n\\label{tab-k}\n\\small\n\\begin{tabular}{@{}lrrrr@{}}\n\\toprule\n$k$ & FAISS IVF & FAISS HNSW & pre-filter & auto \\\\\n\\midrule"]
    for kk, name_ in ks:
        rows_by = defaultdict(list)
        for r in tsv(raw / f"{name_}-hybrid.tsv") + tsv(raw / f"{name_}-faiss-hybrid.tsv"):
            rows_by[r["strategy"]].append((float(r["recall"]), float(r["us"])))
        cells = [f"{st.mean(x[0] for x in rows_by[s_]):.2f} / {us(st.mean(x[1] for x in rows_by[s_]))}" for s_ in ("faiss-ivf", "faiss-hnsw", "pre-filter", "auto")]
        out.append(f"{kk} & " + " & ".join(cells) + " \\\\")
    out.append("\\bottomrule\n\\end{tabular}\n\\end{table}\n")
    write("k.tex", "\n".join(out))
print("wrote", ", ".join(sorted(p.name for p in gen.glob("*.tex"))))

# --- per ontology: the answer quality and latency of the rule and of FAISS
out = ["\\begin{table}[t]\n\\caption{Per ontology, over every class size and query kind: recall@10, and mean / median microseconds per query.}\n\\label{tab-per-ontology}\n\\small\n\\resizebox{\\columnwidth}{!}{%\n\\begin{tabular}{@{}lrrrrr@{}}\n\\toprule\nOntology & queries & auto & FAISS flat & FAISS IVF & FAISS HNSW \\\\\n\\midrule"]
for k, name in ONT:
    cells = []
    for s in ("auto", "faiss-flat", "faiss-ivf", "faiss-hnsw"):
        rows = [x for (kk, b, kind, ss), v in hyb.items() if kk == k and ss == s for x in v]
        cells.append((st.mean(x[0] for x in rows), st.mean(x[1] for x in rows), st.median(x[1] for x in rows), len(rows)))
    out.append(f"{name} & {cells[0][3]} & " + " & ".join(f"{c[0]:.2f} / {us(c[1])} / {us(c[2])}" for c in cells) + " \\\\")
out.append("\\bottomrule\n\\end{tabular}}\n\\end{table}\n")
write("per-ontology.tex", "\n".join(out))

# --- a wider FAISS budget: recall for queries outside small classes
out = ["\\begin{table}[t]\n\\caption{FAISS with an ID selector at its default budget and at a much larger one (half of all IVF lists; HNSW beam 1,024): recall@10 / microseconds, queries outside the class, pooled.}\n\\label{tab-wide}\n\\small\n\\resizebox{\\columnwidth}{!}{%\n\\begin{tabular}{@{}lrrrr@{}}\n\\toprule\nBelow $c$ & IVF & IVF, half the lists & HNSW & HNSW, beam 1,024 \\\\\n\\midrule"]
for b in BUCKETS:
    cells = []
    for s in ("faiss-ivf", "faiss-ivf-wide", "faiss-hnsw", "faiss-hnsw-wide"):
        v = pooled(b, "outside", s)
        cells.append(f"{v[0]:.2f} / {us(v[1])}" if v else "---")
    out.append(f"{BUCKET_TEX[b]} & " + " & ".join(cells) + " \\\\")
out.append("\\bottomrule\n\\end{tabular}}\n\\end{table}\n")
write("wide.tex", "\n".join(out))
print("wrote per-ontology and wide")

# --- latency against class size: a log-scale figure
series = [("faiss-flat", "FAISS exact + selector", "black!60", "pentagon*"), ("faiss-hnsw", "FAISS HNSW + selector", "orange!80!black", "triangle*"),
          ("pre-filter", "pre-filter", "green!50!black", "diamond*"), ("mask/hnsw", "masked HNSW", "red!70!black", "square*"), ("auto", "auto", "blue!70!black", "*")]
plot = ["\\begin{tikzpicture}\n\\begin{axis}[width=\\columnwidth,height=5.2cm,ymode=log,symbolic x coords={<=10,11-100,101-1000,1001-5000,>5000},xtick=data,xticklabels={$\\le$10,11--100,101--1k,1--5k,$>$5k},xlabel={classes below $c$},ylabel={$\\mu$s per query},grid=both,legend style={font=\\scriptsize,at={(0.5,1.02)},anchor=south,legend columns=2},tick label style={font=\\scriptsize},label style={font=\\small}]"]
for s, name, color, mark in series:
    pts = []
    for b in BUCKETS:
        vals = [x[1] for k, _ in ONT for kind in ("inside", "outside", "synonym") for x in hyb.get((k, b, kind, s), [])]
        pts.append(f"({b},{st.mean(vals):.1f})")
    plot.append(f"\\addplot+[mark={mark},color={color},thick] coordinates {{{' '.join(pts)}}};\n\\addlegendentry{{{name}}}")
plot.append("\\end{axis}\n\\end{tikzpicture}")
write("latency-plot.tex", "\n".join(plot) + "\n")

# --- threads
scale = {}
for k, _ in ONT:
    p1, p2 = raw / f"{k}-scale.tsv", raw / f"{k}-faiss-scale.tsv"
    if p1.exists() and p2.exists():
        for r in tsv(p1):
            scale[(k, "auto", int(r["threads"]))] = float(r["queries_per_s"])
        for r in tsv(p2):
            scale[(k, r["index"], int(r["threads"]))] = float(r["queries_per_s"])
have = [(k, n) for k, n in ONT if (k, "auto", 1) in scale]
if have:
    out = ["\\begin{table}[t]\n\\caption{Filtered queries per second on one to eight threads over the whole workload: the auto rule (C++ threads, one shared index) against FAISS's exact scan and HNSW with an ID selector (Python threads; FAISS releases the interpreter lock while it searches).}\n\\label{tab-scale}\n\\small\n\\resizebox{\\columnwidth}{!}{%\n\\begin{tabular}{@{}lrrrrrr@{}}\n\\toprule\n & \\multicolumn{2}{c}{auto} & \\multicolumn{2}{c}{FAISS exact} & \\multicolumn{2}{c}{FAISS HNSW} \\\\\n\\cmidrule(lr){2-3}\\cmidrule(lr){4-5}\\cmidrule(lr){6-7}\nOntology & 1 & 8 & 1 & 8 & 1 & 8 \\\\\n\\midrule"]
    def f(v):
        return f"{v / 1000:.0f}k" if v >= 10000 else f"{v:,.0f}"
    for k, name in have:
        vals = [scale[(k, s, t_)] for s in ("auto", "faiss-flat", "faiss-hnsw") for t_ in (1, 8)]
        out.append(f"{name} & " + " & ".join(f(v) for v in vals) + " \\\\")
    out.append("\\bottomrule\n\\end{tabular}}\n\\end{table}\n")
    write("scale.tex", "\n".join(out))
    print("scale table written")
