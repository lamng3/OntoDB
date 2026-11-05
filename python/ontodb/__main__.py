"""The ``ontodb`` shell, the same as the standalone program."""

from __future__ import annotations

import argparse
from collections.abc import Sequence

from ontodb import _ontodb


def main(argv: Sequence[str] | None = None) -> int:
    """Run the shell.

    Args:
        argv (Sequence[str] | None): The arguments; defaults to the process's own.

    Returns:
        int: The exit code.
    """
    parser = argparse.ArgumentParser(prog="ontodb", description="A database management system for ontologies.")
    parser.add_argument("database", nargs="?", help="A directory for a durable database. Defaults to memory.")
    parser.add_argument("--strict", action="store_true", help="Refuse a database whose log was damaged.")
    args = parser.parse_args(argv)
    _ontodb.run_shell(args.database, args.strict)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
