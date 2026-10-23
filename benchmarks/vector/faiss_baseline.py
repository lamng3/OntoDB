#!/usr/bin/env python3
"""FAISS and hnswlib on the same vectors and the same queries as ontodb_bench_vector.

    faiss_baseline.py filtered <name> <ontology.nt> <terms.ovec> <synonyms.ovec> <workload.tsv> <out dir>
    faiss_baseline.py index    <name> <terms.ovec> <out dir>

filtered: replays <name>-workload.tsv (a class, a query: a stored term or a synonym's vector) and
answers each query three ways with FAISS's own filtering, an IDSelectorBitmap of the class's members
passed to a flat, an IVF-flat (nprobe 16), and an HNSW-flat (efSearch 64) index. The members of the
class come from a descendant table built here from the ontology's subClassOf triples and are not
counted in the time, which favors FAISS; each query is one call from Python, whose overhead is
reported on its own. Recall is against exact search over the members, the query term excluded.

index: unfiltered recall against time for FAISS IVF-flat and HNSW-flat and for hnswlib, over 1,000
query terms, in one batch on one thread (which favors them: no per-call overhead)."""
import math
import random
import re
import struct
import sys
import time
from collections import defaultdict
from pathlib import Path

import faiss
import hnswlib
import numpy as np

faiss.omp_set_num_threads(1)
SUBCLASS = "<http://www.w3.org/2000/01/rdf-schema#subClassOf>"
import os

K = int(os.environ.get("VEC_K", "10"))  # the number of neighbors asked for


def read_ovec(path):
    data = Path(path).read_bytes()
    assert data[:6] == b"OVEC1\n"
    dim, count = struct.unpack_from("<IQ", data, 6)
    pos = 6 + 12
    names, vecs = [], np.empty((count, dim), dtype=np.float32)
    for i in range(count):
        (n,) = struct.unpack_from("<I", data, pos)
        pos += 4
        names.append(data[pos:pos + n].decode())
        pos += n
        vecs[i] = np.frombuffer(data, dtype="<f4", count=dim, offset=pos)
        pos += dim * 4
    return names, vecs


def normalized(m):
    return m / np.linalg.norm(m, axis=-1, keepdims=True)


def descendants(nt_path):
    """child edges of every class, then the reflexive descendant sets by memoized search."""
    children = defaultdict(set)
    pat = re.compile(r"^(<[^>]+>) <http://www\.w3\.org/2000/01/rdf-schema#subClassOf> (<[^>]+>) \.")
    for line in open(nt_path, encoding="utf-8"):
        m = pat.match(line)
        if m:
            children[m.group(2)].add(m.group(1))
    cache = {}

    def below(c):
        if c in cache:
            return cache[c]
        seen, stack = {c}, [c]
        while stack:
            x = stack.pop()
            for y in children.get(x, ()):
                if y not in seen:
                    seen.add(y)
                    stack.append(y)
        cache[c] = seen
        return seen

    return below


def filtered(name, nt, terms, syn, workload, out):
    names, vecs = read_ovec(terms)
    vecs = normalized(vecs)
    row_of = {n: i for i, n in enumerate(names)}
    syn_names, syn_vecs = read_ovec(syn)
    syn_vecs = normalized(syn_vecs)
    below = descendants(nt)
    n, d = vecs.shape
    lists = max(64, int(2.6 * math.sqrt(n)))
    flat = faiss.IndexFlatIP(d)
    flat.add(vecs)
    quant = faiss.IndexFlatIP(d)
    ivf = faiss.IndexIVFFlat(quant, d, lists, faiss.METRIC_INNER_PRODUCT)
    t = time.time()
    ivf.train(vecs)
    ivf.add(vecs)
    ivf_s = time.time() - t
    hnsw = faiss.IndexHNSWFlat(d, 16, faiss.METRIC_INNER_PRODUCT)
    hnsw.hnsw.efConstruction = 200
    t = time.time()
    hnsw.add(vecs)
    hnsw_s = time.time() - t
    # the cost of one call from Python with nothing to do
    tiny = faiss.IndexFlatIP(d)
    tiny.add(vecs[:1])
    q0 = vecs[:1]
    t = time.time()
    for _ in range(2000):
        tiny.search(q0, K + 1)
    overhead_us = (time.time() - t) / 2000 * 1e6
    bitmaps = {}
    with open(Path(out) / f"{name}-faiss-hybrid.tsv", "w") as result:
        result.write("ontology\tbucket\tkind\tclass\tstrategy\trecall\tus\toverhead_us\n")
        for line in list(open(workload))[1:]:
            bucket, kind, cls, ref = line.rstrip("\n").split("\t")
            rows = np.array(sorted(row_of[c] for c in below(cls) if c in row_of), dtype=np.int64)
            if len(rows) == 0:
                continue
            tag, idx = ref.split(":")
            idx = int(idx)
            q = (vecs[idx] if tag == "row" else syn_vecs[idx]).reshape(1, d)
            self_row = idx if (tag == "row" and kind == "inside") else -1
            scores = vecs[rows] @ q[0]
            order = np.argsort(-scores)
            want = [int(rows[i]) for i in order if int(rows[i]) != self_row][:K]
            if cls not in bitmaps:
                bits = np.zeros((n + 7) // 8, dtype=np.uint8)
                for r in rows:
                    bits[r >> 3] |= 1 << (r & 7)
                bitmaps[cls] = bits
            sel = faiss.IDSelectorBitmap(n, faiss.swig_ptr(bitmaps[cls]))
            runs = {
                "faiss-flat": lambda: flat.search(q, K + 1, params=faiss.SearchParameters(sel=sel)),
                "faiss-ivf": lambda: ivf.search(q, K + 1, params=faiss.SearchParametersIVF(sel=sel, nprobe=16)),
                "faiss-hnsw": lambda: hnsw.search(q, K + 1, params=faiss.SearchParametersHNSW(sel=sel, efSearch=64)),
                # a much larger search budget: half of all lists, and a beam of 1,024
                "faiss-ivf-wide": lambda: ivf.search(q, K + 1, params=faiss.SearchParametersIVF(sel=sel, nprobe=lists // 2)),
                "faiss-hnsw-wide": lambda: hnsw.search(q, K + 1, params=faiss.SearchParametersHNSW(sel=sel, efSearch=1024)),
                # the budget between the default and the wide one
                "faiss-ivf-np64": lambda: ivf.search(q, K + 1, params=faiss.SearchParametersIVF(sel=sel, nprobe=64)),
                "faiss-ivf-np256": lambda: ivf.search(q, K + 1, params=faiss.SearchParametersIVF(sel=sel, nprobe=min(256, lists))),
                "faiss-hnsw-ef256": lambda: hnsw.search(q, K + 1, params=faiss.SearchParametersHNSW(sel=sel, efSearch=256)),
                "faiss-hnsw-ef4096": lambda: hnsw.search(q, K + 1, params=faiss.SearchParametersHNSW(sel=sel, efSearch=4096)),
            }
            for strategy, run in runs.items():
                run()
                t0 = time.perf_counter()
                for _ in range(3):
                    _, ids = run()
                us = (time.perf_counter() - t0) / 3 * 1e6
                got = [int(i) for i in ids[0] if i >= 0 and int(i) != self_row][:K]
                recall = len(set(got) & set(want)) / len(want) if want else 1.0
                result.write(f"{name}\t{bucket}\t{kind}\t{cls}\t{strategy}\t{recall:.4f}\t{us:.2f}\t{overhead_us:.2f}\n")
    with open(Path(out) / f"{name}-faiss-build.tsv", "w") as b:
        b.write("what\tseconds\n")
        b.write(f"faiss ivf ({lists} lists)\t{ivf_s:.2f}\nfaiss hnsw\t{hnsw_s:.2f}\ncall overhead (us)\t{overhead_us:.2f}\n")


def index(name, terms, out):
    names, vecs = read_ovec(terms)
    vecs = normalized(vecs)
    n, d = vecs.shape
    rng = random.Random(3)
    queries = [rng.randrange(n) for _ in range(1000)]
    qv = vecs[queries]
    flat = faiss.IndexFlatIP(d)
    flat.add(vecs)
    _, truth = flat.search(qv, K + 1)
    want = [[int(x) for x in row if int(x) != q][:K] for row, q in zip(truth, queries)]
    lists = max(64, int(2.6 * math.sqrt(n)))
    rows = []

    def recall(ids):
        hit = 0
        for row, q, w in zip(ids, queries, want):
            got = [int(x) for x in row if int(x) != q and x >= 0][:K]
            hit += len(set(got) & set(w))
        return hit / (K * len(queries))

    t = time.time()
    flat.search(qv, K + 1)
    rows.append(("faiss-flat", "exact", 1.0, (time.time() - t) / len(queries) * 1e6, 0.0))
    quant = faiss.IndexFlatIP(d)
    ivf = faiss.IndexIVFFlat(quant, d, lists, faiss.METRIC_INNER_PRODUCT)
    t = time.time()
    ivf.train(vecs)
    ivf.add(vecs)
    build = time.time() - t
    for nprobe in (1, 2, 4, 8, 16, 32, 64):
        ivf.nprobe = nprobe
        t = time.time()
        _, ids = ivf.search(qv, K + 1)
        rows.append(("faiss-ivf", f"nprobe={nprobe}", recall(ids), (time.time() - t) / len(queries) * 1e6, build))
    hnsw = faiss.IndexHNSWFlat(d, 16, faiss.METRIC_INNER_PRODUCT)
    hnsw.hnsw.efConstruction = 200
    t = time.time()
    hnsw.add(vecs)
    build = time.time() - t
    for ef in (10, 20, 40, 80, 160):
        hnsw.hnsw.efSearch = ef
        t = time.time()
        _, ids = hnsw.search(qv, K + 1)
        rows.append(("faiss-hnsw", f"ef={ef}", recall(ids), (time.time() - t) / len(queries) * 1e6, build))
    hl = hnswlib.Index(space="ip", dim=d)
    hl.set_num_threads(1)
    t = time.time()
    hl.init_index(max_elements=n, ef_construction=200, M=16)
    hl.add_items(vecs, np.arange(n))
    build = time.time() - t
    for ef in (10, 20, 40, 80, 160):
        hl.set_ef(max(ef, K + 1))
        t = time.time()
        ids, _ = hl.knn_query(qv, k=K + 1)
        rows.append(("hnswlib", f"ef={ef}", recall(ids), (time.time() - t) / len(queries) * 1e6, build))
    # the same settings, one call per query (what a service that answers one query at a time sees)
    hnsw.hnsw.efSearch = 40
    ivf.nprobe = 16
    for label, idx_, setting in (("faiss-flat-1", flat, "exact"), ("faiss-ivf-1", ivf, "nprobe=16"), ("faiss-hnsw-1", hnsw, "ef=40")):
        got = []
        t = time.perf_counter()
        for i in range(len(queries)):
            _, ids = idx_.search(qv[i:i + 1], K + 1)
            got.append(ids[0])
        us = (time.perf_counter() - t) / len(queries) * 1e6
        rows.append((label, setting, recall(got), us, 0.0))
    with open(Path(out) / f"{name}-faiss-index.tsv", "w") as f:
        f.write("ontology\tindex\tsetting\trecall\tus\tbuild_s\n")
        for index_name, setting, r, us, b in rows:
            f.write(f"{name}\t{index_name}\t{setting}\t{r:.4f}\t{us:.2f}\t{b:.2f}\n")


def mechanism(name, nt, terms, syn, workload, out):
    """Why an ID-selector IVF loses recall: how many members of the class lie in the lists it probes.
    For each query, the 16 nearest lists (nprobe, as in the filtered run) and the number of the
    class's members stored in them, against the ten the answer needs."""
    names, vecs = read_ovec(terms)
    vecs = normalized(vecs)
    row_of = {n: i for i, n in enumerate(names)}
    _, syn_vecs = read_ovec(syn)
    syn_vecs = normalized(syn_vecs)
    below = descendants(nt)
    n, d = vecs.shape
    lists = max(64, int(2.6 * math.sqrt(n)))
    quant = faiss.IndexFlatIP(d)
    ivf = faiss.IndexIVFFlat(quant, d, lists, faiss.METRIC_INNER_PRODUCT)
    ivf.train(vecs)
    ivf.add(vecs)
    list_of = np.empty(n, dtype=np.int64)
    for li in range(lists):
        ids = faiss.rev_swig_ptr(ivf.invlists.get_ids(li), ivf.invlists.list_size(li)).copy()
        list_of[ids] = li
    with open(Path(out) / f"{name}-mechanism.tsv", "w") as result:
        result.write("ontology\tbucket\tkind\tclass\tmembers\tmembers_in_probed_lists\tlists_with_a_member\n")
        for line in list(open(workload))[1:]:
            bucket, kind, cls, ref = line.rstrip("\n").split("\t")
            rows = np.array(sorted(row_of[c] for c in below(cls) if c in row_of), dtype=np.int64)
            tag, idx = ref.split(":")
            q = (vecs[int(idx)] if tag == "row" else syn_vecs[int(idx)]).reshape(1, d)
            _, probed = quant.search(q, 16)
            probed = set(int(x) for x in probed[0])
            in_probed = sum(1 for r in rows if int(list_of[r]) in probed)
            with_member = len({int(list_of[r]) for r in rows} & probed)
            result.write(f"{name}\t{bucket}\t{kind}\t{cls}\t{len(rows)}\t{in_probed}\t{with_member}\n")


def scale(name, nt, terms, syn, workload, out):
    """Queries per second with 1 to 8 Python threads over the workload, FAISS's exact scan and its
    HNSW, each with an ID selector (FAISS releases the interpreter lock while it searches)."""
    import threading
    names, vecs = read_ovec(terms)
    vecs = normalized(vecs)
    row_of = {n: i for i, n in enumerate(names)}
    _, syn_vecs = read_ovec(syn)
    syn_vecs = normalized(syn_vecs)
    below = descendants(nt)
    n, d = vecs.shape
    flat = faiss.IndexFlatIP(d)
    flat.add(vecs)
    hnsw = faiss.IndexHNSWFlat(d, 16, faiss.METRIC_INNER_PRODUCT)
    hnsw.hnsw.efConstruction = 200
    hnsw.add(vecs)
    jobs = []
    bitmaps = {}
    for line in list(open(workload))[1:]:
        bucket, kind, cls, ref = line.rstrip("\n").split("\t")
        tag, idx = ref.split(":")
        idx = int(idx)
        q = (vecs[idx] if tag == "row" else syn_vecs[idx]).reshape(1, d).copy()
        if cls not in bitmaps:
            bits = np.zeros((n + 7) // 8, dtype=np.uint8)
            for c in below(cls):
                if c in row_of:
                    r = row_of[c]
                    bits[r >> 3] |= 1 << (r & 7)
            bitmaps[cls] = bits
        jobs.append((q, faiss.IDSelectorBitmap(n, faiss.swig_ptr(bitmaps[cls]))))
    with open(Path(out) / f"{name}-faiss-scale.tsv", "w") as result:
        result.write("ontology\tindex\tthreads\tqueries_per_s\n")
        for label, index_ in (("faiss-flat", flat), ("faiss-hnsw", hnsw)):
            for threads in (1, 2, 4, 8):
                stop = False
                counts = [0] * threads

                def work(t):
                    at = t * 17
                    while not stop:
                        q, sel = jobs[at % len(jobs)]
                        if label == "faiss-flat":
                            index_.search(q, K + 1, params=faiss.SearchParameters(sel=sel))
                        else:
                            index_.search(q, K + 1, params=faiss.SearchParametersHNSW(sel=sel, efSearch=64))
                        at += 1
                        counts[t] += 1

                ts = [threading.Thread(target=work, args=(t,)) for t in range(threads)]
                t0 = time.time()
                for th in ts:
                    th.start()
                time.sleep(3)
                stop = True
                for th in ts:
                    th.join()
                result.write(f"{name}\t{label}\t{threads}\t{sum(counts) / (time.time() - t0):.0f}\n")
                result.flush()


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "mechanism":
        mechanism(*sys.argv[2:8])
    elif cmd == "scale":
        scale(*sys.argv[2:8])
    elif cmd == "filtered":
        filtered(*sys.argv[2:8])
    else:
        index(*sys.argv[2:5])
