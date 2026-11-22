# Stress suite for libtsasm.a (test/stress.c). Run from the worktree root:
#   make -f test/stress.mk stress        full: pathological inputs up to 220 MB, retention,
#                                        20 threads x 60 s (about 5 minutes)
#   make -f test/stress.mk stress-smoke  smaller inputs, 20 threads x 10 s (under 2 minutes)
# STRESS_ARGS adds options, e.g. STRESS_ARGS="-threads 2 -secs 30 -seed 7 -only paren".
# NICE="nice -n 19" lowers priority. Every run is capped at 8 GB with no swap.
TS_RUNTIME ?= $(HOME)/go/pkg/mod/github.com/tree-sitter/go-tree-sitter@v0.25.0
CAP ?= systemd-run --user --scope -q -p MemoryMax=8G -p MemorySwapMax=0
NICE ?=
STRESS_ARGS ?=

stress: build/stress
	$(CAP) $(NICE) timeout 1800 build/stress $(STRESS_ARGS)

stress-smoke: build/stress
	$(CAP) $(NICE) timeout 300 build/stress -smoke $(STRESS_ARGS)

build/stress: test/stress.c tsasm.h libtsasm.a build/libref.a
	mkdir -p build
	gcc -O2 -g -Wall -pthread -o $@ test/stress.c libtsasm.a build/libref.a

# the library and the C tree-sitter reference come from the main Makefile
libtsasm.a build/libref.a: FORCE
	$(MAKE) -f Makefile TS_RUNTIME=$(TS_RUNTIME) $@

FORCE:
.PHONY: stress stress-smoke FORCE
