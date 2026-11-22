#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../tsasm.h"
int64_t ref_parse_go(const uint8_t *src, uint64_t len, TsasmNode *out, uint64_t cap, int use_cursor);
int64_t ref_parse_only(const uint8_t *src, uint64_t len);
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
  TsasmParser *p = tsasm_parser_new(); const TsasmNode *tree;
  double best[3] = {1e9, 1e9, 1e9}; long nodes[3] = {0};
  for (int it = 0; it < iters; it++) {
    double t0 = now(); long c = 0;
    for (int i = 0; i < n; i++) c += tsasm_parse_go(p, srcs[i], lens[i], &tree);
    double t1 = now(); if (t1 - t0 < best[0]) best[0] = t1 - t0; nodes[0] = c;
    c = 0; for (int i = 0; i < n; i++) c += ref_parse_only(srcs[i], lens[i]);
    double t2 = now(); if (t2 - t1 < best[1]) best[1] = t2 - t1;
    c = 0; for (int i = 0; i < n; i++) c += ref_parse_go(srcs[i], lens[i], out, cap, 1);
    double t3 = now(); if (t3 - t2 < best[2]) best[2] = t3 - t2; nodes[2] = c;
  }
  printf("files=%d bytes=%.1fMB\n", n, total / 1e6);
  printf("tsasm (parse+emit)        %.3fs  %.1f MB/s\n", best[0], total / 1e6 / best[0]);
  printf("C ts  (parse only)        %.3fs  %.1f MB/s\n", best[1], total / 1e6 / best[1]);
  printf("C ts  (parse+cursor walk) %.3fs  %.1f MB/s\n", best[2], total / 1e6 / best[2]);
}
