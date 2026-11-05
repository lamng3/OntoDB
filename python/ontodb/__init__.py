"""OntoDB: a database management system for ontologies.

Example:
    >>> import ontodb
    >>> con = ontodb.connect()
    >>> con.load("mouse.owl")  # doctest: +SKIP
    >>> con.execute("SELECT ?c WHERE { ?c ?p ?o } LIMIT 3").fetchall()  # doctest: +SKIP
"""

from ontodb.connection import Connection, OntoDBError, Result, connect, term_value

__version__ = "0.1.0"

__all__ = ["Connection", "OntoDBError", "Result", "__version__", "connect", "term_value"]
