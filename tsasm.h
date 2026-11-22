#ifndef TSASM_H
#define TSASM_H
#include <stdint.h>

typedef struct { uint16_t symbol; uint8_t named; uint8_t pad; uint32_t start_byte; uint32_t end_byte; int32_t parent; } TsasmNode; // 16 bytes

// A parser owns all mutable parse state: its buffers, its output array and its limits. The
// library has no other writable state, so threads may parse at the same time as long as each
// call uses its own parser. One parser serves one call at a time; it may move between threads.
typedef struct TsasmParser TsasmParser;

// Returns NULL if out of memory. The memory limit starts at TSASM_DEFAULT_MEMORY_LIMIT.
TsasmParser *tsasm_parser_new(void);
// Frees the parser, its buffers and its output array. NULL is ignored.
void tsasm_parser_free(TsasmParser *p);
// Caps the bytes the parser may hold: parse buffers plus the output array. A parse that would
// exceed it returns TSASM_ERR_LIMIT. Buffers kept from earlier parses count, so lowering the
// limit below current use frees them.
void tsasm_parser_set_memory_limit(TsasmParser *p, uint64_t bytes);
#define TSASM_DEFAULT_MEMORY_LIMIT (1ull << 30)

// Parses Go source. On success returns the node count and sets *nodes to the tree's VISIBLE nodes
// (named and anonymous, as C tree-sitter's ts_node_child walk shows them) in pre-order; parent is
// the index of the parent in that array, -1 for the root. The array belongs to the parser and stays
// valid until the next tsasm_parse_go or tsasm_parser_free on it. On failure returns a negative
// TSASM_ERR_* code and sets *nodes to NULL. nodes may be NULL to only check that the source parses.
int64_t tsasm_parse_go(TsasmParser *p, const uint8_t *src, uint64_t len, const TsasmNode **nodes);

#define TSASM_ERR_SYNTAX (-1) // no parse: the input is not valid Go (there is no error recovery)
#define TSASM_ERR_LIMIT (-3)  // a budget ran out (memory limit, work budget, GLR version cap, input >= 4 GiB)
#define TSASM_ERR_NOMEM (-4)  // the allocator failed; the parser stays usable

// Symbol name for a symbol id (same ids as tree-sitter-go's parser.c, after alias resolution).
// Thread-safe.
const char *tsasm_symbol_name_go(uint16_t symbol);
#endif
