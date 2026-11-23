---
name: verify-glr-stack
description: "Check a change to tsasm's graph-structured stack (GSS), single-link in-place reductions (.Lad_inplace, p_reduce_fast), general iterator slice reductions (p_pop, p_reduce_gss), version merging and recursive link attachment (p_merge, p_add_link), version condensation (.Lmain_condense), or stack-node / extension-block mark-and-sweep GC (p_snode_slow, p_gc) in src/parser.s against the GlrStack.tla formal model."
---

# Verify GLR graph-structured stack and mark-and-sweep GC

`tsasm` recycles 24-byte stack nodes (`C_SN`) and 64-byte extension blocks (`C_XB`) in two ways: eager in-place recycling during single-version single-link reductions (`.Lad_inplace` and `p_reduce_fast`), and mark-and-sweep garbage collection (`p_gc`) when `C_SFREE` is empty and `C_SNN == C_SNCAP`. If an in-place reduction recycles a node still reachable by another GLR version or holding an extension block, or if `p_gc` misses an extension link or fails to clear `SN_MARK`, a live parse branch is corrupted in place. `verification/glr-stack/GlrStack.tla` model-checks every combination of shifts, GLR forks, head merges, fast-path and general reductions, dead-version condensation, and GC sweeps.

Everything lives in `verification/glr-stack/`:

- `GlrStack.tla` is the TLA+ model. Each action is one stack operation in `src/parser.s`.
- `anchors.tsv` lists the exact instructions in `src/parser.s` that each guard in `GlrStack.tla` mirrors.
- `expected.tsv` records the expected outcome (`PASS` for all six invariants on the current code).
- `check.sh` checks every anchor line, checks `synced.sha256` for drift, runs TLC on every invariant, and with `--mutants` flips each guard constant to `FALSE` to prove every invariant fails on a broken variant.

## The properties

| Property | In plain words | Harm if broken |
| --- | --- | --- |
| `BaseNodeProtected` | Stack node 0 (the base node) is never placed on `C_SFREE` and always has 0 outgoing links. | Allocating node 0 from `C_SFREE` overwrites the base of the parse stack and creates cycles in every version's root chain. |
| `LiveNodesNeverFreed` | No stack node reachable from any version head in `C_HEADS[0..C_NH-1]` (through `SN_L0` or an extension block `SN_EXT`) is ever on the free list `C_SFREE`. | Use-after-free on a live GLR branch when a subsequent shift or push overwrites a stack node still referenced by another version or an extension link. |
| `NoExtensionBlockLeak` | Every free stack node on `C_SFREE` has returned any attached `C_XB` extension block to `C_XFREE` and has `SN_NL <= 1`. | Extension blocks leak or get double-freed on later GC cycles when multi-link nodes are recycled. |
| `MarksClearedAfterGC` | After `p_gc` finishes, every stack node in `0..C_SNN-1` has `SN_MARK = 0`. | A surviving node with a stale `SN_MARK = 1` stops `.Lgc_mark` on the next GC cycle from traversing its predecessors, sweeping live ancestors onto `C_SFREE`. |
| `NoSelfLoopLinks` | No stack node ever has an inline or extension link pointing to itself (`p_add_link` guards `r13d != r12d`). | A cycle in the GSS causes `p_pop` and `p_gc` to walk cyclic paths until the work budget or `C_ESTK` exhausts. |
| `CondenseCleanHeads` | After `.Lmain_condense` or `p_merge` completes, no dead version (`H_DEAD != 0`) remains in `C_HEADS[0..C_NH-1]`. | Dead versions occupy slots against the `MAXVERSIONS` (6) cap and can merge dead links into live versions. |

## Findings so far

All six properties hold on the current code (`PASS` in `verification/glr-stack/expected.tsv`), and all nine mutant runs fail (`check.sh --mutants`). For the buffer-retention and memory-limit verification suite, see `.claude/skills/verify-parser-memory/SKILL.md` (`verification/parser-memory/`).

## Run it

```sh
verification/glr-stack/check.sh                # GlrStack.tla (~2 seconds)
verification/glr-stack/check.sh --mutants      # also proves every property fails on a broken variant (~4 seconds)
verification/parser-memory/check.sh --all --mutants # runs both ParserMemory.tla and GlrStack.tla
```

Read every result with the `/statechart` skill. It shows each invariant violation the checker finds as its shortest path in plain words, and when the check passes it draws a chart of how the system moves between states.

## Real bug or modeling error?

Walk any counterexample trace step by step through `src/parser.s` using `anchors.tsv`. If every step can execute in `src/parser.s`, it is a real bug: reproduce it in C against `libtsasm.a` and report it without modifying `src/parser.s` unless asked. If an assembly branch prevents a step, add that guard to `GlrStack.tla` and `anchors.tsv` and rerun.

## When the code changes

1. Update `GlrStack.tla` and `anchors.tsv` to reflect any changed instruction or guard in `src/parser.s`.
2. Run `verification/glr-stack/check.sh --mutants`.
3. After reading `src/parser.s` side by side with `GlrStack.tla`, run `verification/glr-stack/check.sh --confirm` and update the commit hash below.

## Coverage and sync

Covered in `src/parser.s`:
- `p_snode_slow` and `p_gc` (`.Lgc_root`, `.Lgc_drain`, `.Lgc_sweep`, `.Lgc_mark`, `src/parser.s:559-699`)
- `p_ext_new`, `p_link_addr`, `p_push`, `SHIFT_LEAF`, `p_shift` (`src/parser.s:702-855`)
- `p_reduce_fast` (`.Lrf_general`, `.Lrf_quick`, `.Lrf_fork`, `.Lrf_place`, `src/parser.s:878-1400`) and `.Lad_inplace` (`src/parser.s:2587-2696`)
- `p_add_version`, `p_remove_version`, `p_renumber`, `p_add_link`, `p_merge`, `p_reduce_gss` (`src/parser.s:1622-2327`)
- `.Lmain_condense` (`src/parser.s:3086-3139`)

Last confirmed in sync: 2026-11-23, at commit `5158821` (`5158821c658bdff9a07301a31a21f37220349258`), recorded in `verification/glr-stack/synced.sha256`.

## What this does not cover

- Bounds: `Nodes = 0..3` (base node 0 plus up to 3 allocated stack nodes), `nh \in 1..2` active versions, and up to 2 outgoing links per node (`SN_L0` plus one extension link in `SN_EXT`). In `src/parser.s`, `MAXLINKS = 8`, `MAXVERSIONS = 6`, and `MAXSLICEVER = 10`.
- Dynamic precedence arithmetic (`ADD_DYN`, `MOV_DYN`, `p_select`, `p_subtree_compare`), which determines which parse tree wins an ambiguity rather than stack memory safety, and is checked against C tree-sitter across 17,059 corpus files by `scripts/parity.sh`.
