#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../tsasm.h"
int main(int c, char **v) {
  FILE *f = fopen(v[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  unsigned char *s = malloc(n + 1); if (fread(s, 1, n, f) != (size_t)n) return 1;
  TsasmParser *p = tsasm_parser_new();
  const TsasmNode *out;
  int64_t r = tsasm_parse_go(p, s, n, &out);
  printf("result %ld\n", (long)r);
  for (int64_t i = 0; i < r && i < 200; i++) printf("%ld %s n%d [%u,%u] p%d\n", (long)i, tsasm_symbol_name_go(out[i].symbol), out[i].named, out[i].start_byte, out[i].end_byte, out[i].parent);
  tsasm_parser_free(p);
}
