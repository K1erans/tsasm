// Dev tool: where a parse's buffer memory goes. Parses each file with retention off the table
// (retain 2^40), then reads the context's buffer counters at the offsets in src/parser.s.
// usage: memprof <lang> <filelist> [maxfiles]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "tsasm.h"
#include "langs.h"
static uint32_t u32(void *p, int off) { return *(uint32_t *)((char *)p + off); }
int main(int argc, char **argv) {
  const TestLang *L = test_lang(argv[1]);
  FILE *fl = fopen(argv[2], "r"); int max = argc > 3 ? atoi(argv[3]) : 1 << 30;
  char path[4096]; int n = 0;
  // name, count offset, capacity offset, entry size
  struct { const char *name; int cnt, cap, size; } B[] = {
    {"trees", 8, 12, 4}, {"kids", 24, 28, 4}, {"out", -1, 100, 16}, {"estk", -1, 116, 16},
    {"stack", 328, 332, 24}, {"ext", 344, 348, 64}, {"heads", -1, 364, 8}, {"slices", -1, 380, 16},
    {"cells", -1, 396, 8}, {"slk", -1, 412, 4}};
  enum { NB = sizeof B / sizeof *B };
  double used[NB] = {0}, cap[NB] = {0}, bytes = 0, outn = 0, reach = 0, reachk = 0, leaves = 0, total = 0;
  double reachu = 0, kidsall = 0; double maxcap = 0; long maxlen = 0; char maxpath[4096] = "";
  uint8_t *mark = NULL; uint32_t *stk = NULL; size_t mcap = 0;
  while (n < max && fgets(path, sizeof path, fl)) {
    path[strcspn(path, "\n")] = 0;
    FILE *f = fopen(path, "rb"); if (!f) continue;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *src = malloc(len + 1); if (fread(src, 1, len, f) != (size_t)len) { fclose(f); free(src); continue; } fclose(f);
    TsasmParser *p = tsasm_parser_new(); tsasm_parser_set_retain(p, 1ull << 40);
    const TsasmNode *nodes; int64_t r = tsasm_parse(p, L->tsasm(), src, len, &nodes);
    if (r > 0) {
      uint32_t nn = u32(p, 8), root = u32(p, 52);
      char *tr = *(char **)p; uint32_t *kids = *(uint32_t **)((char *)p + 16);
      if (nn > mcap) { mcap = nn; mark = realloc(mark, mcap); stk = realloc(stk, mcap * 4); }
      memset(mark, 0, nn); int sp = 0; stk[sp++] = root; mark[root] = 1;
      while (sp) {
        uint32_t t = stk[--sp]; reach++;
        uint32_t cc = *(uint32_t *)(tr + 4 * (size_t)t + 12), ci = *(uint32_t *)(tr + 4 * (size_t)t + 16);
        if (!cc) leaves++;
        reachu += cc ? 6 : 4;
        reachk += cc;
        for (uint32_t j = 0; j < cc; j++) { uint32_t c = kids[ci + j]; if (!mark[c]) { mark[c] = 1; stk[sp++] = c; } }
      }
      total += nn; kidsall += u32(p, 24);
      n++; bytes += len; outn += r;
      double fc = 0;
      for (int i = 0; i < NB; i++) fc += (double)u32(p, B[i].cap) * B[i].size;
      if (fc > maxcap) { maxcap = fc; maxlen = len; snprintf(maxpath, sizeof maxpath, "%s", path); }
      for (int i = 0; i < NB; i++) {
        cap[i] += (double)u32(p, B[i].cap) * B[i].size;
        used[i] += B[i].cnt >= 0 ? (double)u32(p, B[i].cnt) * B[i].size : B[i].name[0] == 'o' ? r * 16.0 : 0;
      }
    }
    tsasm_parser_free(p); free(src);
  }
  printf("%d files, %.1f MB, %.3f visible nodes per byte\n", n, bytes / 1e6, outn / bytes);
  printf("%-8s %12s %12s\n", "buffer", "used B/byte", "cap B/byte");
  double tu = 0, tc = 0;
  for (int i = 0; i < NB; i++) { printf("%-8s %12.2f %12.2f\n", B[i].name, used[i] / bytes, cap[i] / bytes); tu += used[i]; tc += cap[i]; }
  printf("%-8s %12.2f %12.2f\n", "total", tu / bytes, tc / bytes);
  printf("dead: %.1f%% of the tree arena (%.2f bytes per input byte), %.1f%% of kids (%.2f)\n", 100 * (1 - reachu / total), (total - reachu) * 4 / bytes, 100 * (1 - reachk / kidsall), (kidsall - reachk) * 4 / bytes);
  printf("most memory: %.1f MB of buffers for %.2f MB, %s\n", maxcap / 1e6, maxlen / 1e6, maxpath);
  printf("trees: %.3f reachable from the root per byte, %.3f of them leaves; arena %.2f bytes per byte; reachable kids %.3f per byte\n",
         reach / bytes, leaves / bytes, total * 4 / bytes, reachk / bytes);
}
