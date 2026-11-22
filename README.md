# tsasm: a tree-sitter parser runtime in x86-64 assembly (Go grammar)

`libtsasm.a` parses Go source with the parse tables of tree-sitter-go v0.25.0 (generated
`parser.c`, ABI 15). The parser loop, GLR forking/merging, tree building and output walk are
hand-written GNU as (`src/parser.s`). The tables and the lexer are generated as assembly by
`gen/gen.py`: it translates the generated `ts_lex` / `ts_lex_keywords` C state machines into
asm state machines. No C code runs at parse time; libc `realloc` is used only to grow buffers.

## Build

    make            # -> libtsasm.a, header tsasm.h (needs python3, GNU as, ar)

`GRAMMAR=` overrides the path to tree-sitter-go's `src/parser.c`. Link with `libtsasm.a`. It
is position-independent (no text relocations) and works in PIE executables.

API: see `tsasm.h`. The output is visible nodes (named and anonymous) in pre-order. `symbol`
is the public symbol id after alias resolution, the same as `ts_node_symbol`.
`tsasm_symbol_name_go` gives the same strings as `ts_node_type`.

Dev tools (need the go-tree-sitter v0.25.0 module source for the C reference):

    make tools      # test/check_all, test/speed, test/fuzz, test/one
    make check      # parity vs C tree-sitter over $(go env GOROOT)/src (memory-capped)

## Layout

    gen/gen.py      parser.c -> build/tables.s (dense parse table, actions, lex modes, reserved
                    words, symbol flags/public map, alias sequences, names), build/lexer.s
    src/parser.s    hand-written runtime (lex/keyword extraction, shift/reduce, GLR, accept, emit)
    test/model.c    C model of the same algorithm; the spec the asm mirrors (results are identical)
    test/ref.c      C tree-sitter reference that emits the same TsasmNode format
    test/check.c    per-file comparison harness; test/speed.c, test/fuzz.c, test/one.c

## How it maps tree-sitter semantics

- Lexing: the parse state's lex mode is used. When the word token (`identifier`) is lexed, the
  keyword lexer runs. The keyword is used if it covers the whole identifier and the state has
  an action for it, or the state's reserved-word set (ABI 15) contains it. There is also the
  keyword→identifier fallback when a keyword has no action after reductions. If lexing fails,
  the parse returns -1. Positions are byte offsets. UTF-8 is decoded like `ts_decode_utf8`
  (invalid → -1, length 1).
- Shift/reduce: `SHIFT_REPEAT` actions are dropped (the C runtime ignores them in `advance`).
  A reduce pops N non-extra entries; extras in between become children and trailing extras are
  pushed back above the new node. Accept splices the root's children with the leading and
  trailing extras, as `ts_parser__accept` does.
- Output: hidden nodes are flattened. Aliases come from the parent's production alias sequence,
  indexed by structural (non-extra) child position. An alias makes a hidden node visible.
- Token cache: there is a single-entry cache with the C runtime's `can_reuse_first_leaf` rules,
  which matters when several versions lex at the same position.
- GLR: actions with several entries fork the version (each reduce runs on a copy; the shift
  stays on the original), with the same version ordering and renumbering as C. Versions advance
  in lockstep by byte position, as in `ts_parser_parse`. Each version is a linear stack and the
  tree arena is shared and append-only. C's graph-structured-stack merges are emulated by
  *convergence detection*: two versions with the same state and position whose stacks are
  identical below a divergent segment are resolved like C's `stack_node_add_link` /
  `select_children`. That means dynamic precedence first, then keep the existing version if the
  subtrees are equivalent, otherwise `ts_subtree_compare`. Versions are also reordered by
  dynamic precedence. The cap is 64 live versions.
- There is no error recovery: if every version dies, the result is -1.
- Hard limits (each returns -1): advance steps ≤ 64·len + 10000, nodes ≤ 32·len + 100000,
  stack depth ≤ 4M entries, input < 4 GiB. Returns -2 if `cap` is too small.

## Results (GOROOT/src of Go 1.22, 6513 files, 70.3 MB)

Parity is an exact match of the full node arrays (symbol, named, start, end, parent) against C
tree-sitter's cursor walk:

| | files |
|---|---|
| exact match | 6424 / 6464 C-valid files (**99.38%**) |
| mismatch | 39 |
| tsasm -1, C OK | 1 (`crypto/tls/handshake_messages.go`, runs out of GLR versions) |
| C has a syntax error | 49 (tsasm returns -1 on all 49) |

Speed (one thread, best of 3, whole corpus in memory, on a loaded machine, so ±20%):

| | time | throughput |
|---|---|---|
| tsasm parse + emit nodes | 0.95–1.19 s | 59–74 MB/s |
| C model of the same algorithm (asm lexer) | 1.57 s | 45 MB/s |
| C tree-sitter parse only (`-O2`) | 7.4–9.7 s | 7–9 MB/s |
| C tree-sitter parse + cursor walk | 9.2–11 s | 6–8 MB/s |

The lexer alone runs at ~325 MB/s (15 ns/token). Instruction profile (callgrind): reduce 23%,
emit 19%, lexer 20%, advance 16%, main loop 7%, shift 5%, GLR merge/copy/reuse ~4%.

## Known gaps

- **Long ambiguous parameter lists** (the 39 mismatches): for `func f(a, b, …, z int64)` with
  ≥ ~10 names, every name can be read as a type or a parameter name. C's graph-structured stack,
  with its internal limits (`MAX_VERSION_COUNT` 6, `MAX_LINK_COUNT` 8, 64 pop iterators),
  produces a degraded parse where the first name becomes a type-only `parameter_declaration`.
  tsasm keeps the correct single declaration. Matching it exactly would need a faithful GSS port.
- The remaining -1 file needs more than 64 simultaneous linear versions.
- No error recovery, by design (strict): files with syntax errors return -1.
- Not reentrant or thread-safe: there is one static context, and its buffers are reused across
  calls and never freed.
- Grammar-specific in a few places: it assumes no external scanner, ≤ 128 tokens, production
  ids < 256 and alias sequences ≤ 16. `gen.py` asserts the first two.
