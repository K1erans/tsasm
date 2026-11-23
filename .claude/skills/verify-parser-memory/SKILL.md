---
name: verify-parser-memory
description: "Check a change to tsasm's arena growth, memory-limit accounting, MAP_MIN heap-to-mmap migration, per-parse node budget (C_NAVAIL / C_NODELIM), mid-parse abort unwinding (p_limit / p_oom), or inter-parse buffer retention (C_RETAIN) against the TLA+ formal models. Use when editing p_grow, p_grow_nodes, p_set_navail, p_grow_kids, p_grow_estk, p_limit, p_oom, RELEASE, p_free_buf, p_release_work, p_release, ALLOC_TREE, ALLOC_NODE, p_emit, tsasm_parse (.Lmain_ret), tsasm_parser_new, tsasm_parser_free, tsasm_parser_set_memory_limit, or tsasm_parser_set_retain in src/parser.s, or the retention and memory-limit contracts in tsasm.h."
---

# Verify parser memory, limits, and arena retention

`tsasm` promises deterministic memory caps (`C_MEMLIMIT`, `C_NODELIM`), hybrid `realloc`/`mmap`/`mremap` buffer management across `MAP_MIN` (128 KiB) with zero leaks or mismatched `free`/`munmap` calls on mid-parse aborts (`p_limit`, `p_oom`), and bounded inter-parse buffer retention (`C_RETAIN`). Bugs here depend on sequences of calls across multiple parses (a big parse that exceeds `C_RETAIN`, followed by a small parse, a mid-parse OOM, or lowering a cap), so a TLA+ model checks every order of steps.

Everything lives in `verification/parser-memory/` (with a companion GLR stack and GC model in `verification/glr-stack/`):

- `ParserMemory.tla` is the TLA+ model of `TsasmParser`'s buffer lifecycle, `MAP_MIN` migration, `C_NAVAIL` node-budget clamping, mid-parse unwinding, and `C_RETAIN` retention across consecutive parses. Each action is one indivisible step in `src/parser.s`.
- `anchors.tsv` lists the exact lines in `src/parser.s` and `tsasm.h` that each guard and transition in `ParserMemory.tla` mirrors.
- `expected.tsv` records the result on the code as it stands (`PASS` for all seven invariants).
- `check.sh` verifies every anchor line, checks `synced.sha256` for drift, runs TLC (TLA+ tools 1.7.4, SHA-256 verified, downloaded into `verification/parser-memory/.tools/tla2tools.jar` on first run) on every invariant, and with `--mutants` flips each guard switch to `FALSE` to prove every invariant fails on a broken variant.
- `repro.c` is a standalone C regression test for the `BigParseOutputDropped` trace against `libtsasm.a`.

## The properties

| Property | In plain words | Harm if broken |
| --- | --- | --- |
| `AccountingExact` | `C_MEMUSED` always equals the exact sum of the allocated capacities of the working buffers and the output array (`C_OUT`). | Memory-limit checks under- or over-count usage, either letting a parse exceed `C_MEMLIMIT` or permanently bricking a parser into returning `TSASM_ERR_LIMIT` after an OOM or buffer release. |
| `MemoryWithinLimit` | Total allocated buffer capacity `C_MEMUSED` never exceeds `C_MEMLIMIT`. | A parser allocates past the caller's `tsasm_parser_set_memory_limit` cap or holds buffers above a newly lowered limit. |
| `AllocatorKindConsistent` | Every buffer's backing allocator (`realloc` heap below `MAP_MIN`, `mmap` at or above `MAP_MIN`) matches its recorded capacity in bytes when `p_free_buf` frees it. | `p_free_buf` (`cmp rsi, MAP_MIN; jae munmap; jmp free`) calls `munmap` on a heap pointer or `free` on an `mmap` pointer, corrupting the heap or crashing in `libc`. |
| `NodeBudgetEnforced` | During a parse, the tree arena high-water mark `C_NN` and fast-path bound `C_NAVAIL` never exceed the per-parse node budget `C_NODELIM`, even when a larger `C_NODES` buffer is retained from an earlier parse. | A small input parsed on a warm parser with a large retained `C_NODES` arena bypasses the `32 * len + 100,000` node budget. |
| `ResultPointerValid` | After `tsasm_parse` returns, `*nodes` points to a live output buffer on success and is `NULL` on any error (`TSASM_ERR_SYNTAX`, `TSASM_ERR_LIMIT`, or `TSASM_ERR_NOMEM`). | The caller dereferences a dangling or stale `*nodes` pointer after a failed parse, or loses the returned node array after `.Lmain_ret` cleanup. |
| `RetainCapRespectedOnIdle` | Between parses, working buffers never exceed `C_RETAIN`, and after any failed parse total retained memory `C_MEMUSED` is at most `C_RETAIN`. | A parser that aborts mid-parse on a huge input keeps hundreds of megabytes of dead working or partial output buffers pinned between parses. |
| `BigParseOutputDropped` | When a big parse (`C_MEMUSED > C_RETAIN`) frees its working buffers at `.Lmain_ret` and keeps only `C_OUT` for the caller, the next `tsasm_parse` drops that `C_OUT` at entry (`tsasm.h:31-33`, `src/parser.s:2984`). | Whenever `work + out > C_RETAIN` but `0 < out <= C_RETAIN`, `C_OUT` is never freed by subsequent smaller parses, and any subsequent parse needing `work > C_RETAIN - out` thrashes `mmap`/`munmap` on all 9 working buffers on every call. |

## Findings so far

All seven properties hold on the current code (`PASS` in `verification/parser-memory/expected.tsv`), and all eight mutant runs fail (`check.sh --mutants`).

Two bugs were found and fixed; their guards are now boolean switches in `ParserMemory.tla` so removing either one fails `check.sh --mutants`:

- `BigParseOutputDropped` (found and fixed 2026-11-23, guarded by `DropsBigOutAfterWorkRelease`). `tsasm.h:31-33` states: *"A parse that needed more [than `C_RETAIN`] frees its working buffers before it returns, keeping only the node array it returns, and the next parse frees that too."* At `.Lmain_ret` (`src/parser.s:3148-3156`), when a successful parse has `C_MEMUSED > C_RETAIN`, `p_release_work` (`src/parser.s:361-376`) frees all 9 working buffers (`C_NODES` through `C_SLK`, setting `C_NCAP = 0`) and sets `C_MEMUSED = C_OUTCAP << 4`. Previously, `tsasm_parse` entry (`src/parser.s:2984-2989`, comment `# drop the last parse's big output array`) checked `cmp qword ptr [r15+C_MEMUSED], rax` (`C_MEMUSED > C_RETAIN`). Whenever Parse 1 had `work + out > C_RETAIN` while `0 < out <= C_RETAIN` (e.g., 48 MiB working buffers + 16 MiB `C_OUT` under the default 32 MiB retain cap), `p_release_work` lowered `C_MEMUSED` to 16 MiB `<= 32 MiB`, so Parse 2 skipped `p_release`, leaving Parse 1's 16 MiB `C_OUT` pinned and causing subsequent medium parses (`work > C_RETAIN - out`) to thrash `mmap`/`munmap` on all 9 working buffers on every call. `tsasm_parse` entry now checks `C_NCAP == 0 && C_OUTCAP != 0` (`src/parser.s:2984-2987`), which detects that the previous parse ran `p_release_work` and drops its `C_OUT` array. Regression test: `verification/parser-memory/repro.c`.
- `NodeBudgetEnforced` (found 2026-11-22 in `decisions.tsv` row 34, guarded by `CapsNavailByNodeLim`). Before `p_set_navail` (`C_NAVAIL = min(C_NCAP, C_NODELIM)` at `src/parser.s:300-303`), `C_NODELIM` was checked only when `C_NODES` grew, so a warm parser with a large retained `C_NODES` arena could allocate past `C_NODELIM`. Turning off `CapsNavailByNodeLim` in `check.sh --mutants` breaks `NodeBudgetEnforced`.

## Run it

```sh
verification/parser-memory/check.sh                # ParserMemory.tla (~3 seconds)
verification/parser-memory/check.sh --mutants      # also proves every property fails on a mutant and passes with the fix
verification/parser-memory/check.sh --all --mutants # runs both ParserMemory.tla and GlrStack.tla with all mutants
```

Exit 0 means every property matches `expected.tsv` (and with `--mutants`, every mutant fails as expected). Read the output like this:

- `OUT OF SYNC` (exit 2): an assembly or header line mirrored in `anchors.tsv` changed or moved. Go to "When the code changes".
- `NOTE covered files changed`: the anchors still match, but `src/parser.s` or `tsasm.h` was edited since `synced.sha256` was recorded. Read the diff for new buffer allocations, new unwind paths, or changes to `.Lmain_ret` / `p_release_work` / `p_release`, then run `verification/parser-memory/check.sh --confirm --all`.
- `CHANGED` (exit 1): a property's result differs from `expected.tsv`. The trace printed beneath it is the shortest sequence of steps that violates the invariant.
- `KNOWN FINDING`: a property recorded as `FAIL` in `expected.tsv` (`BigParseOutputDropped` today), with its counterexample trace.

Read every result with the `/statechart` skill. It shows each invariant violation the checker finds as its shortest path in plain words, and when the check passes it draws a chart of how the system moves between states.

## Real bug or modeling error?

Take the printed trace and walk it through `src/parser.s`, one instruction block per action, using `anchors.tsv` to jump to each label. If every step in the trace can execute in `src/parser.s`, it is a real bug: reproduce it with a small C test linked against `libtsasm.a`, report the counterexample in the code's terms, and do not modify `src/parser.s` unless asked. If a step cannot happen because an assembly check branches away first, the model is missing that guard: add the guard to `ParserMemory.tla`, add its instruction line to `anchors.tsv`, and rerun.

## When the code changes

1. Update `ParserMemory.tla` (or `GlrStack.tla`) to match what `src/parser.s` now does. If `src/parser.s:2984-2989` is fixed to drop `C_OUT` after a parse that ran `p_release_work`, set `dropbig=TRUE` in `verification/parser-memory/check.sh` (and `DropsBigOutAfterWorkRelease = TRUE` in `ParserMemory.cfg`), and change `BigParseOutputDropped` from `FAIL` to `PASS` in `verification/parser-memory/expected.tsv`.
2. Update `anchors.tsv` so every mirrored line has its exact text and count in `src/parser.s` and `tsasm.h`.
3. Run `verification/parser-memory/check.sh --all --mutants` (and `make check-gopkg` if `src/parser.s` changed).
4. Only after reading `src/parser.s` and `tsasm.h` side by side with the models, run `verification/parser-memory/check.sh --confirm --all` and update the commit hash below.

## Coverage and sync

Covered in `src/parser.s` and `tsasm.h`:
- `p_grow` (`src/parser.s:196-275`): capacity doubling, 32-bit overflow check (`jc p_limit`), `C_MEMLIMIT` charging before allocation, `MAP_MIN` (128 KiB) threshold branching (`realloc` below `MAP_MIN`, `mremap` when already `>= MAP_MIN`, `mmap` + `madvise(MADV_HUGEPAGE)` + `memcpy` + `free` when crossing `MAP_MIN`), and committing pointer, capacity, and `C_MEMUSED` only at label `5:` after allocation succeeds.
- `p_grow_nodes` and `p_set_navail` (`src/parser.s:279-304`), `ALLOC_TREE` / `ALLOC_NODE` (`src/parser.s:394-405`): `C_NODELIM` enforcement and `C_NAVAIL = min(C_NCAP, C_NODELIM)`.
- `p_limit` and `p_oom` (`src/parser.s:334-341`): mid-parse stack unwind via `C_SAVEDRSP` to `.Lmain_ret`.
- `RELEASE`, `p_free_buf`, `p_release_work`, and `p_release` (`src/parser.s:345-385`): `MAP_MIN`-matched `munmap` vs `free`, working-buffer release (`C_MEMUSED = C_OUTCAP << 4`), and full release (`C_MEMUSED = 0`).
- `p_emit` (`src/parser.s:2806-2958`): initial and incremental `C_OUT` growth (`.Lem_grow`) and `C_ESTK` growth.
- `tsasm_parse` entry and `.Lmain_ret` (`src/parser.s:2967-3025, 3141-3175`): entry `C_RETAIN` check, 4 GiB length rejection, `C_NODELIM` and `C_NAVAIL` initialization, `.Lmain_ret` `p_release_work` vs `p_release` dispatch, and `*nodes` (`C_RESULT`) assignment.
- `tsasm_parser_new`, `tsasm_parser_free`, `tsasm_parser_set_memory_limit`, and `tsasm_parser_set_retain` (`src/parser.s:3183-3243`, `tsasm.h:23-46`).

Last confirmed in sync: 2026-11-23, at commit `5158821` (`5158821c658bdff9a07301a31a21f37220349258`), recorded in `verification/parser-memory/synced.sha256` and `verification/glr-stack/synced.sha256`.

## What this does not cover

- The 9 working buffers (`C_NODES`, `C_KIDS`, `C_ESTK`, `C_SN`, `C_XB`, `C_HEADS`, `C_SLICES`, `C_CELLS`, `C_SLK`) are abstracted as a composite working capacity `workCap \in 0..2` plus `ncap \in 0..2` for `C_NODES`, with `MapMin = 2`, `Retain = 2`, and `Limit = 4`. All 9 working buffers use the exact same `p_grow` and `RELEASE` / `p_free_buf` code paths.
- 64-bit arithmetic overflow in `C_MEMUSED + delta` is not modelled because `edx` in `p_grow` is a 32-bit unsigned entry count (`jc p_limit` on doubling overflow) and entry sizes are at most 64 bytes (`XB_SIZE`), so `delta <= 2^38` bytes.
- Lexer state machines and parse-table generation in `gen/gen.py`, which are verified by `scripts/parity.sh` and `make fuzz-regress`.

## The GLR stack and mark-and-sweep GC have their own model

`verification/glr-stack/GlrStack.tla` models the graph-structured stack, fast-path single-link in-place reductions (`.Lad_inplace` and `p_reduce_fast`), general/fork reductions across merged DAG nodes (`p_reduce_gss`), head merging and extension-block link attachment (`p_merge`, `p_add_link`), version condensation (`.Lmain_condense`), and stack-node / extension-block mark-and-sweep garbage collection (`p_snode_slow`, `p_gc`) in `src/parser.s:558-2327, 2587-2696, 3086-3139`.

| Property | In plain words | Harm if broken |
| --- | --- | --- |
| `BaseNodeProtected` | Stack node 0 (the base node) is never placed on `C_SFREE` and always has 0 outgoing links. | Allocating node 0 from `C_SFREE` overwrites the base of the parse stack and creates cycles in every version's root chain. |
| `LiveNodesNeverFreed` | No stack node reachable from any version head in `C_HEADS[0..C_NH-1]` (through `SN_L0` or an extension block `SN_EXT`) is ever on the free list `C_SFREE`. | Use-after-free on a live GLR branch when a subsequent shift or push overwrites a stack node still referenced by another version or an extension link. |
| `NoExtensionBlockLeak` | Every free stack node on `C_SFREE` has returned any attached `C_XB` extension block to `C_XFREE` and has `SN_NL <= 1`. | Extension blocks leak or get double-freed on later GC cycles when multi-link nodes are recycled. |
| `MarksClearedAfterGC` | After `p_gc` finishes, every stack node in `0..C_SNN-1` has `SN_MARK = 0`. | A surviving node with a stale `SN_MARK = 1` stops `.Lgc_mark` on the next GC cycle from traversing its predecessors, sweeping live ancestors onto `C_SFREE`. |
| `NoSelfLoopLinks` | No stack node ever has an inline or extension link pointing to itself (`p_add_link` guards `r13d != r12d`). | A cycle in the GSS causes `p_pop` and `p_gc` to walk cyclic paths until the work budget or `C_ESTK` exhausts. |
| `CondenseCleanHeads` | After `.Lmain_condense` or `p_merge` completes, no dead version (`H_DEAD != 0`) remains in `C_HEADS[0..C_NH-1]`. | Dead versions occupy slots against the `MAXVERSIONS` (6) cap and can merge dead links into live versions. |

```sh
verification/glr-stack/check.sh --mutants   # ~4 seconds; all 6 properties hold and all 9 mutants fail
```
