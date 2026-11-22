#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../tsasm.h"
int64_t ref_parse_go(const uint8_t *src, uint64_t len, TsasmNode *out, uint64_t cap, int use_cursor);
int main(int argc, char **argv) {
  FILE *fl = fopen(argv[1], "r"); char path[4096]; int files = 0; long bad = 0, ok = 0, agree = 0, disagree = 0;
  TsasmNode *a = malloc(16 << 22), *b = malloc(16 << 22);
  srand(1);
  while (fgets(path, sizeof path, fl)) {
    path[strcspn(path, "\n")] = 0; FILE *f = fopen(path, "rb"); if (!f) continue;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *s = malloc(len + 1); if (fread(s, 1, len, f) != (size_t)len) return 1; fclose(f);
    for (int k = 0; k < 6; k++) {
      long n = len; uint8_t *t = malloc(len + 1); memcpy(t, s, len);
      if (k == 0) n = 0;
      else if (k == 1) n = len ? rand() % len : 0;                    // truncation
      else if (k == 2) { for (int m = 0; m < 3 && len; m++) t[rand() % len] = rand(); }  // byte flips
      else if (k == 3) { for (long m = 0; m < len; m++) t[m] = rand(); }                  // garbage
      else if (k == 4) { int64_t r = tsasm_parse_go(t, n, a, 3); if (r != -2 && r != -1) { printf("cap3 gave %ld %s\n", (long)r, path); } free(t); continue; }
      else if (k == 5) { for (long m = 0; m < len; m++) if (t[m] == '\n') t[m] = ' '; }  // no newlines
      int64_t r1 = tsasm_parse_go(t, n, a, 1 << 22), r2 = ref_parse_go(t, n, b, 1 << 22, 1);
      if (r1 < 0) bad++; else ok++;
      if (r1 >= 0 && r2 >= 0) { if (r1 == r2 && !memcmp(a, b, r1 * 16)) agree++; else disagree++; }
      if (r1 >= 0 && r2 < 0) printf("asm accepts, C rejects: k=%d %s\n", k, path);
      free(t);
    }
    free(s); files++;
  }
  printf("files=%d variants: rejected=%ld accepted=%ld agree=%ld disagree=%ld\n", files, bad, ok, agree, disagree);
}
