# OntoDB: Database Internals by Example

A short book on how a database engine works, with OntoDB as the running example: pages and the buffer pool, B+ trees and RDF indexing, query plans and joins, transactions and locking, the write-ahead log, ARIES, and durability in practice.

Each chapter teaches the theory, shows how OntoDB applies it, and then lays out OntoDB's design decisions: the choice, the alternatives, why, and what it costs. A list of every decision follows the contents. Each chapter ends with questions, answered in an appendix.

Read it: [ontodb-book.pdf](ontodb-book.pdf).

Build it with `make` (pdflatex with tikz, listings, tcolorbox, and cleveref). Chapters are in `chapters/`, one file each.
