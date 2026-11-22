// Parity: compare tsasm against C tree-sitter on a list of files.
// usage: check_all <lang> <filelist> [verbose]
#define _GNU_SOURCE
#include "langs.h"
#ifdef PROTO
int64_t proto_parse(const TsasmLanguage *, const uint8_t *, uint64_t, const TsasmNode **);
extern uint32_t proto_stat_maxheads, proto_stat_maxiters, proto_stat_maxlinks, proto_stat_maxslices;
#define tsasm_parse(p, l, s, n, o) proto_parse(l, s, n, o)
#endif
int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: check_all <lang> <filelist> [verbose]\n"); return 2; }
  const TestLang *L = test_lang(argv[1]);
  const TsasmLanguage *lang = L->tsasm();
  FILE *fl = fopen(argv[2], "r");
  int verbose = argc > 3 ? atoi(argv[3]) : 0;
  char path[4096];
  uint64_t cap = 1 << 22;
  TsasmNode *a = malloc(cap * sizeof *a);
  TsasmParser *p = tsasm_parser_new();
  int files = 0, both_err = 0, match = 0, mismatch = 0, impl_only_err = 0, ref_only_err = 0, impl_limit = 0;
  while (fgets(path, sizeof path, fl)) {
    path[strcspn(path, "\n")] = 0;
    FILE *f = fopen(path, "rb"); if (!f) continue;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *src = malloc(len + 1); if (fread(src, 1, len, f) != (size_t)len) { fclose(f); free(src); continue; }
    fclose(f);
    files++;
    const TsasmNode *b;
    int64_t r = ref_parse(L->ts(), src, len, a, cap);
    int64_t m = tsasm_parse(p, lang, src, len, &b);
    if (m == TSASM_ERR_LIMIT) impl_limit++;
    if (r < 0 && m < 0) both_err++;
    else if (r < 0) { ref_only_err++; if (verbose) printf("REF_ERR_ONLY %s\n", path); }
    else if (m < 0) { impl_only_err++; if (verbose) printf("IMPL_ERR %s (%ld)\n", path, (long)m); }
    else {
      int ok = r == m;
      int64_t k = 0;
      for (; k < r && k < m; k++) if (memcmp(&a[k], &b[k], 16)) { ok = 0; break; }
      if (ok) match++;
      else {
        mismatch++;
        if (verbose) {
          printf("MISMATCH %s ref=%ld impl=%ld\n", path, (long)r, (long)m);
          if (k < r && k < m)
            printf("  at %ld: ref %s(%d) n%d [%u,%u] p%d | impl %s(%d) n%d [%u,%u] p%d\n", (long)k,
                   tsasm_language_symbol_name(lang, a[k].symbol), a[k].symbol, a[k].named, a[k].start_byte, a[k].end_byte, a[k].parent,
                   tsasm_language_symbol_name(lang, b[k].symbol), b[k].symbol, b[k].named, b[k].start_byte, b[k].end_byte, b[k].parent);
        }
      }
    }
    free(src);
  }
  printf("lang=%s files=%d match=%d mismatch=%d impl_only_err=%d ref_only_err=%d both_err=%d impl_limit=%d\n",
         L->name, files, match, mismatch, impl_only_err, ref_only_err, both_err, impl_limit);
#ifdef PROTO
  printf("proto max heads=%u iters=%u links=%u slices=%u\n", proto_stat_maxheads, proto_stat_maxiters, proto_stat_maxlinks, proto_stat_maxslices);
#endif
  tsasm_parser_free(p);
  return 0;
}
