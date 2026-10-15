# Build the book: make (needs pdflatex with tikz, listings, tcolorbox, cleveref).
PDF = ontodb-book.pdf

$(PDF): ontodb-book.tex preamble.tex chapters/*.tex
	pdflatex -interaction=nonstopmode -halt-on-error ontodb-book.tex >/dev/null
	pdflatex -interaction=nonstopmode -halt-on-error ontodb-book.tex >/dev/null
	pdflatex -interaction=nonstopmode -halt-on-error ontodb-book.tex >/dev/null

clean:
	rm -f *.aux *.log *.out *.toc chapters/*.aux

.PHONY: clean
