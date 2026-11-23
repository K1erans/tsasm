# tsasm: a reentrant tree-sitter parser runtime in x86-64 assembly

`tsasm` (`libtsasm.a` and the Go package `github.com/K1erans/tsasm`) parses source text with tree-sitter's parse tables and returns the visible syntax tree as a flat, pre-order node array.

The parser loop, GLR stack forking and merging, tree arena, and pre-order output walk are hand-written x86-64 GNU assembly (`src/parser.s`). An assembly generator (`gen/gen.py`) compiles each grammar's tree-sitter `parser.c` into native x86-64 lexer state machines and read-only parse tables. No C runs at parse time. `libc` is used only for `realloc`, `free`, and `aligned_alloc`, while buffers of 128 KiB or larger use `mmap` and `mremap` directly.

## At a glance

Measured on the Go 1.26.5 standard library (`src/`, 7,710 files, 90.3 MB in memory) on an Intel i9-13905H (6 P-cores with SMT, 8 E-cores, 20 threads):

| Workers | C tree-sitter 0.25.0 | `gotreesitter` 0.58.0 (patched) | `tsasm` | `tsasm` vs C | `tsasm` vs `gotreesitter` |
|---|---:|---:|---:|---:|---:|
| 1 (pinned P-core) | 7.8 MB/s | 1.0 MB/s | **72.3 MB/s** | **9.3x** | **72x** |
| 6 | 47.9 MB/s | 5.9 MB/s | **402.7 MB/s** | **8.4x** | **68x** |
| 12 | 76.4 MB/s | 8.3 MB/s | **599.6 MB/s** | **7.8x** | **72x** |
| 20 | 93.0 MB/s | 9.6 MB/s | **752.1 MB/s** | **8.1x** | **78x** |

C tree-sitter's numbers measure parse time alone. `tsasm`'s numbers include emitting the flat pre-order node array and copying it into a Go slice.

Across all four built-in ABI 15 grammars, `tsasm` matches C tree-sitter v0.25.0's cursor walk **100% node-for-node** (symbol, named flag, grammar field ID, byte range, and parent index) on every error-free file:

| Grammar | Reference version | Corpus | Exact node-array match | Mismatches | Function |
|---|---|---|---:|---:|---|
| Go | `tree-sitter-go` v0.25.0 | Go 1.22 `src` (6,513 files, 70.3 MB) + Go 1.26.5 `src` (7,710 files, 90.3 MB) | **6,464 / 6,464** and **7,654 / 7,654 (100%)** | 0 | `tsasm_language_go()` |
| C | `tree-sitter-c` v0.24.2 | System headers and C testdata (8,732 files, 436 MB) | **3,556 / 3,556 (100%)** | 0 | `tsasm_language_c()` |
| Java | `tree-sitter-java` v0.23.5 | Guava (3,267 files, 29.7 MB) | **3,254 / 3,254 (100%)** | 0 | `tsasm_language_java()` |
| JSON | `tree-sitter-json` v0.24.8 | App resources and testdata (2,539 files, 48.7 MB) | **2,529 / 2,529 (100%)** | 0 | `tsasm_language_json()` |

On every file that C tree-sitter rejects with a syntax error (49 Go, 5,176 C, 13 Java, 10 JSON), `tsasm` rejects the file as well.

## Why it is fast

- **Compiled assembly lexers and dense tables.** `gen/gen.py` compiles each grammar's `parser.c` into position-independent x86-64 jump tables for the lexer and dense lookup tables in `.data.rel.ro`. Switching languages on a parser swaps one read-only descriptor pointer.
- **Graph-structured stack (GLR) with single-link and fork fast paths.** `src/parser.s` ports C tree-sitter's graph-structured stack (`stack.c`) and GLR driver (`advance`, `reduce`, `accept`, `condense_stack`) with two fast paths in front of the general iterator walk. A reduction whose path has one link per node builds in place and recycles popped stack nodes immediately. A reduction that forks into distinct predecessors skips the general iterator grouping pass. Those fast paths recover a 36% general-GSS overhead down to 3.1% over a static single-stack parser (`results/ab_single_thread_gss.txt`).
- **Compact arena nodes and shared lookahead leaves.** Internal parse-tree nodes are 24 bytes and shifted leaf tokens are 16 bytes (down from 32 bytes). When multiple GLR stack versions shift the same token, they share one 16-byte leaf allocation. Large Go parses use 21.3 bytes of working buffer per input byte (`results/ab_leafshare.txt`).
- **Flat 16-byte pre-order output.** Instead of returning a pointer-linked tree that callers must walk through a stateful C cursor via cgo, `tsasm` emits visible nodes into a contiguous `TsasmNode` array (`16 bytes` per node: `symbol`, `named`, `field`, `start_byte`, `end_byte`, `parent`).
- **Zero shared mutable state.** Every buffer, arena, and limit lives inside the caller's `TsasmParser`. `make check-static` inspects the ELF section tables of `libtsasm.a` and fails the build if any object contains a writable static section.
- **Direct `mmap` and `mremap` with bounded retention.** Working buffers of 128 KiB or larger bypass `malloc` thresholds via `mmap` (with `MADV_HUGEPAGE`) and grow via `mremap`. Between parses, each parser retains at most 32 MiB of working buffers by default (`tsasm_parser_set_retain`) and returns oversized arenas directly to the kernel, cutting 20-thread peak RSS by 30% (`results/ab_retain.txt`).

## Quick start

### Go (`linux/amd64`, cgo)

The repository root is a Go package. `cgo` assembles the pre-generated `.s` files directly with the system C toolchain, so `go get` requires no separate `make` step or `CGO_LDFLAGS`:

```sh
go get github.com/K1erans/tsasm
```

```go
package main

import (
	"fmt"
	"log"

	"github.com/K1erans/tsasm"
)

func main() {
	p, err := tsasm.NewParser() // one per goroutine
	if err != nil {
		log.Fatal(err)
	}
	defer p.Close()

	lang := tsasm.Lookup("go")
	nodes, err := p.Parse(lang, []byte("package main\nfunc main() {}\n"))
	if err != nil {
		log.Fatal(err) // tsasm.ErrSyntax, ErrLimit, or ErrNoMem
	}
	for _, n := range nodes { // valid until p's next Parse or Close
		fmt.Println(lang.FieldName(n.Field), lang.SymbolName(n.Symbol), n.Start, n.End, n.Parent)
	}
}
```

### C (`libtsasm.a`)

The full contract is in `tsasm.h`:

```c
#include "tsasm.h"

TsasmParser *p = tsasm_parser_new(); // one per thread
const TsasmNode *nodes;
int64_t n = tsasm_parse(p, tsasm_language_go(), src, len, &nodes);
if (n > 0) {
    // nodes[0..n) holds the visible syntax tree in pre-order,
    // valid until the next parse on p or tsasm_parser_free(p).
}
tsasm_parser_free(p);
```

- **Caller-owned context.** A `TsasmParser` owns all mutable buffers, the output array, and the memory limit. Threads parse concurrently with one parser each, and a parser may move between threads between calls.
- **Read-only languages.** A `TsasmLanguage` is an immutable descriptor in `.data.rel.ro` shared across all threads.
- **Node layout.** Each `TsasmNode` is 16 bytes:
  - `symbol` (`uint16_t`): public grammar symbol after alias resolution, matching `ts_node_symbol`. `tsasm_language_symbol_name()` returns the same string as `ts_node_type`.
  - `named` (`uint8_t`): 1 for named nodes, 0 for anonymous tokens.
  - `field` (`uint8_t`): grammar field ID (0 for none), matching `ts_tree_cursor_current_field_id`. `tsasm_language_field_name()` returns its string name.
  - `start_byte`, `end_byte` (`uint32_t`): byte offsets in the source slice.
  - `parent` (`int32_t`): index of the parent node in `nodes`, or `-1` for the root.
- **Errors.** `TSASM_ERR_SYNTAX` (`-1`) when the input has a syntax error (no error recovery), `TSASM_ERR_LIMIT` (`-3`) when a budget is exceeded, and `TSASM_ERR_NOMEM` (`-4`) when allocation fails. After any error, `*nodes` is `NULL` and the parser remains valid for reuse.

### Resource budgets

Every parse runs inside deterministic resource caps. Exceeding any budget returns `TSASM_ERR_LIMIT` and leaves the parser reusable:

| Budget | Limit | Headroom on real corpora |
|---|---|---|
| Memory | `tsasm_parser_set_memory_limit` (default 1 GiB) | The largest 1.9 MB Go file peaks at 59 MB (`test/memprof`). |
| Work steps | `64 * input_bytes + 10,000` | Worst accepted file >= 1 KB uses 1.30 units/byte (Go), 0.85 (C), 0.56 (Java), 0.78 (JSON). |
| Tree nodes | `min(2^27, 32 * input_bytes + 100,000)` | `2^27` keeps every 32-bit byte-size calculation exact (caps one file at ~200 MB of Go). |
| Input length | `< 4 GiB` | Byte offsets are 32-bit unsigned integers, matching tree-sitter. |

`tsasm_parser_set_retain(p, bytes)` controls how much working memory a parser keeps between parses (default 32 MiB). If a parse needed more than the retain cap, its internal arenas are unmapped before `tsasm_parse` returns, leaving only the returned `TsasmNode` array.

## Benchmarks and scaling analysis

### Multi-core scaling (`results/scaling.txt`)

Measured with `scripts/scaling_pinned.sh` on Go 1.26.5 `src` (7,710 files, 90.3 MB), sampling the active CPU clock frequencies during each run:

- **1 to 6 P-cores:** Per-GHz throughput stays flat at **18.7 to 20.2 MB/s per GHz per worker**. Six workers pinned to six physical P-cores reach **409.4 MB/s** (5.8x one worker). No shared lock or cache line slows the workers down.
- **SMT contribution:** Running 12 workers across all 6 P-cores (12 threads) reaches **545.0 MB/s** (+38% per physical core).
- **E-core throughput:** An E-core retires **0.80x** a P-core's work per GHz and runs at 2.5 to 2.7 GHz. Eight workers pinned to the 8 E-cores reach **293.1 MB/s**.
- **20-thread package power limit:** With all 20 threads active, the laptop's package power limit pulls P-cores from 3.76 GHz down to 3.19 GHz and E-cores to 2.53 GHz. A hardware model from the three measurements above predicts 818 MB/s at those clocks; `tsasm` measures **767.0 MB/s** (94% of the linear core-GHz ceiling).

### Arena retention vs memory (`results/ab_retain.txt`)

On Go 1.26.5 `src` with 20 workers, freeing buffers above 32 MiB after large files drops peak RSS by 30% (from 0.83 to 0.88 GB down to 0.58 to 0.62 GB) at a 1.6% throughput cost. Inside [jinter](https://github.com/K1erans/jinter)'s chunk splitter (`chunk.Split`), a 32 MiB retain cap parses at **325 to 332 MB/s** across 20 workers, while an 8 MiB retain cap cuts peak RSS above the corpus by 58% (1.78 GB down to 735 MB) for a 6% speed trade-off.

### Comparison with `gotreesitter` (`bench/gts_perf.md`)

In the same benchmark harness on Go 1.26.5 `src`, stock `gotreesitter` v0.58.0 runs at 1.0 MB/s (1 worker) and 2.3 MB/s (20 workers) because its per-parse `runtime.ReadMemStats` calls stop the world across all goroutines. Replacing `ReadMemStats` with `runtime/metrics` raises `gotreesitter` to 1.0, 5.9, 8.3, and 9.6 MB/s at 1, 6, 12, and 20 workers (with 92.54% file acceptance parity against C tree-sitter). Against the patched build, `tsasm` is **68x to 78x faster** at every worker count with 100% parity.

## Safety and verification

### Coverage-guided fuzzing (`fuzz/target.c`)

`scripts/covinstr.py` instruments the assembly with libFuzzer coverage counters, and both the harness and the C reference build with AddressSanitizer. Each input buffer is placed flush against a `PROT_NONE` guard page, after the last byte in `TSASM_GUARD=over` and before the first byte in `TSASM_GUARD=under`, and the assembly's own arenas use the same guard-page allocator. Every fuzz input is checked five ways:

1. A reused parser's output satisfies the structural contract (valid symbol IDs, spans within parent and input bounds, valid pre-order parent chain).
2. A fresh parser produces identical output bytes, proving no state leaks across parses.
3. Injecting an allocation failure at an input-selected call site returns either `TSASM_ERR_NOMEM` or the exact tree, and the next parse on the same parser still succeeds with the exact tree.
4. Running under a tight memory limit returns either `TSASM_ERR_LIMIT` or the exact tree.
5. `tsasm` never accepts an input that C tree-sitter rejects.

Across five fuzzing campaigns (45 job-hours, **17.9 million executions** across Go, C, Java, and JSON in both guard modes, plus replaying all 11,425 accumulated corpus inputs on every build):

| Round | Target build | Wall time | Executions | Findings |
|---|---|---:|---:|---|
| 1 | Initial harness build | 57 min | ~600,000 | 2 bugs fixed (`fuzz/regress/`), 1 harness fix |
| 2 | Post-fix build | 54 min | 2,663,018 | 0 |
| 3 | Speed-optimized build | 16 min | 902,519 | 0 |
| 4 | Final pre-GSS library | 150 min | 9,589,679 | 0 |
| 5 | Graph-structured stack (shipped) | 61 min | 4,097,240 | 0 |

### Stress suite, Valgrind, static checks, and TLA+ formal verification

- **Pathological stress suite (`make stress`, `test/stress.c`):** 126 of 126 checks pass in 124 seconds under an 8 GB cap. Covers nesting to `10^7` levels, ambiguous parameter lists (`f(a, a, ..., a int)` with 100,000 identical identifiers parses in 0.18 s on the GSS engine), unterminated strings and comments, NUL bytes, 10 MB random token soup, lengths from `0xFFFFFFF0` to `2^64 - 1` backed by `PROT_NONE` pages, memory limits from `0` to `4 GiB`, a 220 MB valid Go file, and 20-thread concurrency torture runs (**5.13 million parses** over 60 seconds with random memory limits, retention caps, and mutated inputs: 0 mismatches).
- **Valgrind (`memcheck`, `helgrind`, `drd`):** `memcheck` on a 1-in-20 sample of all four corpora (1,054 files) reports 0 errors and 0 bytes definitely or indirectly lost. `helgrind` and `drd` with 4 threads across 100 Go and 100 Java files report 0 race or lock errors.
- **Zero writable static state (`make check-static`):** Verifies that no object file in `libtsasm.a` has any non-empty writable allocated section other than `.data.rel.ro`.
- **TLA+ formal verification (`verification/parser-memory/`, `verification/glr-stack/`):** Two TLA+ models tied line-by-line to `src/parser.s` via `anchors.tsv` model-check (1) buffer growth, `C_MEMLIMIT` charging, `MAP_MIN` heap-to-`mmap` migration, per-parse `C_NAVAIL` node budgets, mid-parse `p_limit` / `p_oom` unwinding, and `C_RETAIN` inter-parse retention (`ParserMemory.tla`, 7 invariants, 7 mutant checks + fix verification), and (2) the GLR graph-structured stack, single-link in-place node recycling, extension-block attachment, version condensation, and mark-and-sweep stack-node GC (`GlrStack.tla`, 6 invariants, 9 mutant checks). Run both models and all 16 mutant checks with `verification/parser-memory/check.sh --all --mutants`.

## How it maps tree-sitter semantics

- **Lexing.** The current parse state's lex mode selects the lexer entry point. When a word token is scanned, the keyword lexer runs and promotes the token if the keyword matches the full word and either has a valid action in the current state or belongs to the state's ABI 15 reserved-word set. If a keyword has no valid action after pending reductions, lexing falls back to the word symbol. UTF-8 decoding matches `ts_decode_utf8`. A token accepted before skipped whitespace resets its start offset to its end offset, matching `ts_lexer_finish`.
- **Shift, reduce, and accept.** `SHIFT_REPEAT` actions are dropped, matching `ts_parser__advance`. A reduction pops `N` non-extra stack entries, attaches intervening extras as children, and pushes trailing extras back above the reduced node. Accept splices leading and trailing extras into the root node's children.
- **Hidden nodes, aliases, and fields.** Hidden nodes are flattened during the pre-order output walk. Aliases are resolved from the parent production's alias sequence by structural child position, making aliased hidden nodes visible. `gen/gen.py` precomputes a field ID table per production and structural child index; the output walk assigns each visible node its own structural field ID or the field ID passed down from its deepest hidden ancestor (adding ~1.5% to parse time).
- **Single-entry token cache.** Follows C tree-sitter's `can_reuse_first_leaf` rules when multiple GLR versions lex at the same byte offset.
- **Intentional omissions.** Error recovery, pause/resume, external C scanners, and incremental edit reuse are omitted by design.

## Build and development tools

```sh
make                        # build libtsasm.a (requires python3, GNU as, ar)
make check-static           # fail if libtsasm.a has any writable static data
make gopkg                  # copy src/parser.s and build/lang_*.s to root Go package
make check-gopkg            # verify Go package assembly copies are up to date
make tools                  # build test/check_all, test/mtspeed, test/one, test/fuzz
sh scripts/parity.sh        # verify 100% node parity across Go, C, Java, and JSON
make stress                 # run full 126-check stress suite (~2 min)
make fuzz                   # build libFuzzer binaries per grammar in build/fuzz/
make fuzz-regress           # replay regression corpus in both guard-page modes
verification/parser-memory/check.sh --all --mutants # run TLA+ models and mutant checks
sh scripts/scaling.sh       # benchmark tsasm vs C tree-sitter at 1, 6, 12, and 20 workers
```

`make` locates each grammar's `parser.c` in the local Go module cache (override with `GRAMMAR_<name>=/path/to/parser.c`). To add a new ABI 15 grammar (without an external scanner, with at most 256 production IDs and alias sequences of length at most 16), add its name to `GRAMMARS` in `Makefile`, declare `tsasm_language_<name>()` in `tsasm.h`, and add an entry to `test/langs.h`.

### Repository layout

- `src/parser.s`: x86-64 GLR runtime, arena allocator, and pre-order output emitter.
- `gen/gen.py`: compiles a tree-sitter `parser.c` into `build/lang_<name>.s` (parse tables, actions, lex modes, reserved words, symbol and field names, lexer state machines, and `TsasmLanguage` descriptor).
- `tsasm.h`, `tsasm.go`: C header and Go cgo wrapper.
- `zparser_linux_amd64.s`, `zlang_*_linux_amd64.s`: pre-generated assembly files for `go get`.
- `test/`: C tree-sitter parity harness (`ref.c`, `check.c`), multi-threaded benchmark (`mtspeed.c`), memory profiler (`memprof.c`), C prototype (`proto.c`), and stress suite (`stress.c`).
- `fuzz/`: libFuzzer harness (`target.c`), `PROT_NONE` guard-page allocator (`guard.c`), and regression inputs (`regress/`).
- `scripts/covinstr.py`: assembly basic-block instrumenter for libFuzzer coverage counters.
- `verification/`: TLA+ formal models (`ParserMemory.tla`, `GlrStack.tla`), code-anchor harnesses (`check.sh`, `anchors.tsv`, `expected.tsv`), and C counterexample reproduction (`repro.c`).
- `results/`: raw parity, A/B optimization, retention, and CPU-pinned scaling logs.
