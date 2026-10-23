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


def _worker(db, role, queries, seconds, start_at, out, seed):
    con = sqlite3.connect(db, timeout=30)
    con.execute("PRAGMA synchronous = NORMAL")
    n = 0
    while time.time() < start_at:
        pass
    end = start_at + seconds
    if role == "read":
        at = seed * 7
        while time.time() < end:
            sql, params = queries[at % len(queries)]
            for row in con.execute(sql, params):
                for v in row:
                    len(v)
            at += 1
            n += 1
    else:
        k = seed * 100000000
        while time.time() < end:
            ident = (k * 2654435761) & ((1 << 26) - 1)
            k += 1
            con.execute("INSERT OR IGNORE INTO t VALUES (?, ?, ?)", (f"<http://example.org/new/s{ident}>", "<http://example.org/new/p>", "<http://example.org/new/o>"))
            con.commit()
            n += 1
    out.put((role, n))


def concurrency(db, queries_path, seconds):
    """Processes, one connection each (Python threads would share one interpreter lock), against
    one WAL-mode database: lookup readers, insert writers, or one writer beside readers. A writer
    inserts one triple per transaction."""
    import multiprocessing as mp
    ctx = mp.get_context("fork")
    queries = []
    for line in open(queries_path):
        c = line.rstrip("\n").split("\t")
        if len(c) >= 2 and (c[0].startswith("s--") or c[0].startswith("sp-")):
            queries.append(translate(c[1]))
    con = sqlite3.connect(db)
    con.execute("PRAGMA journal_mode = WAL")
    con.commit()
    con.close()

    def run(readers, writers):
        out = ctx.Queue()
        start_at = time.time() + 1.0
        procs = [ctx.Process(target=_worker, args=(db, "read", queries, seconds, start_at, out, i)) for i in range(readers)]
        procs += [ctx.Process(target=_worker, args=(db, "write", queries, seconds, start_at, out, 100 + i)) for i in range(writers)]
        for p in procs:
            p.start()
        results = [out.get() for _ in procs]
        for p in procs:
            p.join()
        return (sum(n for r, n in results if r == "read") / seconds, sum(n for r, n in results if r == "write") / seconds)

    run(2, 0)  # warm the caches
    for t in (1, 2, 4, 8):
        r, _ = run(t, 0)
        print(f"read\tlookup\t{t}\t{r:.0f}\t0")
        sys.stdout.flush()
    for t in (1, 2, 4, 8):
        _, w = run(0, t)
        print(f"insert\t-\t{t}\t0\t{w:.0f}")
        sys.stdout.flush()
    for t in (1, 2, 4, 8):
        r, w = run(t, 1)
        print(f"mixed\tlookup\t{t}\t{r:.0f}\t{w:.0f}")
        sys.stdout.flush()


def main():
    a = sys.argv[1:]
    if a[1] == "conc":
        build_and_conc(a)
        return
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


def build_and_conc(a):
    work = Path(a[0])
    queries, seconds, files = a[2], float(a[3]), a[4:]
    work.mkdir(parents=True, exist_ok=True)
    db = work / "triples.sqlite"
    if db.exists():
        db.unlink()
    con = sqlite3.connect(db)
    con.execute("PRAGMA journal_mode = OFF")
    con.execute("PRAGMA synchronous = OFF")
    con.execute("CREATE TABLE t (s TEXT NOT NULL, p TEXT NOT NULL, o TEXT NOT NULL, PRIMARY KEY (s, p, o)) WITHOUT ROWID")
    for f in files:
        batch = []
        for q in parse(path=f, format=RdfFormat.RDF_XML, base_iri=Path(f).resolve().as_uri()):
            batch.append((str(q.subject), str(q.predicate), str(q.object)))
        con.executemany("INSERT OR IGNORE INTO t VALUES (?, ?, ?)", batch)
    con.execute("CREATE INDEX t_pos ON t (p, o, s)")
    con.execute("CREATE INDEX t_osp ON t (o, s, p)")
    con.execute("ANALYZE")
    con.commit()
    con.close()
    concurrency(str(db), queries, seconds)


if __name__ == "__main__":
    main()
