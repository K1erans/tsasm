// Reference: C tree-sitter + tree-sitter-go, emitting the tsasm node format.
#include <stdint.h>
#include <stdlib.h>
#include "tree_sitter/api.h"
#include "../tsasm.h"
const TSLanguage *tree_sitter_go(void);
static TSParser *P;
static int64_t n, capg; static TsasmNode *outg; static int overflow;
static void walk(TSNode node, int32_t parent) {
  if (n >= capg) { overflow = 1; return; }
  int64_t me = n++;
  outg[me].symbol = ts_node_symbol(node);
  outg[me].named = ts_node_is_named(node);
  outg[me].pad = 0;
  outg[me].start_byte = ts_node_start_byte(node);
  outg[me].end_byte = ts_node_end_byte(node);
  outg[me].parent = parent;
  uint32_t c = ts_node_child_count(node);
  for (uint32_t i = 0; i < c; i++) walk(ts_node_child(node, i), (int32_t)me);
}
// cursor-based (faster) walk
static void walk_cursor(TSTree *tree) {
  TSTreeCursor cur = ts_tree_cursor_new(ts_tree_root_node(tree));
  int32_t stack[4096]; int depth = 0; stack[0] = -1;
  for (;;) {
    TSNode node = ts_tree_cursor_current_node(&cur);
    if (n >= capg) { overflow = 1; break; }
    int64_t me = n++;
    outg[me].symbol = ts_node_symbol(node);
    outg[me].named = ts_node_is_named(node);
    outg[me].pad = 0;
    outg[me].start_byte = ts_node_start_byte(node);
    outg[me].end_byte = ts_node_end_byte(node);
    outg[me].parent = stack[depth];
    if (ts_tree_cursor_goto_first_child(&cur)) { stack[++depth] = (int32_t)me; continue; }
    while (!ts_tree_cursor_goto_next_sibling(&cur)) {
      if (!ts_tree_cursor_goto_parent(&cur)) goto done;
      depth--;
    }
  }
done:
  ts_tree_cursor_delete(&cur);
}
int64_t ref_parse_go(const uint8_t *src, uint64_t len, TsasmNode *out, uint64_t cap, int use_cursor) {
  if (!P) { P = ts_parser_new(); ts_parser_set_language(P, tree_sitter_go()); }
  TSTree *t = ts_parser_parse_string(P, NULL, (const char *)src, (uint32_t)len);
  TSNode root = ts_tree_root_node(t);
  if (ts_node_has_error(root)) { ts_tree_delete(t); return -1; }
  n = 0; capg = cap; outg = out; overflow = 0;
  if (use_cursor) walk_cursor(t); else walk(root, -1);
  ts_tree_delete(t);
  return overflow ? -2 : n;
}
// parse only (for timing)
int64_t ref_parse_only(const uint8_t *src, uint64_t len) {
  if (!P) { P = ts_parser_new(); ts_parser_set_language(P, tree_sitter_go()); }
  TSTree *t = ts_parser_parse_string(P, NULL, (const char *)src, (uint32_t)len);
  int e = ts_node_has_error(ts_tree_root_node(t));
  ts_tree_delete(t);
  return e ? -1 : 0;
}
const char *ref_symbol_name(uint16_t s) { return ts_language_symbol_name(tree_sitter_go(), s); }
