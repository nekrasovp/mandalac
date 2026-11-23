CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Wpedantic
LDLIBS ?= -lm
VERSION ?= dev
PLATFORM ?= local
DIST_DIR ?= dist

.PHONY: all check clean dist preview preview-omz test test-omz

all: mandala

mandala: mandala.c
	$(CC) $(CPPFLAGS) $(CFLAGS) mandala.c $(LDFLAGS) $(LDLIBS) -o $@

check: mandala
	./mandala --check

test: check
	python3 tests/test_shellsaver.py ./mandala
	python3 tests/test_package_release.py

test-omz: mandala
	zsh -n contrib/oh-my-zsh/mandala.zsh tests/test_omz_mandala.zsh
	zsh tests/test_omz_mandala.zsh

preview: mandala
	python3 tools/generate_readme_gif.py ./mandala assets/mandala.gif

preview-omz: mandala
	MANDALAC_BIN="$(CURDIR)/mandala" python3 tools/generate_readme_gif.py \
		/usr/bin/zsh assets/omz-mandala.gif \
		--argument=contrib/oh-my-zsh/mandala.zsh --seconds=6

dist: mandala
	python3 tools/package_release.py \
		--binary ./mandala \
		--version "$(VERSION)" \
		--platform "$(PLATFORM)" \
		--output-dir "$(DIST_DIR)"

clean:
	rm -f mandala
