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

$(GRAMMARS:%=build/lang_%.o): build/%.o: build/%.s
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

# ---- fuzzing: one libFuzzer binary per grammar, build/fuzz/<lang> ----
# The asm gets libFuzzer coverage counters and the guard-page allocator (scripts/covinstr.py);
# the harness and the C reference build with ASan. libFuzzer comes from LLVM's compiler-rt.
FUZZER_RT ?= $(firstword $(wildcard /usr/lib/llvm-*/lib/clang/*/lib/linux/libclang_rt.fuzzer-x86_64.a))
FUZZCC := gcc -O1 -g -fsanitize=address -fno-omit-frame-pointer -I$(TS_RUNTIME)/include
build/fuzz/parser.o: src/parser.s scripts/covinstr.py
	@mkdir -p build/fuzz
	python3 scripts/covinstr.py parser src/parser.s build/fuzz/parser.s
	$(AS) -o $@ build/fuzz/parser.s
$(GRAMMARS:%=build/fuzz/lang_%.o): build/fuzz/lang_%.o: build/lang_%.s scripts/covinstr.py
	@mkdir -p build/fuzz
	python3 scripts/covinstr.py lang_$* build/lang_$*.s build/fuzz/lang_$*.s
	$(AS) -o $@ build/fuzz/lang_$*.s
build/fuzz/guard.o: fuzz/guard.c fuzz/guard.h
	@mkdir -p build/fuzz
	$(FUZZCC) -c fuzz/guard.c -o $@
build/fuzz/ref.o: test/ref.c test/langs.h tsasm.h
	@mkdir -p build/fuzz
	$(FUZZCC) -c test/ref.c -o $@
FUZZOBJS := build/fuzz/parser.o $(GRAMMARS:%=build/fuzz/lang_%.o) build/fuzz/guard.o build/fuzz/ref.o
$(GRAMMARS:%=build/fuzz/%): build/fuzz/%: fuzz/target.c test/langs.h tsasm.h $(FUZZOBJS) build/ts_lib.o $(GRAMMARS:%=build/ts_%.o)
	$(FUZZCC) -DFUZZ_LANG=$* -c fuzz/target.c -o build/fuzz/target_$*.o
	g++ -fsanitize=address -o $@ build/fuzz/target_$*.o $(FUZZOBJS) build/ts_lib.o $(GRAMMARS:%=build/ts_%.o) $(FUZZER_RT) -lpthread
fuzz: $(GRAMMARS:%=build/fuzz/%)
# Replays every saved finding (fuzz/regress/<lang>/) once in both guard modes; fails on any.
fuzz-regress: fuzz
	sh scripts/fuzz_regress.sh
.PHONY: fuzz fuzz-regress

# ---- GSS prototype (test/proto.c): the C spec of the parse stack port, checked like the library ----
build/proto_parser.o: src/parser.s
	@mkdir -p build
	$(AS) --defsym PROTO=1 -o $@ src/parser.s
test/check_proto: test/check.c test/proto.c test/langs.h tsasm.h build/proto_parser.o $(GRAMMARS:%=build/lang_%.o) $(REFLIB)
	gcc $(TOOLFLAGS) -DPROTO -o $@ test/check.c test/proto.c build/proto_parser.o $(GRAMMARS:%=build/lang_%.o) $(REFLIB)
proto: test/check_proto
.PHONY: proto
