#!/usr/bin/env python3
"""SQLite as a triple store: the relational baseline of the comparison (benchmarks/compare).

    sqlite_bench.py <work dir> <queries.tsv> <runs> <rdf file>...
    sqlite_bench.py <work dir> types <classes.tsv> <runs> <rdf file>...

One table t(s, p, o) of N-Triples spellings, WITHOUT ROWID with primary key (s, p, o) and two more
indexes (p, o, s) and (o, s, p): the same three permutations as OntoDB, with the terms stored as
strings and not as dictionary ids. A SPARQL basic graph pattern is translated to a join of t with
itself, one alias per pattern, and SQLite's planner (after ANALYZE) orders it. Only the shapes the
benchmark uses are translated: PREFIX, SELECT, and a group of triple patterns with `a`.
The output format is the one of ontodb_bench_compare."""
import os
import re
import resource
import shutil
import sqlite3
import statistics
import sys
import time
from pathlib import Path

from pyoxigraph import RdfFormat, parse

TYPE = "<http://www.w3.org/1999/02/22-rdf-syntax-ns#type>"
SUBCLASS = "<http://www.w3.org/2000/01/rdf-schema#subClassOf>"


def translate(text):
    """SPARQL (a BGP with PREFIX and SELECT) to (sql, parameters)."""
    prefixes = dict(re.findall(r"PREFIX\s+(\w*):\s*<([^>]*)>", text))
    head = re.search(r"SELECT\s+(DISTINCT\s+)?(.*?)\s*WHERE\s*\{(.*)\}\s*$", text, re.S | re.I)
    distinct, select, body = head.group(1), head.group(2).strip(), head.group(3)
    triples = [t.strip() for t in re.split(r"\s\.(?:\s|$)|\.\s*$", body.strip()) if t.strip()]
    def term(tok):
        if tok == "a":
            return TYPE
        if tok.startswith("<") or tok.startswith("?"):
            return tok
        pre, local = tok.split(":", 1)
        return f"<{prefixes[pre]}{local}>"
    tables, where, params, column = [], [], [], {}
    for i, t in enumerate(triples):
        toks = re.findall(r"<[^>]*>|\S+", t)
        for pos, tok in zip("spo", [term(x) for x in toks]):
            if tok.startswith("?"):
                if tok in column:
                    where.append(f"t{i}.{pos} = {column[tok]}")
                else:
                    column[tok] = f"t{i}.{pos}"
            else:
                where.append(f"t{i}.{pos} = ?")
                params.append(tok)
        tables.append(f"t AS t{i}")
    cols = list(column) if select == "*" else re.findall(r"\?\w+", select)
    sql = f"SELECT {'DISTINCT ' if distinct else ''}{', '.join(column[c] for c in cols)} FROM {', '.join(tables)}"
    if where:
        sql += " WHERE " + " AND ".join(where)
    return sql, params


def types_sql():
    return (
        "WITH RECURSIVE sub(c) AS (SELECT ? UNION SELECT s FROM t, sub WHERE t.p = ? AND t.o = sub.c) "
        "SELECT DISTINCT s FROM t WHERE p = ? AND o IN (SELECT c FROM sub)"
    ), None


DEADLINE = [0.0]


def run(con, sql, params):
    t0 = time.perf_counter()
    DEADLINE[0] = t0 + 60  # a query that runs for a minute is reported as a timeout
    n = 0
    size = 0
    for row in con.execute(sql, params):
        n += 1
        for v in row:
            size += len(v)
    return n, (time.perf_counter() - t0) * 1e6


def timed(con, sql, params, runs):
    try:
        return _timed(con, sql, params, runs)
    except sqlite3.OperationalError as e:
        if "interrupt" in str(e):
            return -1, "timeout"
        raise


def _timed(con, sql, params, runs):
    n, first = run(con, sql, params)
    for _ in range(2):
        run(con, sql, params)
    reps = 1 if first > 2e6 else runs
    times = [run(con, sql, params)[1] for _ in range(reps)]
    return n, f"{statistics.median(times):.1f}"


def main():
    a = sys.argv[1:]
    work = Path(a[0])
    types = a[1] == "types"
    queries = a[2] if types else a[1]
    runs = int(a[3] if types else a[2])
    files = a[4:] if types else a[3:]
    work.mkdir(parents=True, exist_ok=True)
    db = work / "triples.sqlite"
    if db.exists():
        db.unlink()
    t0 = time.perf_counter()
    con = sqlite3.connect(db)
    con.execute("PRAGMA journal_mode = OFF")
    con.execute("PRAGMA synchronous = OFF")
    con.execute("CREATE TABLE t (s TEXT NOT NULL, p TEXT NOT NULL, o TEXT NOT NULL, PRIMARY KEY (s, p, o)) WITHOUT ROWID")
    for f in files:
        fmt = RdfFormat.N_TRIPLES if f.endswith(".nt") else RdfFormat.RDF_XML
        base = Path(f).resolve().as_uri()
        batch = []
        for q in parse(path=f, format=fmt, base_iri=base):
            batch.append((str(q.subject), str(q.predicate), str(q.object)))
            if len(batch) >= 200000:
                con.executemany("INSERT OR IGNORE INTO t VALUES (?, ?, ?)", batch)
                batch = []
        con.executemany("INSERT OR IGNORE INTO t VALUES (?, ?, ?)", batch)
    con.execute("CREATE INDEX t_pos ON t (p, o, s)")
    con.execute("CREATE INDEX t_osp ON t (o, s, p)")
    con.execute("ANALYZE")
    con.commit()
    con.set_progress_handler(lambda: 1 if time.perf_counter() > DEADLINE[0] else 0, 100000)
    load_s = time.perf_counter() - t0
    triples = con.execute("SELECT count(*) FROM t").fetchone()[0]
    rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / (1024 * 1024)  # bytes on macOS
    print(f"#load\ttriples={triples}\tload_s={load_s:.2f}\tstatistics_s=0\tfile_mib={db.stat().st_size / 1048576:.1f}\tpeak_rss_mib={rss:.0f}")
    for line in open(queries):
        c = line.rstrip("\n").split("\t")
        if len(c) < 2:
            continue
        if types:
            sql, _ = types_sql()
            n, us = timed(con, sql, (c[1], SUBCLASS, TYPE), runs)
            print(f"{c[0]}\t{c[1]}\t{c[2]}\t{n}\t{us}")
        else:
            sql, params = translate(c[1])
            n, us = timed(con, sql, params, runs)
            print(f"{c[0]}\t{n}\t{us}")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
