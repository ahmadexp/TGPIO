TEX := ispcs2026-tgpio

.PHONY: all clean

all: $(TEX).pdf

$(TEX).pdf: $(TEX).tex references.bib
	pdflatex $(TEX)
	bibtex $(TEX)
	pdflatex $(TEX)
	pdflatex $(TEX)

clean:
	rm -f $(TEX).aux $(TEX).bbl $(TEX).blg $(TEX).log $(TEX).out $(TEX).pdf
