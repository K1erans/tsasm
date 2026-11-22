#ifndef TSASM_H
#define TSASM_H
#include <stdint.h>
typedef struct { uint16_t symbol; uint8_t named; uint8_t pad; uint32_t start_byte; uint32_t end_byte; int32_t parent; } TsasmNode; // 16 bytes
// Parses Go source. Writes the tree's VISIBLE nodes (named and anonymous, as C tree-sitter's ts_node_child walk would show them) in pre-order into out.
// parent = index of the parent in out, -1 for the root.
// Returns the node count, or -1 on a syntax error / unsupported construct, -2 if cap is too small.
int64_t tsasm_parse_go(const uint8_t *src, uint64_t len, TsasmNode *out, uint64_t cap);
// Symbol name for a symbol id (same ids as tree-sitter-go's parser.c, after alias resolution: report the alias symbol when one applies).
const char *tsasm_symbol_name_go(uint16_t symbol);
#endif
