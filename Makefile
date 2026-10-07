CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Wpedantic
LDLIBS ?= -lm

.PHONY: all check clean preview

all: mandala

mandala: mandala.c
	$(CC) $(CPPFLAGS) $(CFLAGS) mandala.c $(LDFLAGS) $(LDLIBS) -o $@

check: mandala
	./mandala --check

preview: mandala
	python3 tools/generate_readme_gif.py ./mandala assets/mandala.gif

clean:
	rm -f mandala
