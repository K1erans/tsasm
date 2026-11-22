// Print the first 200 nodes tsasm produces for one file. usage: one <lang> <file>
#include "langs.h"
int main(int c, char **v) {
  if (c < 3) { fprintf(stderr, "usage: one <lang> <file>\n"); return 2; }
  const TsasmLanguage *lang = test_lang(v[1])->tsasm();
  FILE *f = fopen(v[2], "rb"); if (!f) return 1;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  unsigned char *s = malloc(n + 1); if (fread(s, 1, n, f) != (size_t)n) return 1;
  TsasmParser *p = tsasm_parser_new();
  const TsasmNode *out;
  int64_t r = tsasm_parse(p, lang, s, n, &out);
  printf("result %ld\n", (long)r);
  for (int64_t i = 0; i < r && i < 200; i++) printf("%ld %s n%d [%u,%u] p%d\n", (long)i, tsasm_language_symbol_name(lang, out[i].symbol), out[i].named, out[i].start_byte, out[i].end_byte, out[i].parent);
  tsasm_parser_free(p);
}
