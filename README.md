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
  `ts_node_symbol`), `named`, `start_byte`, `end_byte`, `parent` (index into the same array,
  -1 for the root). Nodes are the visible ones (named and anonymous, as tree-sitter's
  `ts_node_child` walk shows them) in pre-order. `tsasm_language_symbol_name()` gives the same
  strings as `ts_node_type`.
- **Errors.** `TSASM_ERR_SYNTAX` (-1): the input is not in the language. There is no error
  recovery. `TSASM_ERR_LIMIT` (-3): a budget ran out. `TSASM_ERR_NOMEM` (-4): the allocator
  failed. After any error the parser stays usable and `*nodes` is NULL.
- **Memory.** `tsasm_parser_set_memory_limit()` caps the bytes a parser may hold (1 GiB by
  default). Growth is charged against the limit before it is allocated. Between parses a parser
  keeps its buffers, and drops them at the next parse if they exceed both an eighth of the limit
  (128 MiB by default) and 256 bytes per byte of that parse's input. `tsasm_parser_free()`
  frees everything.

### Budgets

Every parse is bounded. Each budget returns `TSASM_ERR_LIMIT` and leaves the parser usable.

| budget | value | headroom on real code |
|---|---|---|
| memory | `tsasm_parser_set_memory_limit`, default 1 GiB | the 1.9 MB Go file that needs the most holds 92 MB |
| work | 64 units per input byte + 10000 | worst accepted file ≥ 1 KB uses 4.04 units/byte (Go), 0.84 (C), 0.74 (Java), 0.78 (JSON) |
| tree nodes | 32 per input byte + 100000, at most 2^27 | 2^27 keeps every 32-bit size computation exact |
| live GLR versions | 64 | one Go file in 6513 needs more (`crypto/tls/handshake_messages.go`) |
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
- **Token cache.** A single-entry cache follows the C runtime's `can_reuse_first_leaf` rules,
  which matters when several versions lex at the same position.
- **GLR.** Actions with several entries fork the version: each reduce runs on a copy and the
  shift stays on the original, with C's version ordering and renumbering. Versions advance in
  lockstep by byte position. Each version is a linear stack over a shared append-only tree
  arena. C's graph-structured-stack merges are emulated by convergence detection: two versions
  in the same state at the same position whose stacks agree below a divergent segment are
  resolved like `stack_node_add_link` and `select_children` (dynamic precedence, then keep the
  existing version if the subtrees are equivalent, else `ts_subtree_compare`). Versions C
  would hold as one stack head form a merge group, which changes how later conflicts between
  them resolve (this fixed Java's interface-versus-annotation cases).

## Results

### Parity

An exact match of the full node arrays (symbol, named, start, end, parent) against C
tree-sitter v0.25.0's cursor walk, over files C parses without errors. `scripts/parity.sh`
reruns all four and fails on any difference from `results/parity_<lang>.txt`.

| grammar | corpus | exact match | mismatch | tsasm rejects, C accepts |
|---|---|---|---|---|
| Go | Go 1.22 `src`, 6513 files, 70.3 MB | 6424 / 6464 (**99.38%**) | 39 | 1 (version cap) |
| C | system headers and C testdata, 8732 files, 436 MB | 3554 / 3556 | 2 | 0 |
| Java | Guava, 3267 files, 29.7 MB | 3235 / 3254 (99.42%) | 19 | 0 |
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
| 1 | 7.8 MB/s | 72.7 MB/s | 9.3× |
| 6 | 45.9 MB/s | 401.7 MB/s | 8.8× |
| 12 | 76.1 MB/s | 609.9 MB/s | 8.0× |
| 20 | 93.0 MB/s | 751.4 MB/s | 8.1× |

C's column is parse only. tsasm's includes emitting the flat node array; bench copies it into
a Go slice on top. On this corpus tsasm accepts 7653 files and C 7654; parity is 99.48%.

Single-thread cost of the reentrant, multi-grammar runtime against the original static Go-only
build: about +3.1% (old 1.214–1.227 s, new 1.252–1.264 s per pass, 4 interleaved pairs on one
P-core, `results/ab_single_thread.txt`).

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

<!-- FUZZTABLE -->

Round 1 found two real bugs, both fixed and kept in `fuzz/regress/` (`make fuzz-regress`
replays them in both guard modes):

- The generated lexer's set-membership helper took its count in `ecx`, the register that holds
  the accepted symbol. A non-ASCII lookahead that failed a set test after an accept returned
  the count as the token. This bug was in the baseline generator.
- A token accepted before skipped whitespace had its start after its end. C resets the start
  to the end, and now tsasm does too.

**Stress suite** (`make stress`, `test/stress.c`, capped at 8 GB): 126 of 126 checks pass on
the final library in 127 s.

- 124 pathological inputs: nesting to 10^7 levels, ambiguous parameter lists, generics,
  unterminated strings and comments, NUL bytes, random bytes and token soup up to 10 MB, lengths
  from 0xFFFFFFF0 to 2^64-1 over a `PROT_NONE` page (rejected without touching a byte), memory
  limits from 0 to 4 GiB, and a 220 MB valid Go file. Each must give its expected class
  (accept, syntax error, or limit), confirmed against C tree-sitter on a small instance, and the
  parser must parse correctly afterwards.
- Retention: after a parse holding 705 MiB, a small parse drops the parser to 0.04 MiB, and
  freeing it returns to the 0.02 MiB baseline.
- Threads: 20 threads for 60 s ran 2.81M parses (persistent parsers, a fresh parser per file,
  random memory limits, and mutated inputs) checked against a single-threaded reference: 0
  mismatches, 0 anomalies.

**valgrind.** memcheck over a 1-in-20 sample of every corpus (385 Go, 436 C, 163 Java and
126 JSON files): 0 errors, 0 bytes lost. helgrind and DRD with 4 threads over 100 Go and
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

- **Long ambiguous parameter lists** (the 39 Go mismatches). In `func f(a, b, …, z int64)` with
  about 10 or more names, every name can be read as a type or a parameter name. C's
  graph-structured stack, under its internal limits (`MAX_VERSION_COUNT` 6, `MAX_LINK_COUNT` 8,
  64 pop iterators), produces a degraded parse where the first name becomes a type-only
  `parameter_declaration`. tsasm keeps the correct single declaration. Matching C exactly needs
  a faithful GSS port. Long lists also copy linear stacks quadratically. A list of 400 or more
  identical one-letter names (`f(a, a, …, a int)`) returns `TSASM_ERR_LIMIT` from the work
  budget. 1,000 distinct names still parse (42 units per byte); real code peaks at 4.
- **Java's 19 mismatches** are `identifier` versus `type_identifier` choices where C's merged
  stack heads create paths that no single linear version holds. They also need a GSS.
- **The version cap.** One Go file needs more than 64 live versions and returns
  `TSASM_ERR_LIMIT`.
- **No error recovery,** by design: inputs with syntax errors return `TSASM_ERR_SYNTAX`.
- **The memory limit counts capacity,** not bytes in use, so a parser's resident memory is
  usually about half its charged total.
- **The 2^27 node cap** limits one input to roughly 200 MB of Go. The stress suite's 220 MB Go
  input returns `TSASM_ERR_LIMIT` even with the memory limit raised.
- **x86-64 only.** See the ARM64 section.
