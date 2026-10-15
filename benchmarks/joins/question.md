# Join order and join operator

**Question:** how much do join order and the choice of join operator matter on LUBM, and does the optimizer's own choice (`\set join auto`) keep up with the best forced operator?

**Compared:** five settings, all on the indexed backend with SPO, POS, and OSP:

| Setting | Order | Operator |
|---|---|---|
| written | the query's pattern order (no optimizer) | nested loop |
| nlj | the optimizer's greedy order | nested loop |
| inlj | the optimizer's greedy order | index nested loop |
| hash | the optimizer's greedy order | hash join |
| auto | the optimizer's greedy order | the optimizer's choice per join |

All nested loops rebind their inner scan (plan 4.1), so even "written" probes an index once a join variable is bound; what it lacks is a good order.

**Data:** LUBM(1), the 15 files `scripts/fetch-data lubm` generates, and the 14 LUBM queries in `data/lubm/queries/`. The benchmark makes them standard SPARQL (it removes the commas between `SELECT` variables and brackets bare IRIs). Queries that rely on inference (for example `?x rdf:type ub:Student`, where the data says `GraduateStudent`) return a subset, the same under every setting.

**Metric:** wall-clock milliseconds for the whole statement, parse and plan excluded, with a buffer pool large enough to hold the store, so this measures plans, not I/O. A run that passes 20 seconds stops (`ExecutorContext::SetDeadline`) and is reported as `> 20 s`.

**Decision rule:** `auto` should be within a small factor of the best forced setting on every query and never time out.

Run: `benchmarks/joins/run.sh`. Results: [results.md](results.md).
