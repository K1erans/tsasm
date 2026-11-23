// Regression test for BigParseOutputDropped (src/parser.s:2984-2993 and 3147-3160).
//
// Build and run from the repository root:
//   make libtsasm.a
//   gcc -O2 -I. verification/parser-memory/repro.c libtsasm.a -o build/repro_retain && ./build/repro_retain
//
// Verifies the 5-step scenario found by TLC on ParserMemory.tla:
//   1. ParseStartLarge: start Parse 1 on a 1.73 MB Go input under the default 32 MiB retain cap.
//   2. GrowWorkMapMin:  working buffers (C_NODES, C_KIDS, C_SN, ...) grow to ~48 MiB (>= MAP_MIN).
//   3. EmitLargeOut:    p_emit grows C_OUT to 16 MiB (1,048,576 TsasmNode entries), total C_MEMUSED = 64 MiB > 32 MiB.
//   4. MainRetOk:       at .Lmain_ret (src/parser.s:3148-3156), C_MEMUSED (64 MiB) > C_RETAIN (32 MiB), so
//                       p_release_work frees all 9 working buffers (C_NCAP = 0) and sets C_MEMUSED = C_OUTCAP << 4 = 16 MiB.
//   5. ParseStartSmall: start Parse 2 on a 20-byte Go input ("package p\nvar x = 1\n").
//                       At tsasm_parse entry (src/parser.s:2984-2987), C_NCAP == 0 and C_OUTCAP != 0, so p_release
//                       drops Parse 1's 16 MiB C_OUT array before Parse 2 allocates its 4 KiB C_OUT buffer.
//   6. Subsequent medium parses (0.68 MB Go, 26 MiB total buffers <= 32 MiB C_RETAIN) retain their working arenas
//      instead of thrashing mmap/munmap on every call.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "tsasm.h"

static uint8_t *make_go_source(int nfuncs, size_t *out_len) {
  size_t cap = 32 + (size_t)nfuncs * 40;
  char *buf = malloc(cap);
  assert(buf);
  size_t len = (size_t)snprintf(buf, cap, "package p\n");
  for (int i = 0; i < nfuncs; i++) {
    len += (size_t)snprintf(buf + len, cap - len, "func f%d() int { return %d + 1 }\n", i, i);
  }
  *out_len = len;
  return (uint8_t *)buf;
}

int main(void) {
  size_t big_len = 0, med_len = 0;
  uint8_t *big_src = make_go_source(45000, &big_len);
  uint8_t *med_src = make_go_source(18000, &med_len);
  static const char small_src[] = "package p\nvar x = 1\n";

  TsasmParser *p = tsasm_parser_new();
  assert(p);

  const TsasmNode *nodes_big = NULL;
  int64_t n_big = tsasm_parse(p, tsasm_language_go(), big_src, big_len, &nodes_big);
  assert(n_big > 524288 && n_big <= 1048576); // C_OUT grew to 1,048,576 entries = 16 MiB
  uint32_t big_ncap = *(const uint32_t *)((const char *)p + 12);
  uint32_t big_outcap = *(const uint32_t *)((const char *)p + 100);
  assert(big_ncap == 0 && big_outcap == 1048576);

  // Step 5: Parse 2 is a 20-byte input. It must drop the 16 MiB C_OUT from Parse 1 and allocate a 256-entry (4 KiB) C_OUT.
  const TsasmNode *nodes_small = NULL;
  int64_t n_small = tsasm_parse(p, tsasm_language_go(), (const uint8_t *)small_src, sizeof(small_src) - 1, &nodes_small);
  assert(n_small == 11);
  uint32_t small_outcap = *(const uint32_t *)((const char *)p + 100);

  printf("Step 1-4 (Parse 1, %.2f MB Go): %ld visible nodes -> C_NCAP = %u, C_OUTCAP = %u (16 MiB kept for caller)\n",
         big_len / 1e6, (long)n_big, big_ncap, big_outcap);
  printf("Step 5   (Parse 2, %zu-byte Go): %ld visible nodes -> C_OUTCAP = %u (%u KiB, 16 MiB C_OUT dropped at entry!)\n",
         sizeof(small_src) - 1, (long)n_small, small_outcap, (small_outcap * 16) >> 10);
  assert(small_outcap == 256);

  // Step 6: Medium parse (26 MiB total <= 32 MiB C_RETAIN) now retains its working arenas on `p`, matching a fresh parser.
  TsasmParser *fresh = tsasm_parser_new();
  const TsasmNode *nodes_fresh = NULL;
  int64_t n_med_fresh = tsasm_parse(fresh, tsasm_language_go(), med_src, med_len, &nodes_fresh);
  assert(n_med_fresh > 0);

  const TsasmNode *nodes_med_p = NULL;
  int64_t n_med_p = tsasm_parse(p, tsasm_language_go(), med_src, med_len, &nodes_med_p);
  assert(n_med_p == n_med_fresh);

  uint32_t fresh_ncap = *(const uint32_t *)((const char *)fresh + 12);
  uint32_t fresh_outcap = *(const uint32_t *)((const char *)fresh + 100);
  uint64_t fresh_memused = *(const uint64_t *)((const char *)fresh + 144);

  uint32_t p_ncap = *(const uint32_t *)((const char *)p + 12);
  uint32_t p_outcap = *(const uint32_t *)((const char *)p + 100);
  uint64_t p_memused = *(const uint64_t *)((const char *)p + 144);

  printf("\nAfter medium parse (%.2f MB Go, %ld nodes) under default 32 MiB C_RETAIN:\n", med_len / 1e6, (long)n_med_p);
  printf("  Fresh parser:   C_NCAP = %7u (%2u MiB), C_OUTCAP = %7u (%2u MiB), C_MEMUSED = %2lu MiB\n",
         fresh_ncap, (fresh_ncap * 4) >> 20, fresh_outcap, (fresh_outcap * 16) >> 20, fresh_memused >> 20);
  printf("  Reused parser:  C_NCAP = %7u (%2u MiB), C_OUTCAP = %7u (%2u MiB), C_MEMUSED = %2lu MiB\n",
         p_ncap, (p_ncap * 4) >> 20, p_outcap, (p_outcap * 16) >> 20, p_memused >> 20);

  assert(p_ncap == fresh_ncap && p_outcap == fresh_outcap && p_memused == fresh_memused);
  printf("\nPASS: BigParseOutputDropped holds; 16 MiB C_OUT is dropped on the next parse and working arenas no longer thrash.\n");

  tsasm_parser_free(fresh);
  tsasm_parser_free(p);
  free(big_src);
  free(med_src);
  return 0;
}
