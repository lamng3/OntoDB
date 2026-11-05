# OntoDB for Python

```bash
pip install ontodb
```

```python
import ontodb

con = ontodb.connect("mydb")                     # or ontodb.connect() for memory
con.load("mouse.owl")                            # Turtle, N-Triples, or RDF/XML

rows = con.execute("""
    SELECT ?kind WHERE {
      ?kind <http://www.w3.org/2000/01/rdf-schema#subClassOf> <http://mouse.owl#MA_0000002>
    }""").fetchall()                             # or .dicts(), or .raw for terms as stored

con.build_hierarchy()
con.under("http://mouse.owl#MA_0000067")         # everything below a class, by range scans

con.load_vectors(terms, vectors)                 # one vector per term, indexed with IVF and HNSW
con.similar("http://mouse.owl#MA_0000072", 5)    # nearest to a term
con.similar(embed("blood supply of the kidney"), 5, under="http://mouse.owl#MA_0000067")  # to any vector
```

`python -m ontodb [dir]` opens the shell. Wheels bundle the engine and its RDF parser, so there is nothing to
compile. Linux and macOS; Python 3.12 or newer.
