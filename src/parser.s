# tsasm: tree-sitter style GLR parser runtime for tree-sitter-go tables, x86-64 (GNU as, intel syntax)
#
# Exports (System V AMD64):
#   int64_t tsasm_parse_go(const uint8_t *src, uint64_t len, TsasmNode *out, uint64_t cap)
#   const char *tsasm_symbol_name_go(uint16_t symbol)
#
# Internal convention: r15 = &ctx for the whole parse (callee-saved, never modified inside).
# Internal functions follow SysV (rbx, rbp, r12-r15 preserved). Grow helpers preserve ALL
# registers except flags, so hot paths can call them without spilling.
#
# Data model (mirrors test/model.c):
#   Node  (32 bytes): u16 symbol, u8 production_id, u8 extra, u32 start, u32 end,
#                     u32 child_count, u32 child_index (into kids[]), i32 dynamic_precedence
#   Entry (8 bytes) : u32 node, u32 state | (extra << 16)
#   Version (32 bytes): ptr entries, u32 size, u32 cap, u32 status (0 active,1 dead,2 accepted), i32 dynsum

	.intel_syntax noprefix
	.include "tables.inc"

	.set C_NODES, 0
	.set C_NN, 8
	.set C_NCAP, 12
	.set C_KIDS, 16
	.set C_NK, 24
	.set C_KCAP, 28
	.set C_SRC, 32
	.set C_END, 40
	.set C_NV, 48
	.set C_FINISHED, 52
	.set C_LASTPOS, 56
	.set C_TOKVALID, 60
	.set C_TSYM, 64
	.set C_TSTART, 68
	.set C_TEND, 72
	.set C_TPOS, 76
	.set C_TKW, 80
	.set C_TPSTATE, 84
	.set C_OUT, 88
	.set C_CAP, 96
	.set C_ESTK, 104
	.set C_ESTKCAP, 112
	.set C_SAVEDRSP, 120
	.set C_STEPS, 128
	.set C_NODELIM, 136
	.set C_VERS, 192
	.set MAXVERS, 128
	.set MAXV, 64
	.set C_SIZE, C_VERS + MAXVERS * 32

	.set V_E, 0
	.set V_SIZE, 8
	.set V_CAP, 12
	.set V_STATUS, 16
	.set V_DYN, 20

	.set N_SYM, 0
	.set N_PROD, 2
	.set N_EXTRA, 3
	.set N_START, 4
	.set N_END, 8
	.set N_CC, 12
	.set N_CI, 16
	.set N_DYN, 20

	.bss
	.balign 64
ctx:	.zero C_SIZE

	.text

# ---------------------------------------------------------------------------
# grow helpers: preserve every GPR (except rax where documented), realign stack for libc
# ---------------------------------------------------------------------------
.macro SAVE_VOLATILE
	push rbp
	mov rbp, rsp
	push rax
	push rcx
	push rdx
	push rsi
	push rdi
	push r8
	push r9
	push r10
	push r11
	and rsp, -16
.endm
.macro RESTORE_VOLATILE
	lea rsp, [rbp-72]
	pop r11
	pop r10
	pop r9
	pop r8
	pop rdi
	pop rsi
	pop rdx
	pop rcx
	pop rax
	pop rbp
.endm

# grow nodes so that nn < ncap
p_grow_nodes:
	SAVE_VOLATILE
	mov esi, dword ptr [r15+C_NCAP]
	cmp esi, dword ptr [r15+C_NODELIM]
	jae p_abort
	add esi, esi
	mov eax, 65536
	cmp esi, eax
	cmovb esi, eax
	mov dword ptr [r15+C_NCAP], esi
	shl rsi, 5
	mov rdi, qword ptr [r15+C_NODES]
	call realloc@PLT
	test rax, rax
	jz p_oom
	mov qword ptr [r15+C_NODES], rax
	RESTORE_VOLATILE
	ret

# grow kids so that nk + edi <= kcap
p_grow_kids:
	SAVE_VOLATILE
	mov eax, dword ptr [r15+C_NK]
	add eax, edi
	mov esi, dword ptr [r15+C_KCAP]
	test esi, esi
	jnz 1f
	mov esi, 65536
1:	cmp esi, eax
	jae 2f
	add esi, esi
	jc p_abort
	jmp 1b
2:	mov eax, dword ptr [r15+C_NODELIM]
	shl rax, 2
	cmp rsi, rax
	ja p_abort
	mov dword ptr [r15+C_KCAP], esi
	shl rsi, 2
	mov rdi, qword ptr [r15+C_KIDS]
	call realloc@PLT
	test rax, rax
	jz p_oom
	mov qword ptr [r15+C_KIDS], rax
	RESTORE_VOLATILE
	ret

# grow version stack rdi (version ptr) to hold esi entries
p_grow_vstack:
	SAVE_VOLATILE
	mov eax, dword ptr [rdi+V_CAP]
	test eax, eax
	jnz 1f
	mov eax, 256
1:	cmp eax, esi
	jae 2f
	add eax, eax
	jmp 1b
2:	cmp eax, 0x400000
	ja p_abort
	mov dword ptr [rdi+V_CAP], eax
	mov r8, rdi
	mov esi, eax
	shl rsi, 3
	mov rdi, qword ptr [r8+V_E]
	push r8
	push r8
	call realloc@PLT
	pop r8
	pop r8
	test rax, rax
	jz p_oom
	mov qword ptr [r8+V_E], rax
	RESTORE_VOLATILE
	ret

# grow emit stack (frames of 16 bytes) to at least esi frames
p_grow_estk:
	SAVE_VOLATILE
	mov eax, dword ptr [r15+C_ESTKCAP]
	test eax, eax
	jnz 1f
	mov eax, 1024
1:	cmp eax, esi
	ja 2f
	add eax, eax
	jmp 1b
2:	mov dword ptr [r15+C_ESTKCAP], eax
	mov esi, eax
	shl rsi, 4
	mov rdi, qword ptr [r15+C_ESTK]
	call realloc@PLT
	test rax, rax
	jz p_oom
	mov qword ptr [r15+C_ESTK], rax
	RESTORE_VOLATILE
	ret

p_oom:
p_abort:
	mov rsp, qword ptr [r15+C_SAVEDRSP]
	mov rax, -1
	jmp .Lmain_ret

# ALLOC_NODE reg32: reg = new node index (nn++), may grow (preserves regs)
.macro ALLOC_NODE reg
	mov \reg, dword ptr [r15+C_NN]
	cmp \reg, dword ptr [r15+C_NCAP]
	jb 91f
	call p_grow_nodes
91:	inc dword ptr [r15+C_NN]
.endm

# ---------------------------------------------------------------------------
# p_lex(edi = parse state, esi = byte position) -> eax = 1 token lexed into cache, 0 = no token
# ---------------------------------------------------------------------------
	.p2align 4
p_lex:
	push rbx
	push r12
	push r13
	mov ebx, edi
	mov dword ptr [r15+C_TPOS], esi
	mov dword ptr [r15+C_TPSTATE], edi
	mov rsi, qword ptr [r15+C_SRC]
	mov r12, rsi                        # r12 = src base
	mov eax, dword ptr [r15+C_TPOS]
	add rsi, rax
	mov r9, qword ptr [r15+C_END]
	lea rax, [rip + tsg_lex_state]
	movzx ecx, word ptr [rax + rbx*2]
	call tsl_lex_main
	cmp ecx, -1
	je .Llex_fail
	sub r10, r12
	sub r11, r12
	mov dword ptr [r15+C_TSTART], r10d
	mov dword ptr [r15+C_TEND], r11d
	mov dword ptr [r15+C_TKW], 0
	cmp ecx, TSG_WORD
	jne .Llex_store
	# keyword extraction
	mov r13d, r11d                      # identifier end offset
	lea rsi, [r12 + r10]
	call tsl_lex_keyword
	cmp ecx, -1
	je .Llex_word
	mov dword ptr [r15+C_TKW], 1
	sub r11, r12
	cmp r11d, r13d
	jne .Llex_word
	# has_actions(state, kw) || is_reserved(state, kw)
	imul eax, ebx, TSG_NSYM
	add eax, ecx
	lea rdx, [rip + tsg_parse_table]
	movzx eax, word ptr [rdx + rax*2]
	test eax, eax
	jnz .Llex_store
	lea rdx, [rip + tsg_reserved_set]
	movzx eax, byte ptr [rdx + rbx]
	add eax, eax
	mov edx, ecx
	shr edx, 6
	add eax, edx
	lea rdx, [rip + tsg_reserved_bits]
	mov rax, qword ptr [rdx + rax*8]
	bt rax, rcx
	jc .Llex_store
.Llex_word:
	mov ecx, TSG_WORD
.Llex_store:
	mov dword ptr [r15+C_TSYM], ecx
	mov dword ptr [r15+C_TOKVALID], 1
	mov eax, 1
	pop r13
	pop r12
	pop rbx
	ret
.Llex_fail:
	mov dword ptr [r15+C_TOKVALID], 0
	xor eax, eax
	pop r13
	pop r12
	pop rbx
	ret

# ---------------------------------------------------------------------------
# p_can_reuse(edi = state) -> eax (cached token reusable in this state)
# ---------------------------------------------------------------------------
	.p2align 4
p_can_reuse:
	mov ecx, dword ptr [r15+C_TSYM]
	imul eax, edi, TSG_NSYM
	add eax, ecx
	lea rdx, [rip + tsg_parse_table]
	movzx eax, word ptr [rdx + rax*2]
	lea rdx, [rip + tsg_actions]
	mov r8, qword ptr [rdx + rax*8]     # header
	test r8b, r8b
	jz .Lcr_size
	mov esi, dword ptr [r15+C_TPSTATE]
	lea rdx, [rip + tsg_lex_state]
	movzx eax, word ptr [rdx + rsi*2]
	movzx r9d, word ptr [rdx + rdi*2]
	cmp eax, r9d
	jne .Lcr_size
	lea rdx, [rip + tsg_reserved_set]
	movzx eax, byte ptr [rdx + rsi]
	movzx r9d, byte ptr [rdx + rdi]
	cmp eax, r9d
	jne .Lcr_size
	cmp ecx, TSG_WORD
	jne .Lcr_yes
	cmp dword ptr [r15+C_TKW], 0
	jne .Lcr_size
	cmp esi, edi
	je .Lcr_yes
.Lcr_size:
	mov eax, dword ptr [r15+C_TEND]
	cmp eax, dword ptr [r15+C_TSTART]
	jne .Lcr_flag
	test ecx, ecx
	jz .Lcr_flag
	xor eax, eax
	ret
.Lcr_flag:
	mov rax, r8
	shr eax, 8
	and eax, 1
	ret
.Lcr_yes:
	mov eax, 1
	ret

# ---------------------------------------------------------------------------
# p_shift(rdi = version ptr, esi = state, edx = symbol, ecx = extra flag)
# leaf start/end come from the token cache
# ---------------------------------------------------------------------------
	.p2align 4
p_shift:
	test edx, edx
	jnz 1f
	mov ecx, 1                          # eof is always extra
1:	ALLOC_NODE eax
	mov r8, qword ptr [r15+C_NODES]
	mov r9d, eax
	shl r9, 5
	add r9, r8                          # node ptr
	mov r10d, ecx
	shl r10d, 24
	or r10d, edx                        # symbol | extra<<24 (prod = 0)
	mov dword ptr [r9+N_SYM], r10d
	mov r10, qword ptr [r15+C_TSTART]   # start, end
	mov qword ptr [r9+N_START], r10
	mov qword ptr [r9+N_CC], 0
	mov dword ptr [r9+N_DYN], 0
	# push entry
	shl ecx, 16
	or esi, ecx
	mov ecx, dword ptr [rdi+V_SIZE]
	cmp ecx, dword ptr [rdi+V_CAP]
	jb 2f
	push rsi
	lea esi, [rcx+1]
	call p_grow_vstack
	pop rsi
2:	mov r8, qword ptr [rdi+V_E]
	mov dword ptr [r8 + rcx*8], eax
	mov dword ptr [r8 + rcx*8 + 4], esi
	inc ecx
	mov dword ptr [rdi+V_SIZE], ecx
	ret

# ---------------------------------------------------------------------------
# p_reduce(rdi = version ptr, rsi = reduce action word)
# ---------------------------------------------------------------------------
	.p2align 4
p_reduce:
	push rbx
	push rbp
	push r12
	push r13
	push r14
	mov r12, rdi
	mov r13, rsi
	mov rbx, qword ptr [r12+V_E]
	mov r14d, dword ptr [r12+V_SIZE]    # i
	mov rax, r13
	shr eax, 8
	movzx ecx, al                       # child count
	test ecx, ecx
	jz .Lrd_popped
.Lrd_pop:
	dec r14d
	test byte ptr [rbx + r14*8 + 6], 1
	jnz .Lrd_pop
	dec ecx
	jnz .Lrd_pop
.Lrd_popped:
	mov ebp, dword ptr [r12+V_SIZE]     # j
.Lrd_trail:
	cmp ebp, r14d
	jbe .Lrd_trdone
	test byte ptr [rbx + rbp*8 - 2], 1
	jz .Lrd_trdone
	dec ebp
	jmp .Lrd_trail
.Lrd_trdone:
	# ensure kids capacity
	mov edi, ebp
	sub edi, r14d
	mov eax, dword ptr [r15+C_NK]
	add eax, edi
	cmp eax, dword ptr [r15+C_KCAP]
	jbe 1f
	call p_grow_kids
1:	ALLOC_NODE eax
	mov r8, qword ptr [r15+C_NODES]
	mov r9d, eax                        # keep n in eax
	shl r9, 5
	add r9, r8                          # node ptr
	mov rcx, r13
	shr rcx, 16
	movzx edx, cx                       # symbol
	shr rcx, 16
	movzx ecx, cl                       # production id
	shl ecx, 16
	or ecx, edx
	mov dword ptr [r9+N_SYM], ecx       # symbol | prod<<16 | extra=0
	mov ecx, ebp
	sub ecx, r14d
	mov dword ptr [r9+N_CC], ecx
	mov r10, qword ptr [r15+C_KIDS]
	mov r11d, dword ptr [r15+C_NK]
	mov dword ptr [r9+N_CI], r11d
	mov rdi, r13
	sar rdi, 48                         # action dynamic precedence
	mov edx, r14d
	cmp edx, ebp
	jae .Lrd_nokids
.Lrd_kids:
	mov esi, dword ptr [rbx + rdx*8]
	mov dword ptr [r10 + r11*4], esi
	inc r11d
	shl rsi, 5
	add edi, dword ptr [r8 + rsi + N_DYN]
	inc edx
	cmp edx, ebp
	jb .Lrd_kids
	mov dword ptr [r15+C_NK], r11d
	mov dword ptr [r9+N_DYN], edi
	mov esi, dword ptr [rbx + r14*8]
	shl rsi, 5
	mov ecx, dword ptr [r8 + rsi + N_START]
	mov dword ptr [r9+N_START], ecx
	mov esi, dword ptr [rbx + rbp*8 - 8]
	shl rsi, 5
	mov ecx, dword ptr [r8 + rsi + N_END]
	mov dword ptr [r9+N_END], ecx
	jmp .Lrd_goto
.Lrd_nokids:
	mov dword ptr [r9+N_DYN], edi
	xor ecx, ecx
	cmp r14d, 1
	jbe 2f
	mov esi, dword ptr [rbx + r14*8 - 8]
	shl rsi, 5
	mov ecx, dword ptr [r8 + rsi + N_END]
2:	mov dword ptr [r9+N_START], ecx
	mov dword ptr [r9+N_END], ecx
.Lrd_goto:
	# version dynsum += action dyn
	mov rdi, r13
	sar rdi, 48
	add dword ptr [r12+V_DYN], edi
	# next = goto(state below, symbol)
	movzx ecx, word ptr [rbx + r14*8 - 4]
	imul ecx, ecx, TSG_NSYM
	mov rdx, r13
	shr rdx, 16
	movzx edx, dx
	add ecx, edx
	lea rdx, [rip + tsg_parse_table]
	movzx ecx, word ptr [rdx + rcx*2]   # next state
	mov edx, dword ptr [r12+V_SIZE]
	sub edx, ebp                        # ntrail
	jnz .Lrd_withtrail
	mov dword ptr [rbx + r14*8], eax
	mov dword ptr [rbx + r14*8 + 4], ecx
	inc r14d
	mov dword ptr [r12+V_SIZE], r14d
	pop r14
	pop r13
	pop r12
	pop rbp
	pop rbx
	ret
.Lrd_withtrail:
	# make room if i == j
	mov esi, dword ptr [r12+V_SIZE]
	inc esi
	cmp esi, dword ptr [r12+V_CAP]
	jbe 3f
	mov rdi, r12
	call p_grow_vstack
	mov rbx, qword ptr [r12+V_E]
3:	mov r8d, ecx
	or r8d, 0x10000                     # extra entries' state
	lea esi, [r14+1]
	cmp esi, ebp
	ja .Lrd_back
	# forward copy e[i+1+k] = e[j+k]
	xor edi, edi
4:	lea r9d, [rbp + rdi]
	mov r10d, dword ptr [rbx + r9*8]
	lea r9d, [rsi + rdi]
	mov dword ptr [rbx + r9*8], r10d
	mov dword ptr [rbx + r9*8 + 4], r8d
	inc edi
	cmp edi, edx
	jb 4b
	jmp .Lrd_tdone
.Lrd_back:
	mov edi, edx
5:	dec edi
	lea r9d, [rbp + rdi]
	mov r10d, dword ptr [rbx + r9*8]
	lea r9d, [rsi + rdi]
	mov dword ptr [rbx + r9*8], r10d
	mov dword ptr [rbx + r9*8 + 4], r8d
	test edi, edi
	jnz 5b
.Lrd_tdone:
	mov dword ptr [rbx + r14*8], eax
	mov dword ptr [rbx + r14*8 + 4], ecx
	add esi, edx
	mov dword ptr [r12+V_SIZE], esi
	pop r14
	pop r13
	pop r12
	pop rbp
	pop rbx
	ret

# ---------------------------------------------------------------------------
# p_copy_version(edi = source index) -> eax = new index or -1
# ---------------------------------------------------------------------------
p_copy_version:
	mov eax, dword ptr [r15+C_NV]
	cmp eax, MAXVERS
	jae .Lcv_fail
	mov ecx, eax
	shl ecx, 5
	lea r8, [r15 + C_VERS + rcx]        # dest
	shl edi, 5
	lea r9, [r15 + C_VERS + rdi]        # src
	mov esi, dword ptr [r9+V_SIZE]
	cmp esi, dword ptr [r8+V_CAP]
	jbe 1f
	mov rdi, r8
	call p_grow_vstack
1:	push rsi
	push rdi
	mov rdi, qword ptr [r8+V_E]
	mov rsi, qword ptr [r9+V_E]
	mov ecx, dword ptr [r9+V_SIZE]
	rep movsq
	pop rdi
	pop rsi
	mov ecx, dword ptr [r9+V_SIZE]
	mov dword ptr [r8+V_SIZE], ecx
	mov dword ptr [r8+V_STATUS], 0
	mov ecx, dword ptr [r9+V_DYN]
	mov dword ptr [r8+V_DYN], ecx
	inc dword ptr [r15+C_NV]
	ret
.Lcv_fail:
	mov eax, -1
	ret

# p_remove_version(edi = index): rotate the version (and its buffer) to the end, nv--
p_remove_version:
	mov eax, dword ptr [r15+C_NV]
	dec eax
	mov dword ptr [r15+C_NV], eax
	shl edi, 5
	lea r8, [r15 + C_VERS + rdi]
	movdqu xmm0, xmmword ptr [r8]
	movdqu xmm1, xmmword ptr [r8+16]
	shl eax, 5
	lea r9, [r15 + C_VERS + rax]        # last slot
1:	cmp r8, r9
	jae 2f
	movdqu xmm2, xmmword ptr [r8+32]
	movdqu xmm3, xmmword ptr [r8+48]
	movdqu xmmword ptr [r8], xmm2
	movdqu xmmword ptr [r8+16], xmm3
	add r8, 32
	jmp 1b
2:	movdqu xmmword ptr [r9], xmm0
	movdqu xmmword ptr [r9+16], xmm1
	ret

# p_swap_versions(edi, esi)
p_swap_versions:
	shl edi, 5
	shl esi, 5
	lea r8, [r15 + C_VERS + rdi]
	lea r9, [r15 + C_VERS + rsi]
	movdqu xmm0, xmmword ptr [r8]
	movdqu xmm1, xmmword ptr [r8+16]
	movdqu xmm2, xmmword ptr [r9]
	movdqu xmm3, xmmword ptr [r9+16]
	movdqu xmmword ptr [r9], xmm0
	movdqu xmmword ptr [r9+16], xmm1
	movdqu xmmword ptr [r8], xmm2
	movdqu xmmword ptr [r8+16], xmm3
	ret

# p_vpos(rdi = version ptr) -> eax = byte position (end of top node)
.macro VPOS vreg, o64, o32, tmp
	mov \o32, dword ptr [\vreg+V_SIZE]
	mov \tmp, qword ptr [\vreg+V_E]
	mov \o32, dword ptr [\tmp + \o64*8 - 8]
	shl \o64, 5
	mov \tmp, qword ptr [r15+C_NODES]
	mov \o32, dword ptr [\tmp + \o64 + N_END]
.endm

# ---------------------------------------------------------------------------
# p_subtree_compare(edi = a, esi = b) -> eax in {-1, 0, 1}
# ---------------------------------------------------------------------------
p_subtree_compare:
	push rbx
	push r12
	push r13
	mov r8, qword ptr [r15+C_NODES]
	mov eax, edi
	shl rax, 5
	lea r12, [r8 + rax]
	mov eax, esi
	shl rax, 5
	lea r13, [r8 + rax]
	movzx eax, word ptr [r12+N_SYM]
	movzx ecx, word ptr [r13+N_SYM]
	cmp eax, ecx
	jb .Lsc_m1
	ja .Lsc_p1
	mov eax, dword ptr [r12+N_CC]
	cmp eax, dword ptr [r13+N_CC]
	jb .Lsc_m1
	ja .Lsc_p1
	xor ebx, ebx
.Lsc_loop:
	cmp ebx, dword ptr [r12+N_CC]
	jae .Lsc_eq
	mov r9, qword ptr [r15+C_KIDS]
	mov eax, dword ptr [r12+N_CI]
	add eax, ebx
	mov edi, dword ptr [r9 + rax*4]
	mov eax, dword ptr [r13+N_CI]
	add eax, ebx
	mov esi, dword ptr [r9 + rax*4]
	call p_subtree_compare
	test eax, eax
	jnz .Lsc_ret
	inc ebx
	jmp .Lsc_loop
.Lsc_eq:
	xor eax, eax
	jmp .Lsc_ret
.Lsc_m1:
	mov eax, -1
	jmp .Lsc_ret
.Lsc_p1:
	mov eax, 1
.Lsc_ret:
	pop r13
	pop r12
	pop rbx
	ret

# p_equivalent(edi = a, esi = b) -> eax (same symbol, span, child count, extra)
p_equivalent:
	mov r8, qword ptr [r15+C_NODES]
	shl rdi, 5
	shl rsi, 5
	xor eax, eax
	mov ecx, dword ptr [r8 + rdi + N_SYM]
	and ecx, 0xFF00FFFF                 # symbol + extra (ignore production id)
	mov edx, dword ptr [r8 + rsi + N_SYM]
	and edx, 0xFF00FFFF
	cmp ecx, edx
	jne 1f
	mov rcx, qword ptr [r8 + rdi + N_START]
	cmp rcx, qword ptr [r8 + rsi + N_START]
	jne 1f
	mov ecx, dword ptr [r8 + rdi + N_CC]
	cmp ecx, dword ptr [r8 + rsi + N_CC]
	jne 1f
	mov eax, 1
1:	ret

# ---------------------------------------------------------------------------
# p_try_converge(edi = j, esi = i) -> eax 1 if converged (caller removes i), else 0
# ---------------------------------------------------------------------------
p_try_converge:
	push rbx
	push rbp
	push r12
	push r13
	push r14
	shl edi, 5
	lea r12, [r15 + C_VERS + rdi]       # X
	shl esi, 5
	lea r13, [r15 + C_VERS + rsi]       # Y
	xor eax, eax
	mov ecx, dword ptr [r12+V_SIZE]
	cmp ecx, dword ptr [r13+V_SIZE]
	jne .Ltc_ret
	mov r8, qword ptr [r12+V_E]
	mov r9, qword ptr [r13+V_E]
	lea ebp, [rcx-1]                    # hi
	mov ebx, ebp                        # k
.Ltc_scan:
	test ebx, ebx
	jz .Ltc_scanned
	mov edx, dword ptr [r8 + rbx*8]
	cmp edx, dword ptr [r9 + rbx*8]
	je .Ltc_scanned
	mov edx, dword ptr [r8 + rbx*8 + 4]
	cmp edx, dword ptr [r9 + rbx*8 + 4]
	jne .Ltc_ret                        # eax = 0
	dec ebx
	jmp .Ltc_scan
.Ltc_scanned:
	inc ebx                             # lo
	mov eax, 1
	cmp ebx, ebp
	ja .Ltc_ret                         # identical
	lea r14d, [rbx+1]
.Ltc_eq:
	cmp r14d, ebp
	ja .Ltc_pick
	mov r8, qword ptr [r12+V_E]
	mov r9, qword ptr [r13+V_E]
	mov edi, dword ptr [r8 + r14*8]
	mov esi, dword ptr [r9 + r14*8]
	call p_equivalent
	test eax, eax
	jz .Ltc_ret
	inc r14d
	jmp .Ltc_eq
.Ltc_pick:
	mov r8, qword ptr [r12+V_E]
	mov r9, qword ptr [r13+V_E]
	mov edi, dword ptr [r8 + rbx*8]     # xn
	mov esi, dword ptr [r9 + rbx*8]     # yn
	mov r10, qword ptr [r15+C_NODES]
	mov eax, edi
	shl rax, 5
	mov ecx, dword ptr [r10 + rax + N_DYN]
	mov eax, esi
	shl rax, 5
	mov edx, dword ptr [r10 + rax + N_DYN]
	cmp edx, ecx
	jg .Ltc_ywins
	jl .Ltc_xwins
	push rdi
	push rsi
	call p_equivalent
	pop rsi
	pop rdi
	test eax, eax
	jnz .Ltc_xwins
	call p_subtree_compare
	cmp eax, 0
	jle .Ltc_xwins
.Ltc_ywins:
	mov r8, qword ptr [r12+V_E]
	mov r9, qword ptr [r13+V_E]
	mov r10, qword ptr [r15+C_NODES]
	mov eax, dword ptr [r8 + rbx*8]
	shl rax, 5
	mov ecx, dword ptr [r10 + rax + N_DYN]
	sub dword ptr [r12+V_DYN], ecx
	mov rax, qword ptr [r9 + rbx*8]
	mov qword ptr [r8 + rbx*8], rax
	mov eax, eax
	shl rax, 5
	mov ecx, dword ptr [r10 + rax + N_DYN]
	add dword ptr [r12+V_DYN], ecx
.Ltc_xwins:
	mov eax, 1
.Ltc_ret:
	pop r14
	pop r13
	pop r12
	pop rbp
	pop rbx
	ret

# ---------------------------------------------------------------------------
# p_accept(edi = version index): build root, select against finished tree
# ---------------------------------------------------------------------------
p_accept:
	push rbx
	push rbp
	push r12
	push r13
	push r14
	shl edi, 5
	lea r12, [r15 + C_VERS + rdi]
	# push eof leaf (symbol 0, extra) with state 1
	mov rdi, r12
	mov esi, 1
	xor edx, edx
	mov ecx, 1
	call p_shift
	mov rbx, qword ptr [r12+V_E]
	mov ecx, dword ptr [r12+V_SIZE]
	# find topmost non-extra entry r (>= 1)
	lea ebp, [rcx-1]
1:	test byte ptr [rbx + rbp*8 + 6], 1
	jz 2f
	dec ebp
	jnz 1b
	ud2                                 # no root (cannot happen)
2:	mov r13d, dword ptr [rbx + rbp*8]   # root node index
	# total children = (size - 1) - 1 + root.cc
	mov r8, qword ptr [r15+C_NODES]
	mov eax, r13d
	shl rax, 5
	mov r14d, dword ptr [r8 + rax + N_CC]
	lea edi, [rcx - 2 + r14]
	mov eax, dword ptr [r15+C_NK]
	add eax, edi
	cmp eax, dword ptr [r15+C_KCAP]
	jbe 3f
	call p_grow_kids
3:	ALLOC_NODE eax
	push rax
	push rax
	mov r8, qword ptr [r15+C_NODES]
	mov ecx, r13d
	shl rcx, 5
	mov r9d, eax
	shl r9, 5
	movdqu xmm0, xmmword ptr [r8 + rcx]
	movdqu xmm1, xmmword ptr [r8 + rcx + 16]
	movdqu xmmword ptr [r8 + r9], xmm0
	movdqu xmmword ptr [r8 + r9 + 16], xmm1
	add r9, r8                          # new root ptr
	add rcx, r8                         # old root ptr
	mov r10, qword ptr [r15+C_KIDS]
	mov r11d, dword ptr [r15+C_NK]
	mov dword ptr [r9+N_CI], r11d
	mov esi, r11d                       # first child slot
	mov edx, 1                          # k
4:	cmp edx, dword ptr [r12+V_SIZE]
	jae 6f
	cmp edx, ebp
	jne 5f
	# splice root's children
	mov edi, dword ptr [rcx+N_CI]
	mov eax, r14d
	test eax, eax
	jz 7f
8:	mov r8d, dword ptr [r10 + rdi*4]
	mov dword ptr [r10 + r11*4], r8d
	inc edi
	inc r11d
	dec eax
	jnz 8b
7:	inc edx
	jmp 4b
5:	mov eax, dword ptr [rbx + rdx*8]
	mov dword ptr [r10 + r11*4], eax
	inc r11d
	inc edx
	jmp 4b
6:	mov eax, r11d
	sub eax, esi
	mov dword ptr [r9+N_CC], eax
	mov dword ptr [r15+C_NK], r11d
	mov r8, qword ptr [r15+C_NODES]
	mov eax, dword ptr [r10 + rsi*4]
	shl rax, 5
	mov eax, dword ptr [r8 + rax + N_START]
	mov dword ptr [r9+N_START], eax
	mov eax, dword ptr [r10 + r11*4 - 4]
	shl rax, 5
	mov eax, dword ptr [r8 + rax + N_END]
	mov dword ptr [r9+N_END], eax
	pop rax
	pop rax
	# select against finished
	mov ecx, dword ptr [r15+C_FINISHED]
	test ecx, ecx
	js .Lac_take
	mov edx, ecx
	shl rdx, 5
	mov edx, dword ptr [r8 + rdx + N_DYN]
	mov esi, dword ptr [r9+N_DYN]
	cmp esi, edx
	jg .Lac_take
	jl .Lac_done
	mov ebx, eax
	mov edi, ecx
	mov esi, eax
	call p_subtree_compare
	cmp eax, 0
	jle .Lac_done
	mov eax, ebx
.Lac_take:
	mov dword ptr [r15+C_FINISHED], eax
.Lac_done:
	mov dword ptr [r12+V_STATUS], 2
	pop r14
	pop r13
	pop r12
	pop rbp
	pop rbx
	ret

# ---------------------------------------------------------------------------
# p_advance(edi = version index): lex (or reuse cached token), apply actions until a shift,
# accept or error.  Mirrors ts_parser__advance.
# ---------------------------------------------------------------------------
	.p2align 4
p_advance:
	push rbx
	push rbp
	push r12
	push r13
	push r14
	sub rsp, 16                         # [rsp] = k, [rsp+4] = count, [rsp+8] = last_red
	sub qword ptr [r15+C_STEPS], 1
	jb p_abort
	mov ebx, edi
	mov eax, edi
	shl eax, 5
	lea r12, [r15 + C_VERS + rax]
	mov ecx, dword ptr [r12+V_SIZE]
	mov rdx, qword ptr [r12+V_E]
	movzx r13d, word ptr [rdx + rcx*8 - 4]      # state
	VPOS r12, rsi, esi, rdx
	cmp dword ptr [r15+C_TOKVALID], 0
	je .Lad_lex
	cmp esi, dword ptr [r15+C_TPOS]
	jne .Lad_lex
	mov edi, r13d
	push rsi
	push rsi
	call p_can_reuse
	pop rsi
	pop rsi
	test eax, eax
	jnz .Lad_have
.Lad_lex:
	mov edi, r13d
	call p_lex
	test eax, eax
	jz .Lad_dead
.Lad_have:
	mov r14d, dword ptr [r15+C_TSYM]
.Lad_lookup:
	imul eax, r13d, TSG_NSYM
	add eax, r14d
	lea rdx, [rip + tsg_parse_table]
	movzx ebp, word ptr [rdx + rax*2]   # action index
.Lad_entry:
	lea rdx, [rip + tsg_actions]
	mov rax, qword ptr [rdx + rbp*8]
	movzx ecx, al                       # count
	cmp ecx, 1
	jne .Lad_multi
	# ---- single action fast path
	mov rsi, qword ptr [rdx + rbp*8 + 8]
	test esi, 3
	jnz .Lad_single_nonshift
	# shift
	mov ecx, esi
	shr ecx, 2
	and ecx, 1
	mov eax, esi
	shr eax, 16
	test ecx, ecx
	cmovnz eax, r13d
	mov rdi, r12
	mov esi, eax
	mov edx, r14d
	call p_shift
	jmp .Lad_ret
.Lad_single_nonshift:
	mov eax, esi
	and eax, 3
	cmp eax, 1
	jne .Lad_acc_or_rec
	mov rdi, r12
	call p_reduce
	mov ecx, dword ptr [r12+V_SIZE]
	mov rdx, qword ptr [r12+V_E]
	movzx r13d, word ptr [rdx + rcx*8 - 4]
	jmp .Lad_lookup
.Lad_acc_or_rec:
	cmp eax, 2
	jne .Lad_dead
	mov edi, ebx
	call p_accept
	jmp .Lad_ret
	# ---- zero or multiple actions
.Lad_multi:
	test ecx, ecx
	jz .Lad_noaction
	mov dword ptr [rsp+4], ecx
	mov dword ptr [rsp], 0
	mov dword ptr [rsp+8], -1
.Lad_mloop:
	mov ecx, dword ptr [rsp]
	cmp ecx, dword ptr [rsp+4]
	jae .Lad_mdone
	inc dword ptr [rsp]
	lea rdx, [rip + tsg_actions]
	lea eax, [rbp + rcx + 1]
	mov rsi, qword ptr [rdx + rax*8]
	mov eax, esi
	and eax, 3
	jz .Lad_mshift
	cmp eax, 1
	jne .Lad_macc
	# reduce on a copy
	push rsi
	push rsi
	mov edi, ebx
	call p_copy_version
	pop rsi
	pop rsi
	test eax, eax
	js .Lad_dead
	mov dword ptr [rsp+8], eax
	shl eax, 5
	lea rdi, [r15 + C_VERS + rax]
	call p_reduce
	jmp .Lad_mloop
.Lad_mshift:
	mov ecx, esi
	shr ecx, 2
	and ecx, 1
	mov eax, esi
	shr eax, 16
	test ecx, ecx
	cmovnz eax, r13d
	mov rdi, r12
	mov esi, eax
	mov edx, r14d
	call p_shift
	jmp .Lad_ret
.Lad_macc:
	cmp eax, 2
	jne .Lad_dead
	mov edi, ebx
	call p_accept
	jmp .Lad_ret
.Lad_mdone:
	mov eax, dword ptr [rsp+8]
	test eax, eax
	js .Lad_noaction
	# renumber: last reduction version replaces version ebx
	mov edi, ebx
	mov esi, eax
	call p_swap_versions
	mov edi, dword ptr [rsp+8]
	call p_remove_version
	mov ecx, dword ptr [r12+V_SIZE]
	mov rdx, qword ptr [r12+V_E]
	movzx r13d, word ptr [rdx + rcx*8 - 4]
	jmp .Lad_lookup
.Lad_noaction:
	# keyword -> word token fallback
	cmp dword ptr [r15+C_TKW], 0
	je .Lad_dead
	cmp r14d, TSG_WORD
	je .Lad_dead
	cmp r14d, 128
	jae 1f
	lea rdx, [rip + tsg_reserved_set]
	movzx eax, byte ptr [rdx + r13]
	add eax, eax
	mov edx, r14d
	shr edx, 6
	add eax, edx
	lea rdx, [rip + tsg_reserved_bits]
	mov rax, qword ptr [rdx + rax*8]
	bt rax, r14
	jc .Lad_dead
1:	imul eax, r13d, TSG_NSYM
	add eax, TSG_WORD
	lea rdx, [rip + tsg_parse_table]
	movzx eax, word ptr [rdx + rax*2]
	lea rdx, [rip + tsg_actions]
	cmp byte ptr [rdx + rax*8], 0
	je .Lad_dead
	mov r14d, TSG_WORD
	mov ebp, eax
	jmp .Lad_entry
.Lad_dead:
	mov dword ptr [r12+V_STATUS], 1
.Lad_ret:
	add rsp, 16
	pop r14
	pop r13
	pop r12
	pop rbp
	pop rbx
	ret

# ---------------------------------------------------------------------------
# p_emit() -> rax = node count or -2
# ---------------------------------------------------------------------------
p_emit:
	push rbx
	push rbp
	push r12
	push r13
	push r14
	mov r8, qword ptr [r15+C_NODES]
	mov r12, qword ptr [r15+C_OUT]
	mov r13, qword ptr [r15+C_CAP]
	mov eax, -2
	test r13, r13
	jz .Lem_ret
	mov ecx, dword ptr [r15+C_FINISHED]
	mov eax, ecx
	shl rax, 5
	lea r9, [r8 + rax]
	movzx eax, word ptr [r9+N_SYM]
	lea rdx, [rip + tsg_symbol_flags]
	movzx edx, byte ptr [rdx + rax]
	shr edx, 1
	and edx, 1
	lea rsi, [rip + tsg_public_symbol]
	movzx eax, word ptr [rsi + rax*2]
	shl edx, 16
	or eax, edx
	mov dword ptr [r12], eax
	mov rax, qword ptr [r9+N_START]
	mov qword ptr [r12+4], rax
	mov dword ptr [r12+12], -1
	mov r14d, 1                         # n
	# frame stack
	cmp dword ptr [r15+C_ESTKCAP], 0
	jne 1f
	mov esi, 1
	call p_grow_estk
1:	mov rbx, qword ptr [r15+C_ESTK]
	xor ebp, ebp                        # sp (frames)
	mov dword ptr [rbx], ecx
	mov qword ptr [rbx+4], 0
	mov dword ptr [rbx+12], 0
	inc ebp
	mov r10, qword ptr [r15+C_KIDS]
	mov r8, qword ptr [r15+C_NODES]
.Lem_loop:
	test ebp, ebp
	jz .Lem_done
	mov eax, ebp
	shl eax, 4
	lea rdi, [rbx + rax - 16]           # frame ptr
	mov eax, dword ptr [rdi]
	shl rax, 5
	lea r9, [r8 + rax]                  # parent node
	mov ecx, dword ptr [rdi+4]          # ci
	cmp ecx, dword ptr [r9+N_CC]
	jae .Lem_pop
	lea edx, [rcx+1]
	mov dword ptr [rdi+4], edx
	add ecx, dword ptr [r9+N_CI]
	mov esi, dword ptr [r10 + rcx*4]    # child index
	mov eax, esi
	shl rax, 5
	lea r11, [r8 + rax]                 # child node
	xor edx, edx                        # alias
	cmp byte ptr [r11+N_EXTRA], 0
	jne 2f
	movzx eax, byte ptr [r9+N_PROD]
	shl eax, 4
	add eax, dword ptr [rdi+8]
	inc dword ptr [rdi+8]
	lea rcx, [rip + tsg_alias_seq]
	movzx edx, word ptr [rcx + rax*2]
2:	movzx eax, word ptr [r11+N_SYM]
	test edx, edx
	cmovnz eax, edx                     # s
	lea rcx, [rip + tsg_symbol_flags]
	movzx ecx, byte ptr [rcx + rax]
	mov r9d, dword ptr [rdi+12]         # parent out index
	test edx, edx
	jnz 3f
	test ecx, 1
	jz 4f
3:	cmp r14, r13
	jae .Lem_full
	mov rdx, r14
	shl rdx, 4
	add rdx, r12
	push rsi
	lea rsi, [rip + tsg_public_symbol]
	movzx eax, word ptr [rsi + rax*2]
	pop rsi
	shr ecx, 1
	and ecx, 1
	shl ecx, 16
	or eax, ecx
	mov dword ptr [rdx], eax
	mov rax, qword ptr [r11+N_START]
	mov qword ptr [rdx+4], rax
	mov dword ptr [rdx+12], r9d
	mov r9d, r14d
	inc r14
4:	cmp dword ptr [r11+N_CC], 0
	je .Lem_loop
	# push frame (child, 0, 0, parent)
	lea eax, [rbp+1]
	cmp eax, dword ptr [r15+C_ESTKCAP]
	jb 5f
	push rsi
	mov esi, eax
	call p_grow_estk
	pop rsi
	mov rbx, qword ptr [r15+C_ESTK]
5:	mov eax, ebp
	shl eax, 4
	mov dword ptr [rbx + rax], esi
	mov qword ptr [rbx + rax + 4], 0
	mov dword ptr [rbx + rax + 12], r9d
	inc ebp
	jmp .Lem_loop
.Lem_pop:
	dec ebp
	jmp .Lem_loop
.Lem_done:
	mov rax, r14
	jmp .Lem_ret
.Lem_full:
	mov rax, -2
.Lem_ret:
	pop r14
	pop r13
	pop r12
	pop rbp
	pop rbx
	ret

# ---------------------------------------------------------------------------
# int64_t tsasm_parse_go(const uint8_t *src, uint64_t len, TsasmNode *out, uint64_t cap)
# ---------------------------------------------------------------------------
	.globl tsasm_parse_go
	.type tsasm_parse_go, @function
	.p2align 4
tsasm_parse_go:
	push rbx
	push rbp
	push r12
	push r13
	push r14
	push r15
	sub rsp, 8
	lea r15, [rip + ctx]
	mov qword ptr [r15+C_SAVEDRSP], rsp
	mov rax, rsi
	shl rax, 6
	add rax, 10000
	mov qword ptr [r15+C_STEPS], rax    # advance budget: 64 * len + 10000
	mov rax, rsi
	shl rax, 5
	add rax, 100000
	mov r8d, 0x7FFFFFF
	cmp rax, r8
	cmova rax, r8
	mov dword ptr [r15+C_NODELIM], eax  # node budget: 32 * len + 100000
	mov rax, -1
	mov r8d, 0xFFFFFFF0                 # byte offsets are 32-bit: reject inputs >= ~4 GiB
	cmp rsi, r8
	jae .Lmain_ret
	mov qword ptr [r15+C_SRC], rdi
	add rsi, rdi
	mov qword ptr [r15+C_END], rsi
	mov qword ptr [r15+C_OUT], rdx
	mov qword ptr [r15+C_CAP], rcx
	mov dword ptr [r15+C_NN], 0
	mov dword ptr [r15+C_NK], 0
	mov dword ptr [r15+C_FINISHED], -1
	mov dword ptr [r15+C_TOKVALID], 0
	mov dword ptr [r15+C_LASTPOS], 0
	# node 0: zeroed stack-base node
	ALLOC_NODE eax
	mov r8, qword ptr [r15+C_NODES]
	pxor xmm0, xmm0
	movdqu xmmword ptr [r8], xmm0
	movdqu xmmword ptr [r8+16], xmm0
	# version 0: base entry (node 0, state 1)
	mov dword ptr [r15+C_NV], 1
	lea r12, [r15 + C_VERS]
	mov dword ptr [r12+V_STATUS], 0
	mov dword ptr [r12+V_DYN], 0
	cmp dword ptr [r12+V_CAP], 0
	jne 1f
	mov rdi, r12
	mov esi, 256
	call p_grow_vstack
1:	mov rax, qword ptr [r12+V_E]
	mov dword ptr [rax], 0
	mov dword ptr [rax+4], 1
	mov dword ptr [r12+V_SIZE], 1

.Lmain_outer:
.ifdef DEBUG
	mov rdi, r15
	call tsasm_dbg@PLT
.endif
	cmp dword ptr [r15+C_NV], 0
	je .Lmain_end
	xor ebx, ebx                        # vi
.Lmain_vloop:
	cmp ebx, dword ptr [r15+C_NV]
	jae .Lmain_condense
	mov eax, ebx
	shl eax, 5
	lea r12, [r15 + C_VERS + rax]
.Lmain_adv:
	cmp dword ptr [r12+V_STATUS], 0
	jne .Lmain_vnext
	mov edi, ebx
	call p_advance
	cmp dword ptr [r12+V_STATUS], 0
	jne .Lmain_vnext
	VPOS r12, rax, eax, rdx
	mov ecx, dword ptr [r15+C_LASTPOS]
	cmp eax, ecx
	ja .Lmain_newpos
	jb .Lmain_adv
	test ebx, ebx
	jz .Lmain_adv
.Lmain_newpos:
	mov dword ptr [r15+C_LASTPOS], eax
	cmp dword ptr [r15+C_NV], 1         # single version: keep going without condensing
	je .Lmain_adv
.Lmain_vnext:
	inc ebx
	jmp .Lmain_vloop

.Lmain_condense:
	xor ebx, ebx                        # i
.Lcd_i:
	cmp ebx, dword ptr [r15+C_NV]
	jae .Lcd_cap
	mov eax, ebx
	shl eax, 5
	lea r12, [r15 + C_VERS + rax]
	cmp dword ptr [r12+V_STATUS], 0
	je .Lcd_live
	mov edi, ebx
	call p_remove_version
	jmp .Lcd_i
.Lcd_live:
	xor ebp, ebp                        # j
.Lcd_j:
	cmp ebp, ebx
	jae .Lcd_inext
	mov eax, ebp
	shl eax, 5
	lea r13, [r15 + C_VERS + rax]       # X = j
	# same state?
	mov ecx, dword ptr [r12+V_SIZE]
	mov rdx, qword ptr [r12+V_E]
	movzx ecx, word ptr [rdx + rcx*8 - 4]
	mov esi, dword ptr [r13+V_SIZE]
	mov rdx, qword ptr [r13+V_E]
	movzx esi, word ptr [rdx + rsi*8 - 4]
	cmp ecx, esi
	jne .Lcd_dyn
	VPOS r12, rax, eax, rdx
	VPOS r13, rcx, ecx, rdx
	cmp eax, ecx
	jne .Lcd_dyn
	mov edi, ebp
	mov esi, ebx
	call p_try_converge
	test eax, eax
	jz .Lcd_jnext
	mov edi, ebx
	call p_remove_version
	jmp .Lcd_i                          # i now refers to the next version
.Lcd_dyn:
	mov eax, dword ptr [r12+V_DYN]
	cmp eax, dword ptr [r13+V_DYN]
	jle .Lcd_jnext
	mov edi, ebp
	mov esi, ebx
	call p_swap_versions
.Lcd_jnext:
	inc ebp
	jmp .Lcd_j
.Lcd_inext:
	inc ebx
	jmp .Lcd_i
.Lcd_cap:
	cmp dword ptr [r15+C_NV], MAXV
	jbe .Lmain_outer
	mov edi, MAXV
	call p_remove_version
	jmp .Lcd_cap

.Lmain_end:
	mov rax, -1
	cmp dword ptr [r15+C_FINISHED], 0
	jl .Lmain_ret
	call p_emit
.Lmain_ret:
	add rsp, 8
	pop r15
	pop r14
	pop r13
	pop r12
	pop rbp
	pop rbx
	ret
	.size tsasm_parse_go, .-tsasm_parse_go

# const char *tsasm_symbol_name_go(uint16_t symbol)
	.globl tsasm_symbol_name_go
	.type tsasm_symbol_name_go, @function
tsasm_symbol_name_go:
	movzx edi, di
	lea rax, [rip + .Lempty]
	cmp edi, TSG_TOTAL_SYMS
	jae 1f
	lea rax, [rip + tsg_symbol_names]
	mov rax, qword ptr [rax + rdi*8]
1:	ret
	.size tsasm_symbol_name_go, .-tsasm_symbol_name_go

	.section .rodata
.Lempty: .asciz ""
	.section .note.GNU-stack,"",@progbits
