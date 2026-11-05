"""The Python API, on small ontologies written here, in each format the package reads."""

import subprocess
import sys

import numpy as np
import pytest

import ontodb

EX = "http://example.org/"
TURTLE = f"""
@prefix : <{EX}> .
@prefix owl: <http://www.w3.org/2002/07/owl#> .
@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .
:organ a owl:Class ; rdfs:label "organ" .
:heart a owl:Class ; rdfs:label "heart" ; rdfs:subClassOf :organ .
:valve a owl:Class ; rdfs:label "heart \\"valve\\"" ; rdfs:subClassOf :heart .
:lung a owl:Class ; rdfs:label "lung" ; rdfs:subClassOf :organ .
"""
RDFXML = f"""<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:rdfs="http://www.w3.org/2000/01/rdf-schema#"
         xmlns:owl="http://www.w3.org/2002/07/owl#">
  <owl:Class rdf:about="{EX}organ"><rdfs:label>organ</rdfs:label></owl:Class>
  <owl:Class rdf:about="{EX}heart"><rdfs:label>heart</rdfs:label><rdfs:subClassOf rdf:resource="{EX}organ"/></owl:Class>
</rdf:RDF>
"""
LABEL = "http://www.w3.org/2000/01/rdf-schema#label"


@pytest.fixture
def ttl(tmp_path):
    path = tmp_path / "anatomy.ttl"
    path.write_text(TURTLE)
    return path


@pytest.fixture
def con(ttl):
    with ontodb.connect() as connection:
        connection.load(ttl)
        yield connection


def test_loading_returns_the_statement_count(ttl):
    with ontodb.connect() as con:
        assert con.load(ttl) == 11


def test_rdfxml_loads_through_the_bundled_parser(tmp_path):
    path = tmp_path / "anatomy.owl"
    path.write_text(RDFXML)
    with ontodb.connect() as con:
        assert con.load(path) == 5
        assert con.execute(f"SELECT ?l WHERE {{ <{EX}heart> <{LABEL}> ?l }}").fetchall() == [("heart",)]


def test_results_are_values_not_text(con):
    result = con.execute(f"SELECT ?c ?l WHERE {{ ?c <{LABEL}> ?l }} ORDER BY ?l")
    assert result.columns == ["c", "l"]
    assert len(result) == 4
    assert result.raw[0][0].startswith("<")
    assert (f"{EX}lung", "lung") in result.fetchall()
    assert {"c": f"{EX}lung", "l": "lung"} in result.dicts()
    assert result.fetchone() == next(iter(result))


def test_literals_are_unescaped(con):
    assert con.execute(f"SELECT ?l WHERE {{ <{EX}valve> <{LABEL}> ?l }}").fetchall() == [('heart "valve"',)]


def test_updates_report_and_apply(con):
    result = con.execute(f'INSERT DATA {{ <{EX}a> <{LABEL}> "a" }}')
    assert not result.columns and result.message
    assert len(con.execute(f"SELECT ?l WHERE {{ <{EX}a> <{LABEL}> ?l }}")) == 1


def test_a_bad_statement_is_an_error_and_the_connection_survives(con):
    with pytest.raises(ontodb.OntoDBError):
        con.execute("SELECT nonsense")
    with pytest.raises(ontodb.OntoDBError):
        con.execute("SELECT ?x WHERE { ?x OPTIONAL }")
    assert len(con.execute("SELECT ?s WHERE { ?s ?p ?o } LIMIT 1")) == 1


def test_a_missing_file_is_an_error():
    with ontodb.connect() as con, pytest.raises(ontodb.OntoDBError):
        con.load("nowhere.ttl")


def test_a_closed_connection_says_so(ttl):
    con = ontodb.connect()
    assert not con.closed
    con.close()
    assert con.closed
    with pytest.raises(ontodb.OntoDBError, match="closed"):
        con.execute("SELECT ?s WHERE { ?s ?p ?o }")


def test_memory_is_the_default():
    assert not ontodb.connect().durable
    assert not ontodb.connect(":memory:").durable


def test_a_durable_database_survives_reopening(ttl, tmp_path):
    path = tmp_path / "db"
    with ontodb.connect(path) as con:
        assert con.durable
        con.load(ttl)
        con.execute(f'INSERT DATA {{ <{EX}a> <{LABEL}> "a" }}')
    with ontodb.connect(path) as con:
        assert len(con.execute(f"SELECT ?c WHERE {{ ?c <{LABEL}> ?l }}")) == 5
        assert con.verify().rstrip().endswith("ok")


def test_a_transaction_rolls_back():
    with ontodb.connect() as con:
        con.begin()
        con.execute(f'INSERT DATA {{ <{EX}a> <{LABEL}> "a" }}')
        con.rollback()
        assert len(con.execute("SELECT ?s WHERE { ?s ?p ?o }")) == 0


def test_the_hierarchy_answers_what_is_below(con):
    con.build_hierarchy()
    assert set(con.under(f"{EX}heart")) == {f"{EX}heart", f"{EX}valve"}
    assert set(con.under("heart")) == {f"{EX}heart", f"{EX}valve"}  # by label
    assert len(con.under(f"{EX}organ", limit=2)) == 2


def test_a_label_typed_as_a_string_still_names_its_class(tmp_path):
    """Real OWL files spell labels "vein"^^xsd:string, not as plain literals."""
    path = tmp_path / "typed.ttl"
    path.write_text(f"""
    @prefix : <{EX}> .
    @prefix owl: <http://www.w3.org/2002/07/owl#> .
    @prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .
    @prefix xsd: <http://www.w3.org/2001/XMLSchema#> .
    :vein a owl:Class ; rdfs:label "vein"^^xsd:string .
    :renal a owl:Class ; rdfs:label "renal vein"^^xsd:string ; rdfs:subClassOf :vein .
    """)
    with ontodb.connect() as con:
        con.load(path)
        con.build_hierarchy()
        assert set(con.under("vein")) == {f"{EX}vein", f"{EX}renal"}
        con.load_vectors([f"{EX}vein", f"{EX}renal"], [[1.0, 0.0], [0.9, 0.1]], index=None)
        assert con.similar("vein", 1) == [(f"{EX}renal", pytest.approx(0.99, abs=0.01))]


def test_the_hierarchy_must_be_built_first(con):
    with pytest.raises(ontodb.OntoDBError, match="hierarchy"):
        con.under(f"{EX}heart")


TERMS = [f"{EX}organ", f"{EX}heart", f"{EX}valve", f"{EX}lung"]
VECTORS = np.array([[1, 1, 0], [1, 0, 0], [0.9, 0.1, 0], [0, 1, 0]], dtype=np.float32)


def test_search_from_a_term_leaves_out_the_term(con):
    assert con.load_vectors(TERMS, VECTORS, index=None) == 4
    hits = con.similar(f"{EX}heart", 2)
    assert [t for t, _ in hits] == [f"{EX}valve", f"{EX}organ"]


def test_search_from_a_vector_needs_no_term(con):
    con.load_vectors(TERMS, VECTORS, index=None)
    term, score = con.similar([0.0, 5.0, 0.0], 1)[0]  # need not be normalized
    assert term == f"{EX}lung" and score == pytest.approx(1.0)


@pytest.mark.parametrize("index", ["ivf", "hnsw", "all"])
def test_each_index_finds_the_nearest(con, index):
    con.load_vectors(TERMS, VECTORS, index=index)
    assert con.similar([0.0, 1.0, 0.0], 1)[0][0] == f"{EX}lung"


def test_search_filters_by_class(con):
    con.build_hierarchy()
    con.load_vectors(TERMS, VECTORS)
    found = {t for t, _ in con.similar([0.0, 1.0, 0.0], 4, under=f"{EX}heart")}
    assert found == {f"{EX}heart", f"{EX}valve"}


def test_unknown_terms_are_skipped(con):
    assert con.load_vectors([*TERMS, f"{EX}nothing"], np.vstack([VECTORS, [[1, 0, 0]]]), index=None) == 4


def test_bad_vectors_are_errors(con):
    with pytest.raises(ValueError, match="per term"):
        con.load_vectors(TERMS, VECTORS[:2])
    with pytest.raises(ontodb.OntoDBError, match="no vectors"):
        con.similar([1.0, 0.0, 0.0])
    con.load_vectors(TERMS, VECTORS, index=None)
    with pytest.raises(ontodb.OntoDBError, match="dimensions"):
        con.similar([1.0, 0.0])
    with pytest.raises(ontodb.OntoDBError, match="nonzero"):
        con.similar([0.0, 0.0, 0.0])
    with pytest.raises(ontodb.OntoDBError, match="no term"):
        con.similar(f"{EX}nothing")


def test_the_shell_ships_with_the_package(ttl):
    command = f"\\load {ttl}\nSELECT ?l WHERE {{ <{EX}heart> <{LABEL}> ?l }}\n"
    done = subprocess.run([sys.executable, "-m", "ontodb"], input=command, capture_output=True, text=True, timeout=60, check=True)
    assert '"heart"' in done.stdout and "(1 row)" in done.stdout
