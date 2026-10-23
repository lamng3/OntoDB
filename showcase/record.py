#!/usr/bin/env python3
"""Record what the real OntoDB shell prints for the showcase, into showcase/recording.json.

    python3 showcase/record.py            # ONTODB=/path/to/ontodb overrides build/release/ontodb

Needs LUBM(1) (`scripts/fetch-data lubm`) and the Gene Ontology slice (`scripts/web-slice`).
Every number and key on the page comes from the shell's own output: \\explain analyze, \\tree, \\bpm,
\\trace, \\log, \\verify. This script only parses that output and checks it against itself.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
BINARY = os.environ.get("ONTODB", str(ROOT / "build" / "release" / "ontodb"))
LUBM = ROOT / "data" / "downloads" / "lubm" / "University0_0.owl"
QUERIES = ROOT / "data" / "lubm" / "queries"
TINY = ROOT / "data" / "tiny.ttl"
GO = ROOT / "web" / "data" / "go-slice.ttl"
GO_VEC = ROOT / "web" / "data" / "go-slice.ovec"
PROMPT = "OntoDB> "

RDF = "http://www.w3.org/1999/02/22-rdf-syntax-ns#"
UB = "http://www.lehigh.edu/~zhp2/2004/0401/univ-bench.owl#"
WALK_QUERY = (
    f"PREFIX rdf: <{RDF}> PREFIX ub: <{UB}> SELECT ?X WHERE {{ ?X rdf:type ub:GraduateStudent . "
    "?X ub:takesCourse <http://www.Department0.University0.edu/GraduateCourse0> }"
)
WALK_TERMS = [RDF + "type", UB + "GraduateStudent", UB + "takesCourse",
              "http://www.Department0.University0.edu/GraduateCourse0"]  # fmt: skip
PERMS = ("spo", "pos", "osp")
ORDER = {"spo": (0, 1, 2), "pos": (1, 2, 0), "osp": (2, 0, 1)}


def scrub(text: str, work: str = "") -> str:
    """Local paths stay off the page: the loader's base IRI and the scratch database directory."""
    for d in filter(None, (work, os.path.realpath(work) if work else "")):
        text = text.replace(d + "/", "")
    return text.replace(str(ROOT), "…/ontodb")


def run(lines: list[str], db: str | None = None) -> tuple[list[str], str]:
    """One shell process fed `lines`; returns each command's output and everything written to stderr."""
    args = [BINARY] + ([db] if db else [])
    proc = subprocess.run(args, input="\n".join(lines + ["\\quit"]) + "\n", capture_output=True, text=True, check=True)
    chunks = proc.stdout.split(PROMPT)
    return [c.rstrip("\n") for c in chunks[1 : len(lines) + 1]], proc.stderr


class Session:
    """A shell kept open between commands, so the files on disk can be read after each one."""

    def __init__(self, db: str):
        self.proc = subprocess.Popen([BINARY, db], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.banner = self._read()

    def _read(self) -> str:
        buf = b""
        while not buf.endswith(PROMPT.encode()):
            chunk = os.read(self.proc.stdout.fileno(), 65536)
            if not chunk:
                break
            buf += chunk
        return buf.decode().removesuffix(PROMPT).rstrip("\n")

    def __call__(self, line: str) -> str:
        self.proc.stdin.write((line + "\n").encode())
        self.proc.stdin.flush()
        return self._read()

    def close(self, line: str = "\\quit") -> str:
        self.proc.stdin.write((line + "\n").encode())
        self.proc.stdin.flush()
        out = self.proc.stdout.read().decode()
        self.proc.wait()
        return out.rstrip("\n")


def lubm_queries() -> list[tuple[str, str]]:
    """The published LUBM queries made parseable, as benchmarks/compare/prepare.py does."""
    out = []
    for path in sorted(QUERIES.glob("q*.sparql")):
        lines = path.read_text().splitlines()
        note = " ".join(l.lstrip("# ").strip() for l in lines if l.lstrip().startswith("#"))
        text = "\n".join(l for l in lines if not l.lstrip().startswith("#"))
        head, sep, body = text.partition("WHERE")
        text = head.replace(",", " ") + sep + body
        text = re.sub(r'(?<![<"])(http://[^\s}<>"]+)', r"<\1>", text)
        out.append((path.stem, " ".join(text.split()), note))
    return out


# ---- parsing what the shell prints ----


def parse_plan(text: str) -> dict:
    """`\\explain analyze` output -> a tree of {op, detail, est, actual, probed, kids}, plus the time."""
    root, stack, ms = None, [], None
    for line in text.splitlines():
        if line.startswith("execution:"):
            ms = float(line.split()[1])
            continue
        depth = (len(line) - len(line.lstrip(" "))) // 2
        m = re.match(r"\s*(\w+)\s*(.*?)\s*\(estimated (\d+), (?:actual (\d+)|probed by the join above)\)$", line)
        if not m:
            continue
        node = {"op": m[1], "detail": m[2], "est": int(m[3]),
                "actual": int(m[4]) if m[4] is not None else None, "kids": []}  # fmt: skip
        while len(stack) > depth:
            stack.pop()
        (stack[-1]["kids"] if stack else []).append(node)
        root = root or node
        stack.append(node)
    return {"plan": root, "ms": ms}


def parse_tree(text: str) -> dict:
    """`\\tree <perm>` -> root page and levels of nodes; leaves keep their count and first/last key."""
    root = int(re.search(r"root (\d+)", text)[1])
    levels = []
    for line in text.splitlines():
        if not line.startswith("level"):
            continue
        nodes = []
        for m in re.finditer(r"\[(internal|leaf) (\d+): ([^\]]*)\]", line):
            kind, page, body = m[1], int(m[2]), m[3]
            keys = [tuple(map(int, k.split(","))) for k in re.findall(r"\((\d+,\d+,\d+)\)", body)]
            if kind == "leaf":
                nodes.append({"page": page, "leaf": True, "count": int(body.split()[0]), "first": keys[0], "last": keys[1]})
            else:
                nodes.append({"page": page, "leaf": False, "seps": keys})
        levels.append(nodes)
    return {"root": root, "levels": levels}


def parse_bpm(text: str) -> dict:
    head = re.match(r"buffer pool: (\d+) frames, lru_k (\d+)", text)
    frames = {int(f): int(p) for f, p in re.findall(r"frame (\d+): page (\d+)", text)}
    return {"frames": int(head[1]), "k": int(head[2]), "resident": frames}


def parse_wal(text: str) -> tuple[dict[int, str], list[tuple[int, int, int]]]:
    terms, triples = {}, []
    for line in text.splitlines():
        if m := re.match(r"lsn \d+ txn \d+ term (\d+) (.*)$", line):
            terms[int(m[1])] = m[2]
        elif m := re.match(r"lsn \d+ txn \d+ update insert \((\d+) (\d+) (\d+)\)$", line):
            triples.append((int(m[1]), int(m[2]), int(m[3])))
    return terms, triples


def wal_records(text: str) -> list[dict]:
    out = []
    for line in text.splitlines():
        m = re.match(r"lsn (\d+) txn (\d+) (\w+)\s*(.*)$", line)
        if m:
            out.append({"lsn": int(m[1]), "txn": int(m[2]), "kind": m[3], "body": m[4]})
    return out


def trace_visits(stderr: str) -> list[dict]:
    """Group `\\trace` events into visits: from the first pin until no page is pinned.

    Each visit lists the pages it fetched in order (hit or miss, and the frame a miss used) and
    the evictions the replacer chose to make room."""
    visits, cur, pinned = [], None, {}
    for line in stderr.splitlines():
        comp, _, msg = line.partition(": ")
        if cur is None:
            cur = {"pages": [], "evictions": [], "writes": []}
        if comp == "bpm" and (m := re.match(r"(hit|miss) page (\d+)(?: into frame (\d+))?", msg)):
            page = int(m[2])
            cur["pages"].append({"page": page, "hit": m[1] == "hit", "frame": int(m[3]) if m[3] else None})
            pinned[page] = pinned.get(page, 0) + 1
        elif comp == "bpm" and (m := re.match(r"unpin page (\d+)", msg)):
            page = int(m[1])
            pinned[page] -= 1
            if not pinned[page]:
                del pinned[page]
            if not pinned:
                visits.append(cur)
                cur = None
        elif comp == "bpm" and (m := re.match(r"evict page (\d+) from frame (\d+)", msg)):
            cur["evictions"].append({"page": int(m[1]), "frame": int(m[2])})
        elif comp == "replacer" and (m := re.match(r"evict frame (\d+) \(k-distance (\w+)\)", msg)):
            cur.setdefault("kdist", []).append(m[2])
        elif comp == "bpm" and (m := re.match(r"flush page (\d+)", msg)):
            cur["writes"].append(int(m[1]))
    for v in visits:
        for e, k in zip(v["evictions"], v.pop("kdist", [])):
            e["kdist"] = k
    return visits


# ---- recordings ----


def record_walk() -> dict:
    """LUBM q01 on one university, traced page by page on the indexed backend."""
    db_dir = tempfile.mkdtemp()
    try:  # ids: a persistent database logs every term it assigns, and the loader assigns them in the same order
        out, _ = run([f"\\load {LUBM}", "\\log 1000000"], os.path.join(db_dir, "db"))
    finally:
        shutil.rmtree(db_dir)
    terms, inserted = parse_wal(out[1])
    triples = sorted(set(inserted))

    setup = ["\\set backend indexed", f"\\load {LUBM}"]
    out, _ = run(setup + [f"\\explain analyze {WALK_QUERY}", "\\tree spo", "\\tree pos", "\\tree osp", "\\stats"])
    loaded = out[1]
    plan = parse_plan(out[2])
    trees = {p: parse_tree(t) for p, t in zip(PERMS, out[3:6])}

    keys = {p: sorted(tuple(t[i] for i in ORDER[p]) for t in triples) for p in PERMS}
    for p in PERMS:  # the leaves we draw must be the leaves the engine has
        at = 0
        for leaf in trees[p]["levels"][-1]:
            chunk = keys[p][at : at + leaf["count"]]
            assert (chunk[0], chunk[-1]) == (leaf["first"], leaf["last"]), (p, leaf["page"])
            leaf["keys"] = chunk
            at += leaf["count"]
        assert at == len(keys[p]), p

    out, trace = run(setup + ["\\bpm", "\\trace on", WALK_QUERY, "\\trace off", "\\stats"])
    bpm, rows, stats = parse_bpm(out[2]), out[4], out[6]
    visits = trace_visits(trace)

    # Which term each dictionary lookup was for: trace a query that names only that term, and read the
    # first lookup's pages (spelling-tree descent, terms-tree descent, heap page) as its signature.
    signature = {}
    for term in WALK_TERMS:
        _, t = run(setup + ["\\trace on", f"SELECT ?s ?p WHERE {{ ?s ?p <{term}> }}"])
        first = trace_visits(t)[:3]
        signature[tuple(tuple(x["page"] for x in v["pages"]) for v in first)] = term

    ids = {v: k for k, v in terms.items()}
    roots = {trees[p]["root"]: p for p in PERMS}
    node_of = {n["page"]: (p, i) for p in PERMS for i, lvl in enumerate(trees[p]["levels"]) for n in lvl}
    header = {}
    for v in visits:  # a triple index's header page is the one fetched right before its root
        pages = [x["page"] for x in v["pages"]]
        if len(pages) > 1 and pages[1] in roots:
            header[pages[0]] = roots[pages[1]]
    # The dictionary allocates its heap page, then the terms tree's header, then the spelling tree's
    # (DiskDictionary's constructor), before the store allocates the indexes' headers.
    first_index_header = min(header)
    dict_heap, dict_terms, dict_spelling = first_index_header - 4, first_index_header - 3, first_index_header - 2

    def owner(page: int) -> str:
        if page in node_of or page in header:
            return node_of[page][0] if page in node_of else header[page]
        if page in (dict_terms, dict_spelling):
            return "dict"
        return "dict"  # the dictionary's tree nodes and heap pages are everything else

    steps, i = [], 0
    while i < len(visits):
        v = visits[i]
        first = v["pages"][0]["page"]
        if first == dict_spelling:  # a lookup by spelling: spelling tree, terms tree, heap
            group = visits[i : i + 3]
            sig = tuple(tuple(x["page"] for x in g["pages"]) for g in group)
            term = signature.get(sig)
            steps.append({"kind": "bind", "visits": group, "term": term, "id": ids.get(f"<{term}>")})
            i += 3
        elif first in header:
            steps.append({"kind": "scan", "perm": header[first], "visits": [v]})
            i += 1
        elif first in node_of and steps and steps[-1]["kind"] == "scan":  # the scan moved to the next leaf
            steps[-1]["visits"].append(v)
            i += 1
        elif first == dict_terms:  # id -> spelling: terms tree, then the heap page
            steps.append({"kind": "decode", "visits": visits[i : i + 2]})
            i += 2
        else:
            steps.append({"kind": "other", "visits": [v]})
            i += 1
    assert all(s["term"] for s in steps if s["kind"] == "bind"), "a lookup matched no signature"

    # What each index visit found, computed from the keys of the leaf it ended on.
    takes, course, gtype, gs = (ids[f"<{t}>"] for t in (WALK_TERMS[2], WALK_TERMS[3], WALK_TERMS[0], WALK_TERMS[1]))
    leaf_keys = {n["page"]: n["keys"] for p in PERMS for n in trees[p]["levels"][-1]}
    outer = [k for k in keys["pos"] if k[0] == takes and k[1] == course]
    probes = iter(k[2] for k in outer)
    for s in steps:
        if s["kind"] != "scan":
            continue
        leaves = [v["pages"][-1]["page"] for v in s["visits"]]
        if len(leaves) == len(trees[s["perm"]]["levels"][-1]):  # every leaf: the optimizer's statistics pass
            s["kind"], s["leaves"] = "stats", leaves  # Database collects them on the first query after a change
            continue
        leaf = leaves[0]
        if s["perm"] == "pos":
            s["range"] = [takes, course]
            s["found"] = [k for k in leaf_keys[leaf] if k[:2] == (takes, course)]
            assert s["found"] == outer
        else:
            x = next(probes)
            s["probe"] = [x, gtype, gs]
            s["found"] = [k for k in leaf_keys[leaf] if k == (x, gtype, gs)]
        s["leaf"] = leaf
    row_terms = re.findall(r"<([^>]+)>", rows)
    decodes = [s for s in steps if s["kind"] == "decode"]
    for s, t in zip(decodes, row_terms):
        s["term"] = t
        s["id"] = ids.get(f"<{t}>")

    visited = {s["leaf"] for s in steps if s["kind"] == "scan"}
    for p in PERMS:  # keep the keys of the leaves the query read; the others keep their first and last
        for leaf in trees[p]["levels"][-1]:
            if leaf["page"] not in visited:
                del leaf["keys"]
    shown = {k for p in PERMS for leaf in trees[p]["levels"][-1] for key in leaf.get("keys", ()) for k in key}
    shown |= {x for s in steps for x in (s.get("probe") or []) + (s.get("range") or [])}
    names = {i: scrub(terms[i]) for i in shown if i in terms}
    return {
        "query": WALK_QUERY, "loaded": loaded, "plan": plan, "rows": rows, "trees": trees,
        "bpm": bpm, "steps": steps, "stats_after": stats, "terms": names, "triples": len(triples),
        "pages": {"header": {str(k): v for k, v in header.items()}, "heap": dict_heap,
                  "terms": dict_terms, "spelling": dict_spelling},
        "ids": {"type": gtype, "GraduateStudent": gs, "takesCourse": takes, "GraduateCourse0": course},
    }  # fmt: skip


def record_plans() -> list[dict]:
    queries = lubm_queries()
    out, _ = run(["\\set backend indexed", f"\\load {LUBM}"] + [f"\\explain analyze {q}" for _, q, _ in queries])
    plans = []
    for (name, q, note), text in zip(queries, out[2:]):
        plans.append({"name": name, "query": q, "note": note, **parse_plan(text)})
    return plans


def record_recovery() -> dict:
    """A persistent database: statements, a checkpoint, an abrupt exit, a torn log tail, a restart."""
    work = tempfile.mkdtemp()
    db = os.path.join(work, "mydb")
    U = "PREFIX ub: <http://example.edu/univ#>"
    script = [
        ("load", f"\\load {TINY}"),
        ("insert", f"{U} INSERT DATA {{ ub:Fay a ub:Student . ub:Fay ub:memberOf ub:Math . }}"),
        ("checkpoint", "\\checkpoint"),
        ("insert", f"{U} INSERT DATA {{ ub:Gus a ub:Student . }}"),
        ("insert", f"{U} INSERT DATA {{ ub:Hal a ub:Student . }}"),
        ("delete", f"{U} DELETE DATA {{ ub:Alice a ub:Student . }}"),
    ]
    steps = []
    try:
        s = Session(db)
        steps.append({"kind": "open", "cmd": "ontodb mydb", "out": scrub(s.banner, work), "wal": [], **sizes(db)})
        for kind, cmd in script:
            out = s(cmd)
            steps.append({"kind": kind, "cmd": scrub(cmd), "out": scrub(out, work),
                          "wal": wal_records(s("\\log 200")), **sizes(db)})  # fmt: skip
        s.close("\\crash")
        before = sizes(db)
        wal = os.path.join(db, "wal")
        os.truncate(wal, os.path.getsize(wal) - 10)
        steps.append({"kind": "crash", "cmd": "\\crash", "out": "", "cut": 10, "before": before,
                      "wal": steps[-1]["wal"], **sizes(db)})  # fmt: skip
        s = Session(db)
        restart = {"kind": "restart", "cmd": "ontodb mydb", "out": scrub(s.banner, work), "wal": wal_records(s("\\log 200"))}
        steps.append({**restart, **sizes(db)})
        q = f"{U} SELECT ?s WHERE {{ ?s a ub:Student }}"
        steps.append({"kind": "query", "cmd": scrub(q), "out": s(q), "wal": restart["wal"], **sizes(db)})
        steps.append({"kind": "verify", "cmd": "\\verify", "out": s("\\verify"), "wal": restart["wal"], **sizes(db)})
        s.close()
    finally:
        shutil.rmtree(work)
    return {"steps": steps}


def sizes(db: str) -> dict:
    get = lambda name: os.path.getsize(os.path.join(db, name)) if os.path.exists(os.path.join(db, name)) else 0
    return {"wal_bytes": get("wal"), "snapshot_bytes": get("snapshot")}


def record_semantic() -> dict:
    GOI = "http://purl.obolibrary.org/obo/GO_"
    lipid, fatty, retinoid = f"<{GOI}0006629>", f"<{GOI}0006631>", f"<{GOI}0001523>"
    out, _ = run([
        "\\set backend indexed", f"\\load {GO}", "\\hindex build", f"\\under {lipid} 12", f"\\under {fatty} 12",
        f"\\vload {GO_VEC}", "\\vindex build all", f"\\similar {retinoid} 6", f"\\similar {retinoid} 6 under {fatty}",
    ])  # fmt: skip
    return {"loaded": out[1], "hindex": out[2], "under": [out[3], out[4]], "vload": out[5], "vindex": out[6],
            "similar": out[7], "similar_under": out[8]}  # fmt: skip


def main() -> int:
    for path in (LUBM, GO, GO_VEC):
        if not path.exists():
            print(f"missing {path}: run scripts/fetch-data lubm and scripts/web-slice", file=sys.stderr)
            return 1
    version = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    data = {"commit": version, "walk": record_walk(), "plans": record_plans(),
            "recovery": record_recovery(), "semantic": record_semantic()}  # fmt: skip
    (HERE / "recording.json").write_text(json.dumps(data, separators=(",", ":")) + "\n")
    print(f"wrote {HERE / 'recording.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
