# tsasm: a reentrant tree-sitter parser runtime in x86-64 assembly

`libtsasm.a` parses source text with tree-sitter's parse tables and returns the visible syntax
tree as a flat pre-order array. The parser loop, GLR forking and merging, tree building and the
output walk are hand-written GNU as (`src/parser.s`). Each grammar's tables and lexer are
generated as assembly by `gen/gen.py` from that grammar's tree-sitter `parser.c`. No C runs at
parse time; libc is used only for `realloc`, `free` and `aligned_alloc`.

Built-in grammars (ABI 15 `parser.c`, no external scanners):

| grammar | version | function |
|---|---|---|
| Go | tree-sitter-go v0.25.0 | `tsasm_language_go()` |
| C | tree-sitter-c v0.24.2 | `tsasm_language_c()` |
| Java | tree-sitter-java v0.23.5 | `tsasm_language_java()` |
| JSON | tree-sitter-json v0.24.8 | `tsasm_language_json()` |

## Build

    make                # -> libtsasm.a (python3, GNU as, ar); tsasm.h is the header
    make check-static   # fails if the library has any writable static data

`make` reads each grammar's `parser.c` from the Go module cache (`GRAMMAR_<name>=` overrides a
path). The library is position-independent and works in PIE executables.

To add a grammar: add its name to `GRAMMARS` and a `GRAMMAR_<name>` path in the Makefile,
declare `tsasm_language_<name>()` in `tsasm.h`, and add a row to `test/langs.h` so the parity,
fuzz and stress tools know its C reference. The generator rejects grammars it cannot handle:
external scanners, more than 256 production ids, alias sequences longer than 16.

## API

The whole contract is in `tsasm.h`.

```c
TsasmParser *p = tsasm_parser_new();                 // one per thread (or per call site)
const TsasmNode *nodes;
int64_t n = tsasm_parse(p, tsasm_language_go(), src, len, &nodes);
if (n > 0) { /* nodes[0..n) is the tree, valid until the next parse on p */ }
tsasm_parser_free(p);
```

- **Caller-owned context.** A `TsasmParser` holds every piece of mutable state: buffers, the
  output array and the memory limit. The library has no writable static data, which
  `make check-static` enforces. Threads parse at the same time with one parser each. A parser
  serves one call at a time and may move between threads.
- **Languages are read-only.** A `TsasmLanguage` is a table descriptor in `.data.rel.ro`, shared
  by every thread. Any parser can parse any language, and switching costs nothing.
- **Output.** `TsasmNode` is 16 bytes: public `symbol` (after alias resolution, as
  `ts_node_symbol`), `named`, `field`, `start_byte`, `end_byte`, `parent` (index into the same
  array, -1 for the root). Nodes are the visible ones (named and anonymous, as tree-sitter's
  `ts_node_child` walk shows them) in pre-order. `tsasm_language_symbol_name()` gives the same
  strings as `ts_node_type`. `field` is the node's grammar field id (0 for none) as
  `ts_tree_cursor_current_field_id` gives it, and `tsasm_language_field_name()` names it.
- **Errors.** `TSASM_ERR_SYNTAX` (-1): the input is not in the language. There is no error
  recovery. `TSASM_ERR_LIMIT` (-3): a budget ran out. `TSASM_ERR_NOMEM` (-4): the allocator
  failed. After any error the parser stays usable and `*nodes` is NULL.
- **Memory.** `tsasm_parser_set_memory_limit()` caps the bytes a parser may hold (1 GiB by
  default). Growth is charged against the limit before it is allocated. Buffers of 128 KiB or
  more are mapped with `mmap` (huge pages advised) and grown with `mremap`, so memory a parser
  frees goes back to the system whatever malloc's thresholds are. `tsasm_parser_set_retain()`
  caps what a parser keeps between parses (32 MiB by default): a parse that needed more frees
  its working buffers before it returns, keeping only the node array it returns, and the next
  parse frees that too. `tsasm_parser_free()` frees everything.
- **Retention trade-off.** Freed memory comes back zeroed by the kernel when the next big parse
  touches it. On GOROOT with 20 threads (`test/mtspeed`), 32 MiB parses at full speed with 30%
  less peak memory than keeping every buffer; 8 MiB uses a fifth less again but parses 20-30%
  slower, because 20 threads zeroing fresh pages share the memory bus. Inside jinter, where
  parsing is part of the work, 8 MiB costs about 6% at 20 workers for 58% less peak memory
  (`results/ab_retain.txt`).

### Go

The repository root is the Go package `github.com/K1erans/tsasm` (linux/amd64, cgo). cgo
assembles the library with the C compiler, so `go get` is all it takes: no `make`, no
`CGO_LDFLAGS`.

```go
p, err := tsasm.NewParser() // one per goroutine
defer p.Close()
nodes, err := p.Parse(tsasm.Lookup("go"), src) // tsasm.ErrSyntax, ErrLimit or ErrNoMem
for _, n := range nodes { // valid until p's next Parse or Close
	l := tsasm.Lookup("go")
	fmt.Println(l.FieldName(n.Field), l.SymbolName(n.Symbol), n.Start, n.End, n.Parent)
}
```

The package builds from `zparser_linux_amd64.s` and `zlang_<grammar>_linux_amd64.s`, copies of
`src/parser.s` and the generated `build/lang_<grammar>.s`. After changing either, run
`make gopkg`; `make check-gopkg` fails if the copies are stale. The grammar tables are under the
grammars' MIT licenses (`NOTICE`).

### Budgets

Every parse is bounded. Each budget returns `TSASM_ERR_LIMIT` and leaves the parser usable.

| budget | value | headroom on real code |
|---|---|---|
| memory | `tsasm_parser_set_memory_limit`, default 1 GiB | the 1.9 MB Go file that needs the most holds 59 MB (`test/memprof`) |
| work | 64 units per input byte + 10000 | worst accepted file ≥ 1 KB uses 1.30 units/byte (Go), 0.85 (C), 0.56 (Java), 0.78 (JSON) |
| tree nodes | 32 per input byte + 100000, at most 2^27 | 2^27 keeps every 32-bit size computation exact |
| input length | below 4 GiB | byte offsets are 32-bit, as in tree-sitter |

## How it maps tree-sitter semantics

- **Lexing.** The parse state's lex mode picks the lexer entry. When the word token is lexed, the
  keyword lexer runs. The keyword is used if it covers the whole word and the state has an action
  for it, or the state's reserved-word set (ABI 15) contains it. A keyword with no action after
  reductions falls back to the word token. UTF-8 decodes like `ts_decode_utf8`. A token accepted
  before skipped whitespace starts where it ends, as in `ts_lexer_finish`.
- **Shift and reduce.** `SHIFT_REPEAT` actions are dropped, as `ts_parser__advance` does. A reduce
  pops N non-extra entries; extras in between become children, and trailing extras are pushed
  back above the new node. Accept splices the root's children with leading and trailing extras.
- **Output.** Hidden nodes are flattened. Aliases come from the parent's production alias
  sequence, indexed by structural child position. An alias makes a hidden node visible.
- **Fields.** `gen.py` turns each production's field map into a field id per structural child
  position (the first entry that isn't inherited, as the C cursor reads it). The output walk
  gives a node the field at its own position, or else the one its deepest hidden ancestor got,
  which each hidden node's frame passes down; extras get none. It costs about 1-2% of parse
  time (`test/mtspeed`, GOROOT: one thread 1.032-1.036 s per pass against 1.016-1.023,
  20 threads 0.090 s against 0.088).
- **Token cache.** A single-entry cache follows the C runtime's `can_reuse_first_leaf` rules,
  which matters when several versions lex at the same position.
- **GLR.** A port of C tree-sitter's graph-structured stack (`stack.c`) and GLR driver
  (`ts_parser__advance`, `ts_parser__reduce`, `ts_parser__accept`, `ts_parser__condense_stack`),
  limited to what error-free parsing needs. Stack nodes are 24 bytes with one inline link and
  up to 7 more in 64-byte extension blocks (C's `MAX_LINK_COUNT` 8). A pop walks every path with
  up to 64 iterators (`MAX_ITERATOR_COUNT`) and groups the results into slices as `add_slice`
  does. `select_children` picks between alternative child lists (dynamic precedence, then
  `ts_subtree_compare`). Heads in the same state at the same position merge, recursively
  merging links, and condense keeps at most 6 versions (`MAX_VERSION_COUNT`), as C does, so
  there is no separate version cap. Left out: error costs and recovery, pause/resume (a version
  with no action dies), external tokens and incremental reuse. Two fast paths sit in front of
  the general code: a reduce whose path has one link per node builds in place and frees the
  popped nodes, and a reduce that forks into distinct predecessors skips the iterator walk.
  A mark-and-sweep pass recycles the rest when the node arena fills. `test/proto.c` is the C
  prototype the assembly follows; `make proto` checks it like the library.

## Results

### Parity

An exact match of the full node arrays (symbol, named, field, start, end, parent) against C
tree-sitter v0.25.0's cursor walk, over files C parses without errors. `scripts/parity.sh`
reruns all four and fails on any difference from `results/parity_<lang>.txt`.

| grammar | corpus | exact match | mismatch | tsasm rejects, C accepts |
|---|---|---|---|---|
| Go | Go 1.22 `src`, 6513 files, 70.3 MB | 6464 / 6464 (**100%**) | 0 | 0 |
| C | system headers and C testdata, 8732 files, 436 MB | 3556 / 3556 | 0 | 0 |
| Java | Guava, 3267 files, 29.7 MB | 3254 / 3254 | 0 | 0 |
| JSON | app resources and testdata, 2539 files, 48.7 MB | 2529 / 2529 | 0 | 0 |

On every file C rejects, tsasm rejects too (49 Go, 5176 C, 13 Java, 10 JSON).

### Speed and scaling

Measured with `bench/` (`make bench-asm`) on 2026-11-22. The corpus is go1.26.5 `src` (7710
files, 90.3 MB), held in memory, with files handed out largest first. Each cell is the median
of 5 timed passes after a warm-up, C and asm in the same session, fuzzers paused. The machine is
an i9-13905H (6 P-cores with SMT, 8 E-cores, 20 threads) on AC. "1w" is one worker pinned to a
P-core.

| workers | C tree-sitter 0.25.0 | tsasm | tsasm / C |
|---|---:|---:|---:|
| 1 | 7.8 MB/s | 72.3 MB/s | 9.3× |
| 6 | 47.9 MB/s | 402.7 MB/s | 8.4× |
| 12 | 76.4 MB/s | 599.6 MB/s | 7.8× |
| 20 | 93.0 MB/s | 752.1 MB/s | 8.1× |

C's column is parse only. tsasm's includes emitting the flat node array; bench copies it into
a Go slice on top. On this corpus tsasm and C both accept 7654 files; parity is 100% (7654 / 7654). gotreesitter v0.58.0 in the same harness (12:38, same session as C): 1.0, 2.7, 2.2 and 2.3 MB/s;
with a local patch that replaces its `runtime.ReadMemStats` calls (which pause every goroutine)
with `runtime/metrics` (12:52, `bench/gts_perf.md`): 1.0, 5.9, 8.3 and 9.6 MB/s. Parity 92.54%
either way. Against the patched build tsasm is 68–78× faster at every worker count.

Single-thread cost of the reentrant, multi-grammar, graph-structured-stack runtime against the
original static Go-only build: about +3.1% (old 1.199–1.220 s, new 1.236–1.260 s per pass, 6
interleaved rounds on one P-core, per-round +2.5% to +4.0%, `results/ab_single_thread_gss.txt`).
The GSS port first cost +36%; in-place and fork fast paths, inlined shift and reduce, and inline
merge checks brought it back. The pre-GSS figure was also +3.1% (`results/ab_single_thread.txt`).
Freeing big parses' buffers (the 32 MiB retain default) costs another 4.1% on one thread (1.364–1.367
s before, 1.421–1.426 s after, 6 interleaved rounds) and 1.6% at 20 threads, for 30% less peak
memory at 20 threads (0.83–0.88 GB to 0.58–0.62 GB with the corpus, `results/ab_retain.txt`).
Tree nodes then shrank from 32 to 24 bytes (8 were padding): a big Go parse needs 24.6 bytes of
buffers per input byte instead of 30.1, one thread is 0.4% faster and 20 threads 5.5% faster
(0.115–0.116 s against 0.121–0.123 s per pass), peak memory 0.53–0.55 GB against 0.59–0.60
(`results/ab_tree24.txt`). A shifted token's leaf then dropped to 16 bytes (it has no children, so no
child index or precedence; reads of a childless tree's precedence give 0): 22.3 bytes per input
byte for big Go files, Java -9%, JSON -11%, speed unchanged or slightly faster
(`results/ab_leaf16.txt`). Versions that shift the same token now share its leaf, as C pushes one
lookahead subtree onto each; before, nearly every dead leaf was such a duplicate. Big Go files 21.3
bytes per input byte, Java -2%, speed unchanged (`results/ab_leafshare.txt`).

**What limits scaling.** Measured with `scripts/scaling_pinned.sh` on the commit before the
retention change, with the clock of the busy CPUs sampled during each run (`results/scaling.txt`):

- Per-GHz throughput holds from 1 to 6 P-cores: 18.7 to 20.2 MB/s per GHz per worker. Six
  workers on six P-cores give 409 MB/s, 5.8× one worker. Nothing shared slows the workers down.
- SMT adds 38% per core (6 workers on 3 cores' 6 threads: 282 MB/s; 12 workers on 6 cores:
  545 MB/s).
- An E-core does 0.80× a P-core's work per GHz, and runs at 2.5–2.7 GHz against the P-cores'
  3.2–3.8 GHz.
- At 20 workers the package power limit pulls the P-cores to 3.19 GHz and the E-cores to
  2.53 GHz. A model built from the three numbers above predicts 818 MB/s at those clocks; the
  measurement was 767 MB/s (94%). C reaches 99% of the same model.

So throughput follows core-GHz: SMT, E-cores and the power limit. The library has no locks and
no shared writable data, and nothing in the measurements points to a memory-bandwidth wall. A
software fix could recover at most the 6% gap to the model at 20 workers. C scales by a slightly
larger factor (11.9× against 10.3× from 1 to 20 workers). Its clocks stay higher under load
(P-cores at 3.65 GHz against 3.19 GHz with 20 workers), which is measured. That its lower IPC
draws less power is an inference.

### Safety evidence

**Fuzzing.** `fuzz/target.c` is a libFuzzer target built once per grammar. The assembly gets
libFuzzer coverage counters (`scripts/covinstr.py`), and the harness and the C reference build
with AddressSanitizer. The input sits against a `PROT_NONE` page, after its last byte in one run
and before its first byte in the other (`TSASM_GUARD=over|under`), so a read past either end
faults. The asm's own buffers come from the same guard-page allocator. Each input is checked
five ways:

1. a reused parser's tree satisfies the output contract (symbol range, spans inside the parent
   and the input, pre-order parent chain);
2. a fresh parser gives the same bytes, so no state leaks between parses;
3. an allocation failure injected at an input-chosen point gives `TSASM_ERR_NOMEM` or the same
   tree, and the parser then reparses to the same tree;
4. a low memory limit gives `TSASM_ERR_LIMIT` or the same tree;
5. tsasm never accepts what C tree-sitter rejects.

Five rounds on 2026-11-22, 8 jobs each (4 grammars × 2 guard modes), fork mode, inputs up to
8 KiB, 20 s timeout, memory capped:

| round | code | wall time | executions | findings |
|---|---|---:|---:|---|
| 1 | first harness build | 57 min | about 600,000 | 2 real bugs, 1 harness bug |
| 2 | after the round 1 fixes | 54 min | 2,663,018 | none |
| 3 | after the speed work | 16 min | 902,519 | none |
| 4 | final pre-GSS library | 150 min | 9,589,679 | none |
| 5 | graph-structured stack (shipped) | 61 min | 4,097,240 | none |
| total | | 5.6 h (45 job-hours) | about 17.9 million | |

Rounds 2 and 3 were stopped early so the next round would fuzz the code that ships. Each restart
replayed the whole accumulated corpus (about 5,000 C, 2,000 Go, 2,000 Java and 630 JSON inputs)
through the new binaries in both guard modes first, with no failures. Round 5 replayed all
11,425 saved inputs through the GSS build first, then fuzzed it: 0 crashes, out-of-memory
kills or oracle failures. libFuzzer saved one timeout and five slow units, all written in the
same second across four grammars while valgrind loaded the machine; each replays in at most
27 ms and passes every check, so they were a host stall (kept in `round5/stall_artifacts`).
Coverage on the GSS build: 15,506 counters hit in C, 8,755 in Java, 7,289 in Go and 1,683 in
JSON.

Round 1 found two real bugs, both fixed and kept in `fuzz/regress/` (`make fuzz-regress`
replays them in both guard modes):

- The generated lexer's set-membership helper took its count in `ecx`, the register that holds
  the accepted symbol. A non-ASCII lookahead that failed a set test after an accept returned
  the count as the token. This bug was in the baseline generator.
- A token accepted before skipped whitespace had its start after its end. C resets the start
  to the end, and now tsasm does too.

**Stress suite** (`make stress`, `test/stress.c`, capped at 8 GB): 126 of 126 checks pass on
the final library in 124 s.

- 124 pathological inputs: nesting to 10^7 levels, ambiguous parameter lists, generics,
  unterminated strings and comments, NUL bytes, random bytes and token soup up to 10 MB, lengths
  from 0xFFFFFFF0 to 2^64-1 over a `PROT_NONE` page (rejected without touching a byte), memory
  limits from 0 to 4 GiB, and a 220 MB valid Go file. Each must give its expected class
  (accept, syntax error, or limit), confirmed against C tree-sitter on a small instance, and the
  parser must parse correctly afterwards.
- Ambiguous parameter lists no longer copy stacks: `f(a, a, …, a int)` with 100,000 identical
  names parses in 0.18 s (it returned `TSASM_ERR_LIMIT` at 400 names before the GSS port).
- Retention (resident memory): a 20 MB parse peaks at +495 MiB and returns holding only its
  86 MiB node array; the next small parse drops the parser to +1.5 MiB and freeing it to +0.7
  MiB. With `tsasm_parser_set_retain(p, 0)` lowering the retain frees the node array at once
  (+0.00 MiB). On the retention build: 126/126 checks, and 20 threads for 60 s ran 3.56M parses
  (random memory limits and random retain settings included) with 0 mismatches.
- Threads: 20 threads for 60 s ran 5.13M parses (persistent parsers, a fresh parser per file,
  random memory limits, and mutated inputs) checked against a single-threaded reference: 0
  mismatches, 0 anomalies.

**valgrind.** memcheck over a 1-in-20 sample of every corpus on the GSS build (326 Go, 437 C,
164 Java and 127 JSON files): 0 errors, 0 bytes definitely or indirectly lost; what remains at
exit is the harness's 64 MiB compare buffer and the C reference's parser. The pre-GSS build
gave the same result on 385/436/163/126 files. helgrind with 4 threads over 100 Go and 100 Java
files on the GSS build: 0 errors. On the pre-GSS build, helgrind and DRD with 4 threads over 100 Go and
100 Java files: 0 errors. The only suppressions are valgrind's default glibc and ld entries.

**Static state.** `make check-static` reads every object's section table and fails on any
non-empty allocated, writable section other than `.data.rel.ro`, which the loader makes
read-only after relocation. The library has none.

## Development tools

    make tools          # test/check_all, test/mtspeed, test/one, test/fuzz (needs the
                        # go-tree-sitter v0.25.0 module source for the C reference)
    sh scripts/parity.sh        # parity of all four grammars vs results/ (memory-capped)
    make stress                 # stress suite, about 5 minutes, capped at 8 GB
    make stress-smoke           # under 2 minutes
    make fuzz                   # one libFuzzer binary per grammar in build/fuzz/
    make fuzz-regress           # replay every saved finding in fuzz/regress/
    fuzz/run.sh go over 3600    # fuzz Go for an hour, guard page after the input
    sh scripts/scaling.sh       # asm vs C throughput at 1/6/12/20 workers

Layout:

- `gen/gen.py` turns a grammar's `parser.c` into `build/lang_<name>.s`: dense parse table,
  actions, lex modes, reserved words, symbol tables, alias sequences, names, the lexer state
  machines and the `TsasmLanguage` descriptor.
- `src/parser.s` is the runtime.
- `test/ref.c` is the C tree-sitter reference emitting the same `TsasmNode` format.
  `test/check.c` compares per file. `test/stress.c` is the stress suite. `test/mtspeed.c`
  measures throughput at N workers for either engine.
- `fuzz/target.c` is the libFuzzer target, `fuzz/guard.c` the guard-page allocator, and
  `scripts/covinstr.py` adds libFuzzer coverage counters to the assembly.
- `decisions.tsv` is the decision log of the reentrant and multi-grammar work.

## What ARM64 support would take

Everything outside the two assembly emitters carries over unchanged: the tables gen.py builds,
the C reference, and the parity, fuzz, stress and scaling harnesses. The work is three pieces.

1. **Port `src/parser.s` (about 1,700 lines) to AArch64.** The algorithm and data layout stay.
   The instructions change throughout.
   - Memory operands go away. x86 adds, compares and multiplies straight from memory
     (`imul eax, [r15+C_LANG+L_NSYM]`, `cmp r14d, [r15+C_OUTCAP]`); AArch64 loads first.
   - Addressing is narrower. `[base + index*scale + disp]` becomes an `add` plus
     `ldr x, [base, index, lsl #n]`.
   - `push`/`pop` become `stp`/`ldp` pairs with the stack kept 16-byte aligned at all times.
     `cmov` becomes `csel`, `bt`/`btr` become `tbz`/`tbnz` and `bic`, and `rep stos`/`rep movs`
     become loops or calls to `memset`/`memcpy`.
   - There are 31 general registers instead of 16. The context pointer can stay in a
     callee-saved register (x28 for r15), and the hot table bases (parse table, actions, symbol
     output words) can live in x19 to x27 for the whole parse instead of being reloaded from
     the descriptor. Expect fewer loads per token than on x86.
   - The unwinding scheme carries over. `p_limit`/`p_oom` reset `sp` from the saved value and
     branch to the epilogue.
2. **Add an AArch64 backend to the lexer emitter in `gen/gen.py`.** The lexer is generated
   code, so this is a second emitter (a few hundred lines), not hand porting. Jump-table
   dispatch becomes `adr` + `ldrsw` of a relative offset + `br`, which stays position
   independent. The lexer's register contract (position, end, state, token start and end) maps
   onto x-registers one to one.
3. **Port the coverage instrumenter.** `scripts/covinstr.py` saves flags with `pushfq`/`popfq`
   around each counter increment. On AArch64 `ldrb`/`add`/`strb` without the `s` suffix leave
   NZCV alone, so the increment needs only two scratch registers.

Platform targets differ. Linux AArch64 uses the same ELF directives (`.type`, `.size`,
`.section .data.rel.ro`) and can be tested with qemu-user for correctness. Apple Silicon needs
Mach-O syntax as well: `_`-prefixed symbols, `@PAGE`/`@PAGEOFF` relocations, `__TEXT,__const`
sections and no `.type`/`.size`. A thin macro layer or a per-platform emitter covers it. In both
cases `scripts/parity.sh`, the fuzz targets and the stress suite then verify the port without
changes.

## Known gaps

- **No error recovery,** by design: inputs with syntax errors return `TSASM_ERR_SYNTAX`.
- **The memory limit counts capacity,** not bytes in use, so a parser's resident memory is
  usually about half its charged total.
- **The 2^27 node cap** limits one input to roughly 200 MB of Go. The stress suite's 220 MB Go
  input returns `TSASM_ERR_LIMIT` even with the memory limit raised.
- **x86-64 only.** See the ARM64 section.
