#!/usr/bin/env python3
"""The LUBM queries that need only RDFS subclass reasoning, in two forms.

    prepare_inference.py <out.tsv> <out-path.tsv>

LUBM 3, 4, 6, 7, 8, 9, and 10 ask for the instances of classes that have subclasses (Student,
Professor, Faculty, Course, Publication) and use no other inference. <out.tsv> keeps them as
written, for an engine that entails subclass instances itself. <out-path.tsv> rewrites each
`?x rdf:type C` into the standard SPARQL 1.1 form for an engine that does not,
`?c rdfs:subClassOf* C . ?x rdf:type ?c` (the path first: Jena does not reorder it)."""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from prepare import lubm_queries  # noqa: E402

KEEP = {"q03", "q04", "q06", "q07", "q08", "q09", "q10"}
TYPE = re.compile(r"(\?\w+) (?:rdf:type|a) ((?:ub:\w+)|(?:<[^>]+>))")


def path_form(text):
    counter = [0]

    def sub(m):
        counter[0] += 1
        v = f"?c{counter[0]}"
        return f"{v} rdfs:subClassOf* {m.group(2)} . {m.group(1)} rdf:type {v}"

    text = TYPE.sub(sub, text)
    return text.replace("PREFIX rdf:", "PREFIX rdfs: <http://www.w3.org/2000/01/rdf-schema#> PREFIX rdf:", 1)


def main():
    queries = [(n, q) for n, q in lubm_queries() if n in KEEP]
    Path(sys.argv[1]).write_text("".join(f"{n}\t{q}\n" for n, q in queries))
    Path(sys.argv[2]).write_text("".join(f"{n}\t{path_form(q)}\n" for n, q in queries))
    print(f"{len(queries)} queries")


if __name__ == "__main__":
    main()
