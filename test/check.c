// Compare an implementation (model or asm) against C tree-sitter on a list of files.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../tsasm.h"
int64_t ref_parse_go(const uint8_t *src, uint64_t len, TsasmNode *out, uint64_t cap, int use_cursor);
int64_t ref_parse_only(const uint8_t *src, uint64_t len);
const char *ref_symbol_name(uint16_t s);
int64_t model_parse_go(const uint8_t *src, uint64_t len, TsasmNode *out, uint64_t cap);
extern int model_stat_forks, model_stat_merges, model_stat_maxv;
typedef int64_t (*fn)(const uint8_t *, uint64_t, TsasmNode *, uint64_t);
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
int main(int argc, char **argv) {
  // usage: check (model|asm) filelist [verbose]
  fn impl = strcmp(argv[1], "model") == 0 ? model_parse_go : tsasm_parse_go;
  FILE *fl = fopen(argv[2], "r");
  int verbose = argc > 3 ? atoi(argv[3]) : 0;
  char path[4096];
  uint64_t cap = 1 << 22;
  TsasmNode *a = malloc(cap * sizeof *a), *b = malloc(cap * sizeof *b);
  int files = 0, ref_err = 0, impl_err = 0, both_err = 0, match = 0, mismatch = 0, impl_only_err = 0, ref_only_err = 0;
  while (fgets(path, sizeof path, fl)) {
    path[strcspn(path, "\n")] = 0;
    FILE *f = fopen(path, "rb"); if (!f) continue;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *src = malloc(len + 1); fread(src, 1, len, f); fclose(f);
    files++;
    int fk0 = model_stat_forks, mg0 = model_stat_merges;
    int64_t r = ref_parse_go(src, len, a, cap, 1);
    int64_t m = impl(src, len, b, cap);
    if (r < 0 && m < 0) both_err++;
    else if (r < 0) { ref_only_err++; if (verbose) printf("REF_ERR_ONLY %s\n", path); }
    else if (m < 0) { impl_only_err++; if (verbose) printf("IMPL_ERR %s (%ld) forks=%d\n", path, (long)m, model_stat_forks - fk0); }
    else {
      int ok = r == m;
      int64_t k = 0;
      for (; k < r && k < m; k++) if (memcmp(&a[k], &b[k], 16)) { ok = 0; break; }
      if (ok) match++;
      else {
        mismatch++;
        if (verbose) {
          printf("MISMATCH %s ref=%ld impl=%ld merges=%d forks=%d\n", path, (long)r, (long)m, model_stat_merges - mg0, model_stat_forks - fk0);
          if (k < r && k < m) {
            printf("  at %ld: ref %s(%d) n%d [%u,%u] p%d | impl %s(%d) n%d [%u,%u] p%d\n", (long)k,
                   ref_symbol_name(a[k].symbol), a[k].symbol, a[k].named, a[k].start_byte, a[k].end_byte, a[k].parent,
                   ref_symbol_name(b[k].symbol), b[k].symbol, b[k].named, b[k].start_byte, b[k].end_byte, b[k].parent);
          }
        }
      }
    }
    free(src);
  }
  printf("files=%d match=%d mismatch=%d impl_only_err=%d ref_only_err=%d both_err=%d | forks=%d merges=%d maxv=%d\n",
         files, match, mismatch, impl_only_err, ref_only_err, both_err, model_stat_forks, model_stat_merges, model_stat_maxv);
  return 0;
}
