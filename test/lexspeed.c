#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
int32_t tsl_lex_c(uint32_t state, const uint8_t *p, const uint8_t *end, uint64_t out[2]);
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
int main(int argc, char **argv) {
  FILE *fl = fopen(argv[1], "r"); char path[4096]; int n = 0; uint8_t **srcs = malloc(8 * 8000); long *lens = malloc(8 * 8000); long total = 0;
  while (fgets(path, sizeof path, fl)) {
    path[strcspn(path, "\n")] = 0; FILE *f = fopen(path, "rb"); if (!f) continue;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    srcs[n] = malloc(len + 1); if (fread(srcs[n], 1, len, f) != (size_t)len) return 1; fclose(f); lens[n++] = len; total += len;
  }
  double best = 1e9; long toks = 0;
  for (int it = 0; it < 3; it++) {
    double t0 = now(); toks = 0;
    for (int i = 0; i < n; i++) {
      const uint8_t *p = srcs[i], *e = srcs[i] + lens[i]; uint64_t o[2];
      while (p < e) { int32_t s = tsl_lex_c(0, p, e, o); if (s <= 0 || (const uint8_t *)o[1] == p) { p++; continue; } p = (const uint8_t *)o[1]; toks++; }
    }
    double t1 = now(); if (t1 - t0 < best) best = t1 - t0;
  }
  printf("lex only (state 0): %ld tokens, %.3fs, %.1f MB/s, %.1f ns/token\n", toks, best, total / 1e6 / best, best * 1e9 / toks);
}
