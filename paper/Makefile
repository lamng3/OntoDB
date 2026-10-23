# The paper in two versions from one source.
#   ontodb.pdf       the main paper: 12 pages with references, then an appendix of proofs and further algorithms
#   ontodb-full.pdf  the extended version: every measurement, detail, and proof in place
# Needs tectonic. The tables under gen/ are produced by make-compare.py and make-figs.py from the
# benchmark outputs (see ../benchmarks) and are committed, so the paper builds without them.
all: ontodb.pdf ontodb-full.pdf

ontodb.pdf: ontodb.tex $(wildcard *.tex gen/*.tex) refs.bib
	tectonic ontodb.tex

ontodb-full.pdf: ontodb-full.tex ontodb.tex $(wildcard *.tex gen/*.tex) refs.bib
	tectonic ontodb-full.tex

.PHONY: all
