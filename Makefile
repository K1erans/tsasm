# tsasm runtime: x86-64 assembly tree-sitter parser for Go
GRAMMAR ?= $(HOME)/go/pkg/mod/github.com/tree-sitter/tree-sitter-go@v0.25.0/src/parser.c
AS ?= as

all: libtsasm.a

build/tables.s build/lexer.s build/tables.inc build/tables.h: gen/gen.py $(GRAMMAR)
	mkdir -p build
	python3 gen/gen.py $(GRAMMAR) build

build/%.o: build/%.s
	$(AS) -o $@ $<

build/parser.o: src/parser.s build/tables.inc
	$(AS) -I build -o $@ src/parser.s

libtsasm.a: build/parser.o build/lexer.o build/tables.o
	rm -f $@
	ar rcs $@ $^

clean:
	rm -rf build libtsasm.a

.PHONY: all clean

# ---- dev/test tools (need the go-tree-sitter + tree-sitter-go module sources) ----
TS_RUNTIME ?= $(HOME)/go/pkg/mod/github.com/tree-sitter/go-tree-sitter@v0.25.0
GRAMMAR_DIR := $(dir $(GRAMMAR))
REFLIB := test/libref.a

$(REFLIB): test/ref.c tsasm.h
	gcc -O2 -c -I$(TS_RUNTIME)/include -I$(TS_RUNTIME)/src $(TS_RUNTIME)/src/lib.c -o test/ts_lib.o
	gcc -O2 -c -I$(GRAMMAR_DIR) $(GRAMMAR) -o test/go_parser.o
	gcc -O2 -c -I$(TS_RUNTIME)/include test/ref.c -o test/ref.o
	ar rcs $@ test/ref.o test/ts_lib.o test/go_parser.o

build/lexwrap.o: test/lexwrap.s
	$(AS) -o $@ $<

tools: libtsasm.a $(REFLIB) build/lexwrap.o
	gcc -O2 -g -w -o test/check_all test/check.c test/model.c libtsasm.a build/lexwrap.o $(REFLIB)
	gcc -O2 -w -o test/speed test/speed.c libtsasm.a $(REFLIB)
	gcc -O2 -w -o test/fuzz test/fuzz.c libtsasm.a $(REFLIB)
	gcc -O2 -g -o test/one test/one.c libtsasm.a
	gcc -O2 -g -pthread -I$(TS_RUNTIME)/include -o test/mtspeed test/mtspeed.c libtsasm.a $(REFLIB)

# parity vs C tree-sitter over GOROOT/src (memory capped at 4 GB)
check: tools
	find -L $$(go env GOROOT)/src -type f -name '*.go' | sort > test/files.txt
	ulimit -v 4000000; cd test && ./check_all asm files.txt 1 > asm_full.txt; tail -1 asm_full.txt

.PHONY: tools check
