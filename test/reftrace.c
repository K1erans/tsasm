#include <stdio.h>
#include <stdlib.h>
#include "tree_sitter/api.h"
const TSLanguage *tree_sitter_go(void);
static void lg(void *p, TSLogType t, const char *m) { (void)p; if (t == TSLogTypeParse) printf("%s\n", m); }
int main(int c, char **v) {
  FILE *f = fopen(v[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  char *s = malloc(n); fread(s, 1, n, f);
  TSParser *p = ts_parser_new(); ts_parser_set_language(p, tree_sitter_go());
  ts_parser_set_logger(p, (TSLogger){0, lg});
  TSTree *t = ts_parser_parse_string(p, 0, s, n);
  char *str = ts_node_string(ts_tree_root_node(t)); printf("%s\n", str);
}
