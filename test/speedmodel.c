#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../tsasm.h"
int64_t model_parse_go(const uint8_t *, uint64_t, TsasmNode *, uint64_t);
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
int main(int argc, char **argv) {
  FILE *fl = fopen(argv[1], "r"); int iters = argc > 2 ? atoi(argv[2]) : 1;
  char path[4096]; int n = 0; uint8_t **srcs = malloc(8 * 8000); long *lens = malloc(8 * 8000); long total = 0;
  while (fgets(path, sizeof path, fl)) {
    path[strcspn(path, "\n")] = 0; FILE *f = fopen(path, "rb"); if (!f) continue;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    srcs[n] = malloc(len + 1); if (fread(srcs[n], 1, len, f) != (size_t)len) return 1; fclose(f); lens[n++] = len; total += len;
  }
  uint64_t cap = 1 << 22; TsasmNode *out = malloc(cap * sizeof *out);
  double best = 1e9; long c = 0;
  for (int it = 0; it < iters; it++) {
    double t0 = now(); c = 0;
    for (int i = 0; i < n; i++) c += model_parse_go(srcs[i], lens[i], out, cap);
    double t1 = now(); if (t1 - t0 < best) best = t1 - t0;
  }
  printf("model (C, asm lexer): %d files %.1fMB best %.3fs %.1f MB/s (nodes %ld)\n", n, total / 1e6, best, total / 1e6 / best, c);
}
