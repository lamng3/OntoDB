#!/usr/bin/env python3
"""Build showcase/index.html from showcase/recording.json and benchmarks/compare/results.md, so it opens with no server.

    python3 showcase/record.py   # runs the shell; needs LUBM(1) and the GO slice
    python3 showcase/build.py    # writes showcase/index.html

The page draws what the shell printed and what the benchmark recorded; nothing is measured in the browser.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
RESULTS = ROOT / "benchmarks" / "compare" / "results.md"


def tables(markdown: str) -> dict[str, list[list[dict]]]:
    """Every pipe table in the file, as rows of {header: cell}, grouped under the `##` heading above it."""
    out, heading, block = {}, "", []

    def flush():
        if len(block) > 2:
            head = [c.strip() for c in block[0].strip("|").split("|")]
            rows = [dict(zip(head, (c.strip() for c in line.strip("|").split("|")))) for line in block[2:]]
            out.setdefault(heading, []).append(rows)
        block.clear()

    for line in markdown.splitlines():
        if line.startswith("## "):
            flush()
            heading = line[3:].strip()
        elif line.startswith("|"):
            block.append(line)
        else:
            flush()
    flush()
    return out


def micros(cell: str) -> float | None:
    """'8.2 ms' / '9 µs' / '1.5 ms' -> microseconds."""
    m = re.match(r"([\d.,]+)\s*(µs|ms|s)$", cell.strip())
    if not m:
        return None
    return float(m[1].replace(",", "")) * {"µs": 1, "ms": 1e3, "s": 1e6}[m[2]]


def bench() -> dict:
    t = tables(RESULTS.read_text(encoding="utf-8"))
    head = RESULTS.read_text(encoding="utf-8").split("\n\n")[1].strip()
    lubm10 = t["LUBM(10)"]
    engines = [r["Engine"] for r in lubm10[0]]
    load = [{"engine": r["Engine"], "triples": r["triples"], "load": r["load (s)"], "disk": r["on disk (MiB)"],
             "rss": r["peak RSS (MiB)"]} for r in lubm10[0]]  # fmt: skip
    queries = [{"name": r["Query"], "rows": int(r["rows"]), "us": {e: micros(r[e]) for e in engines}} for r in lubm10[1]]
    go_rows = t["Instances of a class and its subclasses (Gene Ontology, 200,000 entities)"][0]
    go = [{"bucket": r["Below C"], "answers": r["answers"],
           "us": {"OntoDB ids": micros(r["OntoDB ids"]), "OntoDB": micros(r["OntoDB strings"]),
                  "Oxigraph (RocksDB)": micros(r["Oxigraph (RocksDB)"]), "Oxigraph (memory)": micros(r["Oxigraph (memory)"])}}
          for r in go_rows]  # fmt: skip
    conc = []
    for r in t["Concurrency (LUBM(10), 3 s per cell, 1 / 2 / 4 / 8 threads)"][0]:
        nums = lambda cell: [int(x.replace(",", "")) for x in cell.split("/")]
        conc.append({"engine": r["Engine"], "lookups": nums(r["lookups/s, 1 / 2 / 4 / 8 threads"]),
                     "inserts": nums(r["inserts/s, 1 / 2 / 4 / 8"])})  # fmt: skip
    return {"machine": head, "engines": engines, "load": load, "queries": queries, "go": go, "conc": conc}


def main() -> int:
    data = json.loads((HERE / "recording.json").read_text(encoding="utf-8"))
    data["bench"] = bench()
    blob = json.dumps(data, ensure_ascii=False, separators=(",", ":")).replace("</", "<\\/")
    page = (HERE / "template.html").read_text(encoding="utf-8")
    page = page.replace("/*BASE_CSS*/", (HERE / "base.css").read_text(encoding="utf-8"))
    page = page.replace("/*BASE_JS*/", (HERE / "base.js").read_text(encoding="utf-8"))
    page = page.replace("__DATA__", blob)
    (HERE / "index.html").write_text(page, encoding="utf-8")
    print(f"wrote {HERE / 'index.html'} ({len(page) // 1024} KB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
