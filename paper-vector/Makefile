# The vector-search paper. Needs tectonic. The tables under gen/ come from make-tables.py and the
# benchmark outputs of ../benchmarks/vector (run.sh); they are committed, so the paper builds alone.
vec.pdf: vec.tex $(wildcard *.tex gen/*.tex) refs.bib
	tectonic vec.tex
