// Reference: C tree-sitter with any grammar, emitting the tsasm node format (cursor walk).
#include <stdint.h>
#include <stdlib.h>
#include "tree_sitter/api.h"
#include "langs.h"
static TSParser *P;
static const TSLanguage *cur;
static TSParser *parser(const TSLanguage *lang) {
  if (!P) P = ts_parser_new();
  if (cur != lang) { ts_parser_set_language(P, lang); cur = lang; }
  return P;
}
// -1 on a syntax error, -2 if cap is too small
int64_t ref_parse(const TSLanguage *lang, const uint8_t *src, uint64_t len, TsasmNode *out, uint64_t cap) {
  TSTree *t = ts_parser_parse_string(parser(lang), NULL, (const char *)src, (uint32_t)len);
  TSNode root = ts_tree_root_node(t);
  if (ts_node_has_error(root)) { ts_tree_delete(t); return -1; }
  TSTreeCursor cur = ts_tree_cursor_new(root);
  size_t scap = 4096, depth = 0; int32_t *stack = malloc(scap * sizeof *stack); stack[0] = -1;
  int64_t n = 0, overflow = 0;
  for (;;) {
    TSNode node = ts_tree_cursor_current_node(&cur);
    if ((uint64_t)n >= cap) { overflow = 1; break; }
    int64_t me = n++;
    out[me] = (TsasmNode){ts_node_symbol(node), ts_node_is_named(node), ts_tree_cursor_current_field_id(&cur), ts_node_start_byte(node), ts_node_end_byte(node), stack[depth]};
    if (ts_tree_cursor_goto_first_child(&cur)) {
      if (++depth == scap) stack = realloc(stack, (scap *= 2) * sizeof *stack);
      stack[depth] = (int32_t)me;
      continue;
    }
    while (!ts_tree_cursor_goto_next_sibling(&cur)) {
      if (!ts_tree_cursor_goto_parent(&cur)) goto done;
      depth--;
    }
  }
done:
  free(stack);
  ts_tree_cursor_delete(&cur);
  ts_tree_delete(t);
  return overflow ? -2 : n;
}
int64_t ref_parse_only(const TSLanguage *lang, const uint8_t *src, uint64_t len) {
  TSTree *t = ts_parser_parse_string(parser(lang), NULL, (const char *)src, (uint32_t)len);
  int e = ts_node_has_error(ts_tree_root_node(t));
  ts_tree_delete(t);
  return e ? -1 : 0;
}
