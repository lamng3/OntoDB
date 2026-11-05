"""Connections to an OntoDB database, and the results of its queries."""

from __future__ import annotations

import re
from collections.abc import Callable, Iterator, Sequence
from dataclasses import dataclass
from functools import wraps
from os import PathLike
from typing import Any, TypeVar

import numpy as np

from ontodb import _ontodb

T = TypeVar("T")
_UNESCAPE = {'\\"': '"', "\\\\": "\\", "\\n": "\n", "\\t": "\t", "\\r": "\r"}
_IRI = re.compile(r"^[A-Za-z][A-Za-z0-9+.-]*:\S*$")


class OntoDBError(RuntimeError):
    """The database rejected a statement or failed an operation."""


def term_value(term: str) -> str:
    """Read a term as the store spells it.

    Args:
        term (str): An IRI in angle brackets, a quoted literal, or a blank node.

    Returns:
        str: The IRI without brackets, the literal's text, or the blank node as spelled.

    Example:
        >>> term_value('"heart"^^<http://www.w3.org/2001/XMLSchema#string>')
        'heart'
        >>> term_value("<http://example.org/heart>")
        'http://example.org/heart'
    """
    if term.startswith("<") and term.endswith(">"):
        return term[1:-1]
    if term.startswith('"'):
        text = term[1 : term.rindex('"')]
        return re.sub(r"\\.", lambda m: _UNESCAPE.get(m.group(0), m.group(0)), text)
    return term


def _spell(term: str) -> str:
    """Write a term for the engine: a bare IRI gets brackets, and anything else is `<iri>` or a label."""
    return f"<{term}>" if not term.startswith(("<", '"')) and _IRI.match(term) else term


def _checked(method: Callable[..., T]) -> Callable[..., T]:
    """Raise the engine's failures as :class:`OntoDBError`."""

    @wraps(method)
    def call(*args: Any, **kwargs: Any) -> T:
        try:
            return method(*args, **kwargs)
        except (RuntimeError, NotImplementedError, OSError) as error:
            raise OntoDBError(str(error)) from error

    return call


@dataclass(frozen=True)
class Result:
    """What a statement returned.

    Args:
        columns (list[str]): The variables of a query, without their ``?``.
        raw (list[list[str]]): The rows, with each term as the store spells it.
        message (str): What an update reports, such as how many triples it changed.
        milliseconds (float): How long it took.
    """

    columns: list[str]
    raw: list[list[str]]
    message: str = ""
    milliseconds: float = 0.0

    def fetchall(self) -> list[tuple[str, ...]]:
        """Read every row.

        Returns:
            list[tuple[str, ...]]: The rows, with IRIs and literals as plain strings.
        """
        return [tuple(term_value(t) for t in row) for row in self.raw]

    def fetchone(self) -> tuple[str, ...] | None:
        """Read the first row.

        Returns:
            tuple[str, ...] | None: The row, or ``None`` when there are no rows.
        """
        return tuple(term_value(t) for t in self.raw[0]) if self.raw else None

    def dicts(self) -> list[dict[str, str]]:
        """Read every row as a mapping.

        Returns:
            list[dict[str, str]]: One mapping of variable to value per row.
        """
        return [dict(zip(self.columns, row, strict=True)) for row in self.fetchall()]

    def __iter__(self) -> Iterator[tuple[str, ...]]:
        return iter(self.fetchall())

    def __len__(self) -> int:
        return len(self.raw)


class Connection:
    """A connection to an OntoDB database. Use :func:`connect` to make one.

    Args:
        path (str | PathLike | None): A directory for a durable database; ``None`` keeps it in memory.
        strict (bool): Refuse to open a durable database whose log was damaged, rather than recover to
            the last consistent point.
    """

    def __init__(self, path: str | PathLike[str] | None = None, *, strict: bool = False) -> None:
        self._con = _checked(_ontodb.Connection)(None if path is None else str(path), strict)

    @property
    def closed(self) -> bool:
        """Whether the connection is closed."""
        return self._con.closed

    @property
    def durable(self) -> bool:
        """Whether updates survive a restart."""
        return self._con.durable

    @_checked
    def close(self) -> None:
        """Close the connection. A durable database keeps what was committed."""
        self._con.close()

    def __enter__(self) -> Connection:
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    @_checked
    def load(self, path: str | PathLike[str]) -> int:
        """Load an RDF file: Turtle, N-Triples, or RDF/XML, by its extension.

        Args:
            path (str | PathLike): The file.

        Returns:
            int: How many statements the file holds.
        """
        return self._con.load(str(path))[0]

    @_checked
    def execute(self, statement: str) -> Result:
        """Run one SPARQL statement.

        Args:
            statement (str): A ``SELECT``, ``INSERT DATA``, or ``DELETE DATA``. Basic graph patterns,
                ``FILTER``, ``DISTINCT``, ``ORDER BY``, ``LIMIT``, and ``OFFSET`` are supported.

        Returns:
            Result: The rows, or what an update changed.
        """
        result = self._con.execute(statement)
        return Result(list(result.columns), [list(row) for row in result.rows], result.message, result.milliseconds)

    sql = execute

    @_checked
    def begin(self) -> None:
        """Start a transaction. Only an in-memory database accepts one yet."""
        self._con.begin()

    @_checked
    def commit(self) -> None:
        """Commit the transaction."""
        self._con.commit()

    @_checked
    def rollback(self) -> None:
        """Abort the transaction."""
        self._con.abort()

    @_checked
    def checkpoint(self) -> str:
        """Write a snapshot of a durable database.

        Returns:
            str: What was written.
        """
        return self._con.checkpoint()

    @_checked
    def verify(self) -> str:
        """Check a durable database on disk.

        Returns:
            str: The report; it ends in ``ok`` when the database is sound.
        """
        return self._con.verify()

    @_checked
    def build_hierarchy(self, predicate: str = "") -> str:
        """Index a class hierarchy as intervals, for :meth:`under` and for filtered search.

        Args:
            predicate (str): The hierarchy's predicate; defaults to ``rdfs:subClassOf``.

        Returns:
            str: What was built.
        """
        return self._con.build_hierarchy(_spell(predicate) if predicate else "")

    @_checked
    def under(self, concept: str, limit: int = 0) -> list[str]:
        """List the classes at or below a class, with range scans over the hierarchy's intervals.

        Args:
            concept (str): A class IRI or label.
            limit (int): The most to return; 0 returns them all.

        Returns:
            list[str]: The classes, as plain strings.
        """
        return [term_value(t) for t in self._con.under(_spell(concept), limit)]

    @_checked
    def load_vectors(self, terms: Sequence[str], vectors: Any, *, index: str | None = "all") -> int:
        """Store one vector per term, and index them. Terms the database does not hold are skipped.

        Args:
            terms (Sequence[str]): The terms, as IRIs or as the store spells them.
            vectors (Any): An array of shape ``(len(terms), dimension)``.
            index (str | None): The index to build: ``"ivf"``, ``"hnsw"``, ``"all"``, or ``None`` to
                search exactly.

        Returns:
            int: How many vectors were kept.
        """
        matrix = np.asarray(vectors, dtype=np.float32, order="C")
        kept = self._con.put_vectors([_spell(t) for t in terms], matrix)
        if index is not None and kept:
            self._con.build_index(index)
        return kept

    @_checked
    def similar(self, query: str | Any, k: int = 10, *, under: str | None = None) -> list[tuple[str, float]]:
        """Find the terms nearest a term or a vector, optionally only below a class.

        Args:
            query (str | Any): A term, by IRI or label; or a vector, such as a free-text query's embedding.
            k (int): How many to return.
            under (str | None): Only classes at or below this class. Run :meth:`build_hierarchy` first.

        Returns:
            list[tuple[str, float]]: ``(term, cosine similarity)`` pairs, best first. A term is not
            returned as its own neighbor.
        """
        below = _spell(under) if under else ""
        if isinstance(query, str):
            hits = self._con.nearest_term(_spell(query), k, below)
        else:
            hits = self._con.nearest(np.asarray(query, dtype=np.float32, order="C"), k, below)
        return [(term_value(term), float(score)) for term, score in hits]


def connect(path: str | PathLike[str] | None = None, *, strict: bool = False) -> Connection:
    """Open a database.

    Args:
        path (str | PathLike | None): A directory for a durable database, created if it does not exist;
            ``None`` or ``":memory:"`` keeps it in memory.
        strict (bool): Refuse to open a durable database whose log was damaged.

    Returns:
        Connection: The connection.

    Example:
        >>> with connect() as con:
        ...     _ = con.execute('INSERT DATA { <http://x/a> <http://x/b> "c" }')
        ...     con.execute("SELECT ?o WHERE { <http://x/a> <http://x/b> ?o }").fetchall()
        [('c',)]
    """
    return Connection(None if path == ":memory:" else path, strict=strict)
