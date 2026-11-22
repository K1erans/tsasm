# tsasm runtime: x86-64 assembly tree-sitter parser
AS ?= as
TS_MODS ?= $(HOME)/go/pkg/mod/github.com/tree-sitter
GRAMMARS ?= go c java json
GRAMMAR_go ?= $(TS_MODS)/tree-sitter-go@v0.25.0/src/parser.c
GRAMMAR_c ?= $(TS_MODS)/tree-sitter-c@v0.24.2/src/parser.c
GRAMMAR_java ?= $(TS_MODS)/tree-sitter-java@v0.23.5/src/parser.c
GRAMMAR_json ?= $(TS_MODS)/tree-sitter-json@v0.24.8/src/parser.c

all: libtsasm.a

.SECONDEXPANSION:
build/lang_%.s: gen/gen.py $$(GRAMMAR_$$*)
	@mkdir -p build
	python3 gen/gen.py $(GRAMMAR_$*) $* build

build/%.o: build/%.s
	$(AS) -o $@ $<

build/parser.o: src/parser.s
	@mkdir -p build
	$(AS) -o $@ src/parser.s

libtsasm.a: build/parser.o $(GRAMMARS:%=build/lang_%.o)
	rm -f $@
	ar rcs $@ $^

# The library must hold no writable static data (the structural half of thread safety).
check-static: libtsasm.a
	sh scripts/check_static.sh libtsasm.a

clean:
	rm -rf build libtsasm.a

.PHONY: all clean check-static

# ---- dev/test tools (C reference: the go-tree-sitter module's tree-sitter core + each grammar) ----
TS_RUNTIME ?= $(TS_MODS)/go-tree-sitter@v0.25.0
REFLIB := build/libref.a
REFOBJS := build/ref.o build/ts_lib.o $(GRAMMARS:%=build/ts_%.o)

build/ts_lib.o:
	@mkdir -p build
	gcc -O2 -c -I$(TS_RUNTIME)/include -I$(TS_RUNTIME)/src $(TS_RUNTIME)/src/lib.c -o $@
build/ts_%.o: $$(GRAMMAR_$$*)
	@mkdir -p build
	gcc -O2 -c -I$(dir $<) $< -o $@
build/ref.o: test/ref.c test/langs.h tsasm.h
	@mkdir -p build
	gcc -O2 -c -I$(TS_RUNTIME)/include test/ref.c -o $@
$(REFLIB): $(REFOBJS)
	rm -f $@
	ar rcs $@ $^

TOOLFLAGS := -O2 -g -pthread -I$(TS_RUNTIME)/include
tools: libtsasm.a $(REFLIB)
	gcc $(TOOLFLAGS) -o test/check_all test/check.c libtsasm.a $(REFLIB)
	gcc $(TOOLFLAGS) -o test/fuzz test/fuzz.c libtsasm.a $(REFLIB)
	gcc $(TOOLFLAGS) -o test/one test/one.c libtsasm.a $(REFLIB)
	gcc $(TOOLFLAGS) -o test/mtspeed test/mtspeed.c libtsasm.a $(REFLIB)

# Go parity vs C tree-sitter over GOROOT/src (memory capped at 4 GB)
check: tools
	find -L $$(go env GOROOT)/src -type f -name '*.go' | sort > test/files.txt
	systemd-run --user --scope -q -p MemoryMax=4G -p MemorySwapMax=0 timeout 1200 test/check_all go test/files.txt 1 > test/asm_full.txt; tail -1 test/asm_full.txt

.PHONY: tools check

# Stress suite (test/stress.c): pathological inputs, retention, 20 threads (capped at 8 GB)
stress stress-smoke:
	$(MAKE) -f test/stress.mk $@
.PHONY: stress stress-smoke
