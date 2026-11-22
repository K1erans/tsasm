// Mutation smoke test against C tree-sitter: empty, truncated, byte-flipped, garbage and
// newline-stripped variants of each file, plus a 4 KiB memory limit. usage: fuzz <lang> <filelist>
#include "langs.h"
int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: fuzz <lang> <filelist>\n"); return 2; }
  const TestLang *L = test_lang(argv[1]);
  const TsasmLanguage *lang = L->tsasm();
  FILE *fl = fopen(argv[2], "r"); char path[4096]; int files = 0; long bad = 0, ok = 0, agree = 0, disagree = 0;
  TsasmNode *b = malloc(16 << 22);
  TsasmParser *p = tsasm_parser_new(); const TsasmNode *an;
  srand(1);
  while (fgets(path, sizeof path, fl)) {
    path[strcspn(path, "\n")] = 0; FILE *f = fopen(path, "rb"); if (!f) continue;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *s = malloc(len + 1); if (fread(s, 1, len, f) != (size_t)len) return 1; fclose(f);
    for (int k = 0; k < 6; k++) {
      long n = len; uint8_t *t = malloc(len + 1); memcpy(t, s, len);
      if (k == 0) n = 0;
      else if (k == 1) n = len ? rand() % len : 0;
      else if (k == 2) { for (int m = 0; m < 3 && len; m++) t[rand() % len] = rand(); }
      else if (k == 3) { for (long m = 0; m < len; m++) t[m] = rand(); }
      else if (k == 4) {
        tsasm_parser_set_memory_limit(p, 4096);
        int64_t r = tsasm_parse(p, lang, t, n, &an);
        tsasm_parser_set_memory_limit(p, TSASM_DEFAULT_MEMORY_LIMIT);
        if (r != TSASM_ERR_LIMIT) printf("4 KiB limit gave %ld %s\n", (long)r, path);
        free(t); continue;
      }
      else if (k == 5) { for (long m = 0; m < len; m++) if (t[m] == '\n') t[m] = ' '; }
      int64_t r1 = tsasm_parse(p, lang, t, n, &an), r2 = ref_parse(L->ts(), t, n, b, 1 << 22);
      if (r1 < 0) bad++; else ok++;
      if (r1 >= 0 && r2 >= 0) { if (r1 == r2 && !memcmp(an, b, r1 * 16)) agree++; else disagree++; }
      if (r1 >= 0 && r2 < 0) printf("asm accepts, C rejects: k=%d %s\n", k, path);
      free(t);
    }
    free(s); files++;
  }
  printf("lang=%s files=%d variants: rejected=%ld accepted=%ld agree=%ld disagree=%ld\n", L->name, files, bad, ok, agree, disagree);
}
