#!/usr/bin/env python3
"""Write the query list both engines run: the 14 LUBM queries and sampled single-pattern queries.

    .venv/bin/python prepare.py <lubm dir> <out.tsv> [seed]

Each line of the output is `name TAB one-line SPARQL`. The pattern queries bind some of s, p, o to
terms drawn from the data (pyoxigraph loads the files only to sample them), so every engine is asked
the same questions. Shapes: s-- (a subject's triples), sp-, --o, -po (one predicate and one object,
the shape of `?x rdf:type C`), and -p- (a whole predicate)."""
import random
import re
import sys
from pathlib import Path

from pyoxigraph import NamedNode, RdfFormat, Store

HERE = Path(__file__).resolve().parent
QUERIES = HERE.parent.parent / "data" / "lubm" / "queries"
PER_SHAPE = 100


def lubm_queries():
    out = []
    for path in sorted(QUERIES.glob("q*.sparql")):
        text = "\n".join(l for l in path.read_text().splitlines() if not l.lstrip().startswith("#"))
        head, sep, body = text.partition("WHERE")
        head = head.replace(",", " ")  # the published queries put commas between SELECT variables
        text = head + sep + body
        text = re.sub(r'(?<![<"])(http://[^\s}<>"]+)', r"<\1>", text)  # and leave some IRIs bare
        out.append((path.stem, " ".join(text.split())))
    return out


def sample_patterns(files, seed):
    store = Store()
    for f in files:
        store.bulk_load(path=str(f), format=RdfFormat.RDF_XML, base_iri=f.resolve().as_uri())
    triples = [(q.subject, q.predicate, q.object) for q in store]
    rng = random.Random(seed)
    iri = lambda t: isinstance(t, NamedNode)
    shapes = {"s--": [], "sp-": [], "--o": [], "-po": []}
    for s, p, o in rng.sample(triples, min(len(triples), PER_SHAPE * 6)):
        if not (iri(s) and iri(p)):
            continue
        shapes["s--"].append(f"SELECT * WHERE {{ {s} ?p ?o }}")
        shapes["sp-"].append(f"SELECT * WHERE {{ {s} {p} ?o }}")
        if iri(o):
            shapes["--o"].append(f"SELECT * WHERE {{ ?s ?p {o} }}")
            shapes["-po"].append(f"SELECT * WHERE {{ ?s {p} {o} }}")
    out = []
    for shape, queries in shapes.items():
        for i, q in enumerate(queries[:PER_SHAPE]):
            out.append((f"{shape}#{i:03d}", q))
    for i, p in enumerate(sorted({str(t[1]) for t in triples})):
        out.append((f"-p-#{i:03d}", f"SELECT * WHERE {{ ?s {p} ?o }}"))
    return out


def main():
    data, out = Path(sys.argv[1]), Path(sys.argv[2])
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    files = sorted(data.glob("*.owl"))
    rows = lubm_queries() + sample_patterns(files, seed)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("".join(f"{n}\t{q}\n" for n, q in rows))
    print(f"{len(rows)} queries -> {out}")


if __name__ == "__main__":
    main()
