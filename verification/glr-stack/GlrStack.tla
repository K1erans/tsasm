------------------------------ MODULE GlrStack ------------------------------
(* Formal model of tsasm's graph-structured stack (GSS), single-link in-place
   reductions (.Lad_inplace and p_reduce_fast), general/fork reductions across
   merged nodes (p_reduce_gss), version merging and recursive link attachment
   (p_merge, p_add_link), version condensation (.Lmain_condense), and
   stack-node / extension-block mark-and-sweep garbage collection
   (p_snode_slow, p_gc) in src/parser.s.

   Stack node 0 is the base node created at the start of tsasm_parse
   (src/parser.s:3040: state 1, 0 links, unmarked). Nodes 1..MaxNodes live in
   the C_SN arena. Each allocated stack node has link count nl \in 0..2:
   link 0 is stored inline at SN_L0 (src/parser.s:129), and link 1 lives in an
   extension block from C_XB attached at SN_EXT (src/parser.s:132, 1901-1907).
   Free stack nodes are linked through SN_L0 in C_SFREE (src/parser.s:55), and
   free extension blocks through XB_NEXT in C_XFREE (src/parser.s:56).

   Every switch constant mirrors one guard in src/parser.s listed in anchors.tsv. *)
EXTENDS Naturals, FiniteSets

CONSTANTS
    InPlaceRequiresSingleVersion, \* .Lad_inplace / p_reduce_fast (src/parser.s:890-891, 2661): in-place node recycling requires C_NH = 1
    InPlaceRequiresSingleLink,    \* .Lad_inplace / p_reduce_fast (src/parser.s:917-918, 2607-2608): in-place reduction requires SN_NL = 1
    GCMarksBase,                  \* p_gc (src/parser.s:594, 635-636): mark base node 0 and stop sweep at index 1
    GCMarksExtLinks,              \* p_gc (src/parser.s:619-626): mark predecessors in extension block SN_EXT (links 1..SN_NL-1)
    GCSweepsExtBlocks,            \* .Lgc_sweep (src/parser.s:639-640, 649-656): recycle SN_EXT to C_XFREE and set SN_NL = 0 on unmarked nodes
    GCClearsMarks,                \* .Lgc_sweep (src/parser.s:646, 657): clear SN_MARK on all surviving nodes and node 0
    AddLinkRejectsSelf,           \* p_add_link (src/parser.s:1808-1809): cmp r13d, r12d; je .Lal_ret prevents self-loop links
    CondenseRemovesDead,          \* .Lmain_condense (src/parser.s:3094-3098): remove versions with H_DEAD != 0
    MergeChecksLive               \* p_merge (src/parser.s:1966-1969): refuse to merge a dead version with a live version

VARIABLES
    snn,          \* C_SNN: stack node arena high-water mark (1..4)
    nh,           \* C_NH: number of version heads (1..2)
    h1Node,       \* C_HEADS[0].H_NODE: stack node of version 0 (0..3)
    h1Dead,       \* C_HEADS[0].H_DEAD: whether version 0 is dead
    h2Node,       \* C_HEADS[1].H_NODE: stack node of version 1 when nh = 2 (0..3)
    h2Dead,       \* C_HEADS[1].H_DEAD: whether version 1 is dead
    nl,           \* function 0..3 -> 0..2: SN_NL link count of each stack node
    l0,           \* function 0..3 -> 0..3: SN_L0 primary predecessor of each stack node
    l1,           \* function 0..3 -> 0..3: extension block link 1 predecessor (SN_EXT in C_XB)
    mark,         \* function 0..3 -> BOOLEAN: SN_MARK byte after step completes
    sfree,        \* subset of 0..3: nodes currently on the C_SFREE free list
    extOwned,     \* subset of 1..3: nodes currently owning an allocated C_XB extension block
    afterCondense,\* TRUE immediately after .Lmain_condense or p_merge completes
    act           \* name of the last action for trace output

vars == <<snn, nh, h1Node, h1Dead, h2Node, h2Dead, nl, l0, l1, mark,
          sfree, extOwned, afterCondense, act>>

Nodes == 0..3
AllocNodes == 1..3

\* Nodes directly linked from the set S via SN_L0 (and SN_EXT when followExt is TRUE).
StepLinks(S, followExt) ==
    { l0[n] : n \in { x \in S : nl[x] >= 1 } }
    \cup (IF followExt THEN { l1[n] : n \in { x \in S : nl[x] = 2 } } ELSE {})

\* Transitive closure of reachable stack nodes from roots R (up to depth 3).
ReachFrom(R, followExt) ==
    LET S1 == R \cup StepLinks(R, followExt)
        S2 == S1 \cup StepLinks(S1, followExt)
        S3 == S2 \cup StepLinks(S2, followExt)
    IN S3

\* All stack nodes currently reachable from the version heads C_HEADS[0..C_NH-1] (src/parser.s:597-627).
HeadRoots == IF nh = 1 THEN {h1Node} ELSE {h1Node, h2Node}
Reachable == ReachFrom(HeadRoots, TRUE)

\* Smallest node index in a non-empty subset S of Nodes.
PickMin(S) == CHOOSE x \in S : \A y \in S : x <= y

\* tsasm_parse initial stack setup: node 0 is base, version 0 on node 0 (src/parser.s:3021-3051).
Init ==
    /\ snn = 1
    /\ nh = 1
    /\ h1Node = 0 /\ h1Dead = FALSE
    /\ h2Node = 0 /\ h2Dead = FALSE
    /\ nl = [n \in Nodes |-> 0]
    /\ l0 = [n \in Nodes |-> 0]
    /\ l1 = [n \in Nodes |-> 0]
    /\ mark = [n \in Nodes |-> FALSE]
    /\ sfree = {}
    /\ extOwned = {}
    /\ afterCondense = TRUE
    /\ act = "InitStack"

\* p_push / SHIFT_LEAF on version 0 using C_SFREE or high-water mark C_SNN (src/parser.s:560-567, 752-786).
PushV1 ==
    LET useFree == sfree # {}
        n == IF useFree THEN PickMin(sfree) ELSE snn
    IN
    /\ ~h1Dead \* src/parser.s:3062
    /\ (useFree \/ snn <= 3) \* src/parser.s:564
    /\ h1Node' = n
    /\ snn' = IF useFree THEN snn ELSE snn + 1
    /\ sfree' = IF useFree THEN sfree \ {n} ELSE sfree
    /\ nl' = [nl EXCEPT ![n] = 1]
    /\ l0' = [l0 EXCEPT ![n] = h1Node]
    /\ l1' = [l1 EXCEPT ![n] = 0]
    /\ mark' = [mark EXCEPT ![n] = FALSE]
    /\ extOwned' = extOwned \ {n}
    /\ afterCondense' = FALSE
    /\ act' = "PushV1"
    /\ UNCHANGED <<nh, h1Dead, h2Node, h2Dead>>

\* GLR split: p_add_version forks version 1 from base node 0 and pushes a distinct node (src/parser.s:1622-1640, 2716-2756).
ForkV2 ==
    LET useFree == sfree # {}
        n == IF useFree THEN PickMin(sfree) ELSE snn
    IN
    /\ nh = 1 \* src/parser.s:1623
    /\ ~h1Dead \* src/parser.s:3062
    /\ h1Node # 0 \* src/parser.s:2718
    /\ (useFree \/ snn <= 3) \* src/parser.s:564
    /\ nh' = 2
    /\ h2Node' = n /\ h2Dead' = FALSE
    /\ snn' = IF useFree THEN snn ELSE snn + 1
    /\ sfree' = IF useFree THEN sfree \ {n} ELSE sfree
    /\ nl' = [nl EXCEPT ![n] = 1]
    /\ l0' = [l0 EXCEPT ![n] = 0]
    /\ l1' = [l1 EXCEPT ![n] = 0]
    /\ mark' = [mark EXCEPT ![n] = FALSE]
    /\ extOwned' = extOwned \ {n}
    /\ afterCondense' = FALSE
    /\ act' = "ForkV2"
    /\ UNCHANGED <<h1Node, h1Dead>>

\* p_push / SHIFT_LEAF on version 1 so version 1 has its own intermediate predecessor before merge (src/parser.s:752-786).
PushV2 ==
    LET useFree == sfree # {}
        n == IF useFree THEN PickMin(sfree) ELSE snn
    IN
    /\ nh = 2 \* src/parser.s:3058
    /\ ~h2Dead \* src/parser.s:3062
    /\ (useFree \/ snn <= 3) \* src/parser.s:564
    /\ h2Node' = n
    /\ snn' = IF useFree THEN snn ELSE snn + 1
    /\ sfree' = IF useFree THEN sfree \ {n} ELSE sfree
    /\ nl' = [nl EXCEPT ![n] = 1]
    /\ l0' = [l0 EXCEPT ![n] = h2Node]
    /\ l1' = [l1 EXCEPT ![n] = 0]
    /\ mark' = [mark EXCEPT ![n] = FALSE]
    /\ extOwned' = extOwned \ {n}
    /\ afterCondense' = FALSE
    /\ act' = "PushV2"
    /\ UNCHANGED <<nh, h1Node, h1Dead, h2Dead>>

\* p_merge + p_add_link: version 1's head links join version 0's head via an extension block in C_XB,
\* leaving version 1's old head node unreachable for p_gc while l0[h2Node] stays reachable ONLY via l1[h1Node]
\* (src/parser.s:1794-2007, 3117-3120).
MergeV2IntoV1 ==
    LET target == l0[h2Node]
    IN
    /\ nh = 2 \* src/parser.s:3089
    /\ h1Node # 0 \* src/parser.s:1970
    /\ h2Node # 0 \* src/parser.s:1971
    /\ h1Node # h2Node \* src/parser.s:1829
    /\ nl[h1Node] = 1 \* src/parser.s:1901
    /\ nl[h2Node] = 1 \* src/parser.s:1987
    /\ (MergeChecksLive => (~h1Dead /\ ~h2Dead)) \* src/parser.s:1966
    /\ (AddLinkRejectsSelf => target # h1Node) \* src/parser.s:1808
    /\ nh' = 1
    /\ h2Node' = 0 /\ h2Dead' = FALSE
    /\ nl' = [nl EXCEPT ![h1Node] = 2]
    /\ l1' = [l1 EXCEPT ![h1Node] = target]
    /\ extOwned' = extOwned \cup {h1Node}
    /\ afterCondense' = TRUE
    /\ act' = "MergeV2IntoV1"
    /\ UNCHANGED <<snn, h1Node, h1Dead, l0, mark, sfree>>

\* p_add_link attempted with link.node == self (src/parser.s:1808-1809: cmp r13d, r12d; je .Lal_ret).
TryAddSelfLink ==
    /\ ~h1Dead \* src/parser.s:3062
    /\ h1Node # 0 \* src/parser.s:1808
    /\ nl[h1Node] = 1 \* src/parser.s:1901
    /\ IF AddLinkRejectsSelf
       THEN UNCHANGED <<nl, l1, extOwned>>
       ELSE /\ nl' = [nl EXCEPT ![h1Node] = 2]
            /\ l1' = [l1 EXCEPT ![h1Node] = h1Node]
            /\ extOwned' = extOwned \cup {h1Node}
    /\ afterCondense' = FALSE
    /\ act' = "TryAddSelfLink"
    /\ UNCHANGED <<snn, nh, h1Node, h1Dead, h2Node, h2Dead, l0, mark, sfree>>

\* Single-link in-place reduction (.Lad_inplace / p_reduce_fast, src/parser.s:890-893, 973-976, 991-997, 2607-2678).
\* Pops h1Node, updates h1Node to its predecessor, and recycles popped node directly onto C_SFREE.
ReduceInPlace ==
    LET popped == h1Node
        base == l0[popped]
    IN
    /\ ~h1Dead \* src/parser.s:3062
    /\ popped # 0 \* src/parser.s:2603
    /\ (InPlaceRequiresSingleVersion => nh = 1) \* src/parser.s:2661
    /\ (InPlaceRequiresSingleLink => nl[popped] = 1) \* src/parser.s:2607
    /\ h1Node' = base
    /\ sfree' = sfree \cup {popped}
    /\ afterCondense' = FALSE
    /\ act' = "ReduceInPlace"
    /\ UNCHANGED <<snn, nh, h1Dead, h2Node, h2Dead, nl, l0, l1, mark, extOwned>>

\* General GSS reduction across a multi-link merged node (p_reduce_gss, src/parser.s:2163-2327).
\* Unlike ReduceInPlace, p_reduce_gss does NOT put popped nodes onto C_SFREE directly; p_gc collects them later.
ReduceMergedGSS ==
    /\ nh = 1 \* src/parser.s:2172
    /\ ~h1Dead \* src/parser.s:3062
    /\ h1Node # 0 \* src/parser.s:1420
    /\ nl[h1Node] = 2 \* src/parser.s:1441
    /\ h1Node' = 0
    /\ afterCondense' = FALSE
    /\ act' = "ReduceMergedGSS"
    /\ UNCHANGED <<snn, nh, h1Dead, h2Node, h2Dead, nl, l0, l1, mark, sfree, extOwned>>

\* Version 1 shares version 0's head node before a fast reduction (src/parser.s:1635).
ShareHeadWithV2 ==
    /\ nh = 1 \* src/parser.s:1623
    /\ ~h1Dead \* src/parser.s:3062
    /\ h1Node # 0 \* src/parser.s:1635
    /\ nh' = 2
    /\ h2Node' = h1Node /\ h2Dead' = FALSE
    /\ afterCondense' = FALSE
    /\ act' = "ShareHeadWithV2"
    /\ UNCHANGED <<snn, h1Node, h1Dead, nl, l0, l1, mark, sfree, extOwned>>

\* .Lad_dead marks version 0 dead when no action applies while version 1 is still live (src/parser.s:2798-2799).
MarkV1Dead ==
    /\ nh = 2 \* src/parser.s:3058
    /\ ~h1Dead \* src/parser.s:3062
    /\ ~h2Dead \* src/parser.s:3062
    /\ h1Dead' = TRUE
    /\ afterCondense' = FALSE
    /\ act' = "MarkV1Dead"
    /\ UNCHANGED <<snn, nh, h1Node, h2Node, h2Dead, nl, l0, l1, mark, sfree, extOwned>>

\* .Lad_dead marks version 1 dead when no action applies while version 0 is still live (src/parser.s:2798-2799).
MarkV2Dead ==
    /\ nh = 2 \* src/parser.s:3058
    /\ ~h1Dead \* src/parser.s:3062
    /\ ~h2Dead \* src/parser.s:3062
    /\ h2Dead' = TRUE
    /\ afterCondense' = FALSE
    /\ act' = "MarkV2Dead"
    /\ UNCHANGED <<snn, nh, h1Node, h1Dead, h2Node, nl, l0, l1, mark, sfree, extOwned>>

\* .Lmain_condense removes dead versions and caps active versions at MAXVERSIONS (src/parser.s:3086-3139).
Condense ==
    /\ nh = 2 \* src/parser.s:3089
    /\ (h1Dead \/ h2Dead) \* src/parser.s:3094
    /\ IF CondenseRemovesDead
       THEN /\ nh' = 1
            /\ h1Node' = IF h1Dead THEN h2Node ELSE h1Node
            /\ h1Dead' = FALSE
            /\ h2Node' = 0 /\ h2Dead' = FALSE
       ELSE UNCHANGED <<nh, h1Node, h1Dead, h2Node, h2Dead>>
    /\ afterCondense' = TRUE
    /\ act' = "Condense"
    /\ UNCHANGED <<snn, nl, l0, l1, mark, sfree, extOwned>>

\* p_snode_slow -> p_gc: mark stack nodes reachable from C_HEADS[0..C_NH-1] (and base node 0),
\* sweep unmarked nodes in 1..C_SNN-1 into C_SFREE (and their extension blocks into C_XFREE),
\* and clear SN_MARK on surviving nodes (src/parser.s:568, 582-699).
RunGC ==
    LET Alloc == { n \in AllocNodes : n < snn }
        Marked == (IF GCMarksBase THEN {0} ELSE {}) \cup ReachFrom(HeadRoots, GCMarksExtLinks)
        Swept == (Alloc \ Marked) \cup (IF GCMarksBase THEN {} ELSE {0})
        ExtSwept == { n \in Swept : n \in extOwned }
    IN
    /\ snn > 1 \* src/parser.s:564
    /\ sfree = {} \* src/parser.s:561
    /\ sfree' = Swept
    /\ extOwned' = IF GCSweepsExtBlocks THEN extOwned \ ExtSwept ELSE extOwned
    /\ nl' = [n \in Nodes |->
                IF n \in Swept
                THEN (IF GCSweepsExtBlocks /\ nl[n] > 1 THEN 0 ELSE nl[n])
                ELSE nl[n]]
    /\ mark' = [n \in Nodes |-> IF GCClearsMarks THEN FALSE ELSE (n \in Marked)]
    /\ afterCondense' = FALSE
    /\ act' = "RunGC"
    /\ UNCHANGED <<snn, nh, h1Node, h1Dead, h2Node, h2Dead, l0, l1>>

Next ==
    \/ PushV1
    \/ ForkV2
    \/ PushV2
    \/ MergeV2IntoV1
    \/ TryAddSelfLink
    \/ ReduceInPlace
    \/ ReduceMergedGSS
    \/ ShareHeadWithV2
    \/ MarkV1Dead
    \/ MarkV2Dead
    \/ Condense
    \/ RunGC

Spec == Init /\ [][Next]_vars

-------------------------------------------------------------------------------
\* Stack node 0 (the base node) is never placed on C_SFREE and always has 0 outgoing links.
BaseNodeProtected == 0 \notin sfree /\ nl[0] = 0

\* No stack node reachable from any version head in C_HEADS[0..C_NH-1] is ever on the free list C_SFREE.
LiveNodesNeverFreed == (Reachable \cap sfree) = {}

\* Every free stack node on C_SFREE has returned any extension block to C_XFREE and has SN_NL <= 1.
NoExtensionBlockLeak == \A n \in sfree : n \notin extOwned /\ nl[n] <= 1

\* Outside p_gc, every stack node in 0..C_SNN-1 has SN_MARK = 0 so the next GC starts clean.
MarksClearedAfterGC == \A n \in Nodes : ~mark[n]

\* No stack node ever has a link pointing to itself (p_add_link guards r13d != r12d).
NoSelfLoopLinks ==
    \A n \in AllocNodes :
        /\ (nl[n] >= 1 => l0[n] # n)
        /\ (nl[n] = 2 => l1[n] # n)

\* After .Lmain_condense or p_merge completes, no dead version remains in C_HEADS[0..C_NH-1].
CondenseCleanHeads ==
    afterCondense =>
        /\ ~h1Dead
        /\ (nh = 2 => ~h2Dead)
===============================================================================
