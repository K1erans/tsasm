	.intel_syntax noprefix
	.text
# int32 tsl_lex_c(uint32 state, const uint8 *p, const uint8 *end, uint64 out[2])
	.globl tsl_lex_c
tsl_lex_c:
	push rbx
	mov rbx, rcx
	mov r9, rdx
	mov ecx, edi
	call tsl_lex_main
	mov qword ptr [rbx], r10
	mov qword ptr [rbx+8], r11
	mov eax, ecx
	pop rbx
	ret
# int32 tsl_kw_c(const uint8 *p, const uint8 *end, uint64 out[2])
	.globl tsl_kw_c
tsl_kw_c:
	push rbx
	mov rbx, rdx
	mov r9, rsi
	mov rsi, rdi
	call tsl_lex_keyword
	mov qword ptr [rbx], r10
	mov qword ptr [rbx+8], r11
	mov eax, ecx
	pop rbx
	ret
	.section .note.GNU-stack,"",@progbits
