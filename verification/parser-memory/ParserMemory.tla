--------------------------- MODULE ParserMemory ---------------------------
(* Formal model of tsasm's buffer lifecycle, memory-limit charging, MAP_MIN
   heap-to-mmap migration, per-parse C_NAVAIL node budget, mid-parse abort
   unwinding (p_limit / p_oom), and inter-parse arena retention (C_RETAIN) in
   src/parser.s.

   Capacities are measured in abstract units where MapMin = 2 (128 KiB in
   src/parser.s:118): 0 is unallocated, 1 is a heap block (< MAP_MIN, allocated
   with realloc and freed with free), and 2 is an anonymous mapping (>= MAP_MIN,
   allocated with mmap/mremap and freed with munmap). Default retain is 2 units
   (32 MiB) and default memory limit is 4 units (1 GiB).

   Every switch constant mirrors one guard in src/parser.s listed in anchors.tsv. *)
EXTENDS Naturals

CONSTANTS
    ChargesBeforeAlloc,          \* p_grow (src/parser.s:211-213): check new usage against C_MEMLIMIT before allocation
    CommitsAfterAlloc,           \* p_grow (src/parser.s:271-273): store ptr, capacity and C_MEMUSED only after allocation succeeds
    MigratesAtMapMin,            \* p_grow / p_free_buf (src/parser.s:225-266, 356-358): cross MAP_MIN via mmap+memcpy+free
    CapsNavailByNodeLim,         \* p_set_navail / p_grow_nodes (src/parser.s:285-286, 300-303): C_NAVAIL = min(C_NCAP, C_NODELIM)
    ReleasesAllOnFailedBigParse, \* .Lmain_ret (src/parser.s:3153-3157): failed parse (rax < 0) over C_RETAIN calls p_release
    ClearsResultOnError,         \* .Lmain_ret (src/parser.s:3164-3167): *nodes = NULL when rax < 0
    ReleasesOnLoweredCap,        \* tsasm_parser_set_memory_limit / set_retain (src/parser.s:3222-3226, 3236-3240): p_release if C_MEMUSED > rsi
    DropsBigOutAfterWorkRelease  \* tsasm_parse (src/parser.s:2984-2987): drop C_OUT at parse entry when p_release_work left C_NCAP = 0 and C_OUTCAP != 0

VARIABLES
    phase,        \* "idle", "parsing", "ret_ok", "ret_err"
    workCap,      \* total capacity of the 9 working buffers (C_NODES..C_SLK) in units (0..2)
    workKind,     \* backing allocator of working buffers: "none", "heap", "mmap"
    ncap,         \* C_NCAP: tree node arena capacity (0..2, <= workCap)
    nn,           \* C_NN: tree units allocated in the current parse (0..3)
    nodeLim,      \* C_NODELIM: per-parse node budget (1..2)
    navail,       \* C_NAVAIL: fast-path bound min(C_NCAP, C_NODELIM) (0..2)
    outCap,       \* C_OUTCAP << 4: output array capacity in units (0..2)
    outKind,      \* backing allocator of C_OUT: "none", "heap", "mmap"
    memUsed,      \* C_MEMUSED: total charged bytes in units (0..5)
    memLimit,     \* C_MEMLIMIT: memory limit in units ({1, 4})
    retain,       \* C_RETAIN: inter-parse retain cap in units ({0, 2})
    resultPtr,    \* caller's *nodes (C_RESULT): "none", "null", "out"
    lastStatus,   \* outcome of last tsasm_parse: "none", "ok", "err"
    workReleased, \* TRUE when .Lmain_ret ran p_release_work on a parse that exceeded C_RETAIN, leaving C_OUT for the caller
    act           \* name of the last action for trace output

vars == <<phase, workCap, workKind, ncap, nn, nodeLim, navail, outCap, outKind,
          memUsed, memLimit, retain, resultPtr, lastStatus, workReleased, act>>

MapMin == 2

Min(a, b) == IF a <= b THEN a ELSE b
Max(a, b) == IF a >= b THEN a ELSE b

\* p_set_navail (src/parser.s:300-304): C_NAVAIL = min(C_NCAP, C_NODELIM)
Navail(c, lim) == IF CapsNavailByNodeLim THEN Min(c, lim) ELSE c

\* Backing allocator kind after growing a buffer from oldKind to newCap (src/parser.s:225-267)
KindAfterGrow(oldKind, newCap) ==
    IF newCap = 0 THEN "none"
    ELSE IF newCap < MapMin THEN "heap"
    ELSE IF MigratesAtMapMin THEN "mmap"
    ELSE IF oldKind = "heap" THEN "heap"
    ELSE "mmap"

\* tsasm_parser_new (src/parser.s:3183-3199)
Init ==
    /\ phase = "idle"
    /\ workCap = 0 /\ workKind = "none" /\ ncap = 0
    /\ nn = 0 /\ nodeLim = 1 /\ navail = 0
    /\ outCap = 0 /\ outKind = "none"
    /\ memUsed = 0 /\ memLimit = 4 /\ retain = 2
    /\ resultPtr = "none" /\ lastStatus = "none"
    /\ workReleased = FALSE
    /\ act = "ParserNew"

\* tsasm_parse entry (src/parser.s:2984-3052) with node budget lim \in {1, 2}
ParseEntry(lim, name) ==
    LET drop == (memUsed > retain) \/ (DropsBigOutAfterWorkRelease /\ workReleased)
        nextNcap == IF drop THEN 0 ELSE ncap
    IN
    /\ phase = "idle" \* src/parser.s:2967
    /\ phase' = "parsing"
    /\ IF drop
       THEN /\ workCap' = 0 /\ workKind' = "none" /\ ncap' = 0
            /\ outCap' = 0 /\ outKind' = "none"
            /\ memUsed' = 0 /\ workReleased' = FALSE
       ELSE UNCHANGED <<workCap, workKind, ncap, outCap, outKind, memUsed, workReleased>>
    /\ nodeLim' = lim
    /\ nn' = 0
    /\ navail' = Navail(nextNcap, lim)
    /\ act' = name
    /\ UNCHANGED <<memLimit, retain, resultPtr, lastStatus>>

ParseStartSmall == TRUE /\ ParseEntry(1, "ParseStartSmall")
ParseStartLarge == TRUE /\ ParseEntry(2, "ParseStartLarge")

\* tsasm_parse input length check: len >= 0xFFFFFFF0 (src/parser.s:3008-3011)
RejectOversizeLen ==
    /\ phase = "parsing" \* src/parser.s:3010
    /\ nn = 0 \* src/parser.s:3010
    /\ phase' = "ret_err"
    /\ act' = "RejectOversizeLen"
    /\ UNCHANGED <<workCap, workKind, ncap, nn, nodeLim, navail, outCap, outKind,
                   memUsed, memLimit, retain, resultPtr, lastStatus, workReleased>>

\* ALLOC_NODE fast path when C_NN + N_UNITS <= C_NAVAIL (src/parser.s:395-401)
AllocNodeFast ==
    /\ phase = "parsing" \* src/parser.s:395
    /\ nn + 1 <= navail \* src/parser.s:397
    /\ nn' = nn + 1
    /\ act' = "AllocNodeFast"
    /\ UNCHANGED <<phase, workCap, workKind, ncap, nodeLim, navail, outCap, outKind,
                   memUsed, memLimit, retain, resultPtr, lastStatus, workReleased>>

\* ALLOC_NODE slow path -> p_grow_nodes (src/parser.s:279-296, 398-401)
GrowNodes ==
    LET req == nn + 1
        newNcap == Max(ncap, Min(req, 2))
        newWork == Max(workCap, newNcap)
        delta == newWork - workCap
    IN
    /\ phase = "parsing" \* src/parser.s:279
    /\ nn < 2 \* src/parser.s:283
    /\ req > navail \* src/parser.s:397
    /\ IF (CapsNavailByNodeLim /\ req > nodeLim) \/ (ChargesBeforeAlloc /\ memUsed + delta > memLimit)
       THEN /\ phase' = "ret_err"
            /\ UNCHANGED <<workCap, workKind, ncap, nn, navail, memUsed>>
       ELSE /\ phase' = "parsing"
            /\ ncap' = newNcap
            /\ workCap' = newWork
            /\ workKind' = KindAfterGrow(workKind, newWork)
            /\ memUsed' = memUsed + delta
            /\ navail' = Navail(newNcap, nodeLim)
            /\ nn' = req
    /\ act' = "GrowNodes"
    /\ UNCHANGED <<nodeLim, outCap, outKind, memLimit, retain, resultPtr, lastStatus, workReleased>>

\* p_grow on working buffers below MAP_MIN via realloc (src/parser.s:196-231)
GrowWorkSmall ==
    /\ phase = "parsing" \* src/parser.s:196
    /\ workCap = 0 \* src/parser.s:199
    /\ IF ChargesBeforeAlloc /\ memUsed + 1 > memLimit
       THEN /\ phase' = "ret_err"
            /\ UNCHANGED <<workCap, workKind, ncap, navail, memUsed>>
       ELSE /\ phase' = "parsing"
            /\ workCap' = 1
            /\ workKind' = "heap"
            /\ ncap' = 1
            /\ navail' = Navail(1, nodeLim)
            /\ memUsed' = memUsed + 1
    /\ act' = "GrowWorkSmall"
    /\ UNCHANGED <<nn, nodeLim, outCap, outKind, memLimit, retain, resultPtr, lastStatus, workReleased>>

\* p_grow on working buffers to >= MAP_MIN via mmap+memcpy+free or mremap (src/parser.s:225-267)
GrowWorkMapMin ==
    LET delta == 2 - workCap
    IN
    /\ phase = "parsing" \* src/parser.s:196
    /\ workCap < 2 \* src/parser.s:203
    /\ IF ChargesBeforeAlloc /\ memUsed + delta > memLimit
       THEN /\ phase' = "ret_err"
            /\ UNCHANGED <<workCap, workKind, ncap, navail, memUsed>>
       ELSE /\ phase' = "parsing"
            /\ workCap' = 2
            /\ workKind' = KindAfterGrow(workKind, 2)
            /\ ncap' = 2
            /\ navail' = Navail(2, nodeLim)
            /\ memUsed' = memUsed + delta
    /\ act' = "GrowWorkMapMin"
    /\ UNCHANGED <<nn, nodeLim, outCap, outKind, memLimit, retain, resultPtr, lastStatus, workReleased>>

\* p_grow allocator failure (realloc/mremap/mmap returns NULL/-1 -> p_oom, src/parser.s:229-250, 337-341)
GrowOOM ==
    LET delta == 2 - workCap
    IN
    /\ phase = "parsing" \* src/parser.s:228
    /\ workCap < 2 \* src/parser.s:203
    /\ (ChargesBeforeAlloc => memUsed + delta <= memLimit) \* src/parser.s:212
    /\ phase' = "ret_err"
    /\ memUsed' = IF CommitsAfterAlloc THEN memUsed ELSE memUsed + delta
    /\ act' = "GrowOOM"
    /\ UNCHANGED <<workCap, workKind, ncap, nn, nodeLim, navail, outCap, outKind,
                   memLimit, retain, resultPtr, lastStatus, workReleased>>

\* p_emit grows C_OUT to need \in {1, 2} and finishes parse (src/parser.s:2812-2818, 2900-2901, 2941-2951)
EmitOut(need, name) ==
    LET newOut == Max(outCap, need)
        delta == newOut - outCap
    IN
    /\ phase = "parsing" \* src/parser.s:3146
    /\ workCap > 0 \* src/parser.s:3027
    /\ IF ChargesBeforeAlloc /\ memUsed + delta > memLimit
       THEN /\ phase' = "ret_err"
            /\ UNCHANGED <<outCap, outKind, memUsed>>
       ELSE /\ phase' = "ret_ok"
            /\ outCap' = newOut
            /\ outKind' = KindAfterGrow(outKind, newOut)
            /\ memUsed' = memUsed + delta
    /\ act' = name
    /\ UNCHANGED <<workCap, workKind, ncap, nn, nodeLim, navail, memLimit, retain, resultPtr, lastStatus, workReleased>>

EmitSmallOut == TRUE /\ EmitOut(1, "EmitSmallOut")
EmitLargeOut == TRUE /\ EmitOut(2, "EmitLargeOut")

\* p_emit grows C_OUT to 1 and then hits p_limit / p_oom on a subsequent .Lem_grow or p_grow_estk (src/parser.s:2817, 2923, 2947)
EmitAbortAfterGrow ==
    LET newOut == Max(outCap, 1)
        delta == newOut - outCap
    IN
    /\ phase = "parsing" \* src/parser.s:3146
    /\ workCap > 0 \* src/parser.s:3027
    /\ (ChargesBeforeAlloc => memUsed + delta <= memLimit) \* src/parser.s:212
    /\ phase' = "ret_err"
    /\ outCap' = newOut
    /\ outKind' = KindAfterGrow(outKind, newOut)
    /\ memUsed' = memUsed + delta
    /\ act' = "EmitAbortAfterGrow"
    /\ UNCHANGED <<workCap, workKind, ncap, nn, nodeLim, navail, memLimit, retain, resultPtr, lastStatus, workReleased>>

\* .Lmain_end when C_FINISHED < 0 -> ERR_SYNTAX (src/parser.s:3141-3145)
SyntaxError ==
    /\ phase = "parsing" \* src/parser.s:3142
    /\ phase' = "ret_err"
    /\ act' = "SyntaxError"
    /\ UNCHANGED <<workCap, workKind, ncap, nn, nodeLim, navail, outCap, outKind,
                   memUsed, memLimit, retain, resultPtr, lastStatus, workReleased>>

\* .Lmain_ret on successful parse (rax >= 0, src/parser.s:3147-3175)
MainRetOk ==
    /\ phase = "ret_ok" \* src/parser.s:3147
    /\ phase' = "idle"
    /\ IF memUsed > retain
       THEN /\ workCap' = 0 /\ workKind' = "none" /\ ncap' = 0
            /\ memUsed' = outCap
            /\ workReleased' = TRUE
       ELSE /\ UNCHANGED <<workCap, workKind, ncap, memUsed>>
            /\ workReleased' = FALSE
    /\ resultPtr' = "out"
    /\ lastStatus' = "ok"
    /\ act' = "MainRetOk"
    /\ UNCHANGED <<nn, nodeLim, navail, outCap, outKind, memLimit, retain>>

\* .Lmain_ret on failed parse (rax < 0: ERR_SYNTAX, ERR_LIMIT, ERR_NOMEM, src/parser.s:3147-3175)
MainRetErr ==
    /\ phase = "ret_err" \* src/parser.s:3147
    /\ phase' = "idle"
    /\ IF memUsed > retain
       THEN IF ReleasesAllOnFailedBigParse
            THEN /\ workCap' = 0 /\ workKind' = "none" /\ ncap' = 0
                 /\ outCap' = 0 /\ outKind' = "none"
                 /\ memUsed' = 0 /\ workReleased' = FALSE
            ELSE /\ workCap' = 0 /\ workKind' = "none" /\ ncap' = 0
                 /\ memUsed' = outCap /\ workReleased' = TRUE
                 /\ UNCHANGED <<outCap, outKind>>
       ELSE UNCHANGED <<workCap, workKind, ncap, outCap, outKind, memUsed, workReleased>>
    /\ resultPtr' = IF ClearsResultOnError THEN "null" ELSE "out"
    /\ lastStatus' = "err"
    /\ act' = "MainRetErr"
    /\ UNCHANGED <<nn, nodeLim, navail, memLimit, retain>>

\* tsasm_parser_set_memory_limit (src/parser.s:3220-3228)
SetLimit(newLim, name) ==
    /\ phase = "idle" \* src/parser.s:3220
    /\ memLimit # newLim \* src/parser.s:3221
    /\ memLimit' = newLim
    /\ IF ReleasesOnLoweredCap /\ memUsed > newLim
       THEN /\ workCap' = 0 /\ workKind' = "none" /\ ncap' = 0
            /\ outCap' = 0 /\ outKind' = "none"
            /\ memUsed' = 0 /\ workReleased' = FALSE
            /\ resultPtr' = "none" /\ lastStatus' = "none"
       ELSE UNCHANGED <<workCap, workKind, ncap, outCap, outKind, memUsed, workReleased, resultPtr, lastStatus>>
    /\ act' = name
    /\ UNCHANGED <<phase, nn, nodeLim, navail, retain>>

SetLimitLow     == TRUE /\ SetLimit(1, "SetLimitLow")
SetLimitDefault == TRUE /\ SetLimit(4, "SetLimitDefault")

\* tsasm_parser_set_retain (src/parser.s:3234-3242)
SetRetain(newRet, name) ==
    /\ phase = "idle" \* src/parser.s:3234
    /\ retain # newRet \* src/parser.s:3235
    /\ retain' = newRet
    /\ IF ReleasesOnLoweredCap /\ memUsed > newRet
       THEN /\ workCap' = 0 /\ workKind' = "none" /\ ncap' = 0
            /\ outCap' = 0 /\ outKind' = "none"
            /\ memUsed' = 0 /\ workReleased' = FALSE
            /\ resultPtr' = "none" /\ lastStatus' = "none"
       ELSE UNCHANGED <<workCap, workKind, ncap, outCap, outKind, memUsed, workReleased, resultPtr, lastStatus>>
    /\ act' = name
    /\ UNCHANGED <<phase, nn, nodeLim, navail, memLimit>>

SetRetainZero    == TRUE /\ SetRetain(0, "SetRetainZero")
SetRetainDefault == TRUE /\ SetRetain(2, "SetRetainDefault")

\* tsasm_parser_free (src/parser.s:3205-3214)
ParserFree ==
    /\ phase = "idle" \* src/parser.s:3205
    /\ memUsed > 0 \* src/parser.s:3210
    /\ workCap' = 0 /\ workKind' = "none" /\ ncap' = 0
    /\ nn' = 0 /\ navail' = 0
    /\ outCap' = 0 /\ outKind' = "none"
    /\ memUsed' = 0 /\ workReleased' = FALSE
    /\ resultPtr' = "none" /\ lastStatus' = "none"
    /\ act' = "ParserFree"
    /\ UNCHANGED <<phase, nodeLim, memLimit, retain>>

Next ==
    \/ ParseStartSmall
    \/ ParseStartLarge
    \/ RejectOversizeLen
    \/ AllocNodeFast
    \/ GrowNodes
    \/ GrowWorkSmall
    \/ GrowWorkMapMin
    \/ GrowOOM
    \/ EmitSmallOut
    \/ EmitLargeOut
    \/ EmitAbortAfterGrow
    \/ SyntaxError
    \/ MainRetOk
    \/ MainRetErr
    \/ SetLimitLow
    \/ SetLimitDefault
    \/ SetRetainZero
    \/ SetRetainDefault
    \/ ParserFree

Spec == Init /\ [][Next]_vars

-------------------------------------------------------------------------------
\* C_MEMUSED always equals the exact sum of the allocated working buffers and output array capacities.
AccountingExact == memUsed = workCap + outCap

\* Total allocated buffer capacity C_MEMUSED never exceeds C_MEMLIMIT.
MemoryWithinLimit == memUsed <= memLimit

\* Every buffer's backing allocator (realloc heap below MAP_MIN, mmap at or above MAP_MIN) matches its capacity when p_free_buf frees it.
AllocatorKindConsistent ==
    /\ (workCap = 0 => workKind = "none")
    /\ (workCap = 1 => workKind = "heap")
    /\ (workCap >= MapMin => workKind = "mmap")
    /\ (outCap = 0 => outKind = "none")
    /\ (outCap = 1 => outKind = "heap")
    /\ (outCap >= MapMin => outKind = "mmap")

\* The tree arena count C_NN and fast-path bound C_NAVAIL never exceed the per-parse node budget C_NODELIM, even with a larger retained C_NODES.
NodeBudgetEnforced == phase = "parsing" => (nn <= nodeLim /\ navail <= nodeLim)

\* After tsasm_parse returns, *nodes points to a live output buffer on success and is NULL on any error.
ResultPointerValid ==
    phase = "idle" =>
        /\ (lastStatus = "ok" => (resultPtr = "out" /\ outCap > 0 /\ outKind # "none"))
        /\ (lastStatus = "err" => resultPtr = "null")

\* Between parses, working buffers never exceed C_RETAIN, and after a failed parse total retained memory is at most C_RETAIN.
RetainCapRespectedOnIdle ==
    phase = "idle" =>
        /\ workCap <= retain
        /\ (lastStatus = "err" => memUsed <= retain)

\* When a big parse (> C_RETAIN) freed its working buffers at .Lmain_ret and kept only C_OUT for the caller, the next tsasm_parse drops that C_OUT at entry.
BigParseOutputDropped == phase = "parsing" => ~workReleased
===============================================================================
