#!/usr/bin/env python3
"""Turn benchmarks/semantic/raw/knn.md into pgfplots coordinates (gen/knn-plot.tex).
The raw tables are not committed; the generated file is, so the paper builds alone."""
import re
from pathlib import Path

root = Path(__file__).resolve().parents[1]
rows = []
for line in (root / "benchmarks/semantic/raw/knn.md").read_text().splitlines():
    cells = [c.strip() for c in line.strip("|").split("|")]
    if len(cells) == 6 and re.match(r"^[0-9.]+$", cells[2]):
        rows.append((cells[0], cells[1], float(cells[2]), float(cells[3])))

series = [
    ("HNSW $M{=}16$", "hnsw M=16", "mark=*,color=blue!70!black"),
    ("IVF $L{=}512$", "ivf L=512 float32", "mark=square*,color=red!70!black"),
    ("IVF $L{=}512$, sq8", "ivf L=512 sq8", "mark=triangle*,color=orange!80!black"),
    ("IVF $L{=}512$, pq96", "ivf L=512 pq96", "mark=diamond*,color=green!50!black"),
]
out = []
out.append("\\begin{tikzpicture}\n\\begin{axis}[width=\\columnwidth,height=6.2cm,xmode=log,xlabel={$\\mu$s per query (log)},ylabel={recall@10},ymin=0.6,ymax=1.005,grid=both,legend pos=south east,legend style={font=\\scriptsize},tick label style={font=\\scriptsize},label style={font=\\small}]")
for label, key, style in series:
    pts = [(us, r) for name, _, r, us in rows if name == key]
    out.append(f"\\addplot+[{style},thick] coordinates {{" + " ".join(f"({us:g},{r:g})" for us, r in pts) + "};")
    out.append(f"\\addlegendentry{{{label}}}")
flat = [(us, r) for name, _, r, us in rows if name == "flat float32"][0]
out.append(f"\\addplot[only marks,mark=pentagon*,mark size=3pt,color=black] coordinates {{({flat[0]:g},{flat[1]:g})}};")
out.append("\\addlegendentry{flat, exact}")
out.append("\\end{axis}\n\\end{tikzpicture}")
(root / "paper/gen/knn-plot.tex").write_text("\n".join(out) + "\n")
print("wrote gen/knn-plot.tex with", sum(1 for r in rows), "rows")


# ---- the order experiment: share of the 10 nearest neighbors within +-W positions ----
orders = []
for line in (root / "benchmarks/semantic/raw/order.md").read_text().splitlines():
    cells = [c.strip() for c in line.strip("|").split("|")]
    if len(cells) == 8 and re.match(r"^[0-9.]+$", cells[2]):
        orders.append((cells[0], [float(x) for x in cells[2:]]))
windows = [10, 50, 100, 500, 1000, 5000]
styles = {
    "dictionary id (first seen)": ("dictionary id", "mark=square*,color=red!70!black"),
    "hierarchy, deepest parent": ("hierarchy postorder", "mark=*,color=blue!70!black"),
    "label, alphabetical": ("label, alphabetical", "mark=triangle*,color=orange!80!black"),
    "random": ("random", "mark=x,color=gray"),
}
out = ["\\begin{tikzpicture}\n\\begin{axis}[width=\\columnwidth,height=5.6cm,xmode=log,xlabel={window $\\pm W$ (positions)},ylabel={neighbors found},ymin=0,ymax=1,grid=both,legend pos=north west,legend style={font=\\scriptsize},tick label style={font=\\scriptsize},label style={font=\\small}]"]
for name, vals in orders:
    if name in styles:
        label, style = styles[name]
        out.append(f"\\addplot+[{style},thick] coordinates {{" + " ".join(f"({w},{v:g})" for w, v in zip(windows, vals)) + "};")
        out.append(f"\\addlegendentry{{{label}}}")
out.append("\\end{axis}\n\\end{tikzpicture}")
(root / "paper/gen/order-plot.tex").write_text("\n".join(out).replace("\\\\", "\\") + "\n")
print("wrote gen/order-plot.tex")
