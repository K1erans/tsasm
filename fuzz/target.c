// libFuzzer target for the asm runtime (built by `make fuzz`, see the Makefile). One binary per
// grammar: FUZZ_LANG names it. Every input is parsed several ways and each must hold:
//   - the source sits against a PROT_NONE page (fuzz/guard.c), so any read past either end faults;
//   - a tree satisfies the output contract in tsasm.h (pre-order, parents, spans, symbol ids);
//   - a second parser gives the same result and bytes (no hidden state between parsers);
//   - a parse that hits an injected allocation failure returns TSASM_ERR_NOMEM or succeeds, and
//     the parser then reparses to the same result;
//   - a low memory limit gives the same result or TSASM_ERR_LIMIT;
//   - the asm never accepts what C tree-sitter rejects.
// Trees that differ from C's on known GLR choices and C-accepted inputs the asm rejects are
// counted, not fatal; the counts print at exit.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../test/langs.h"
#include "guard.h"

#define STR2(x) #x
#define STR(x) STR2(x)

static const TestLang *L;
static const TsasmLanguage *lang;
static TsasmParser *keep;               // reused across inputs, so retained buffers get exercised
static TsasmNode *ref;
static size_t refcap;
static long n_inputs, n_accept, n_syntax, n_limit, n_shape_diff, n_c_only;

static void report(void) {
  fprintf(stderr, "tsasm-fuzz %s: inputs=%ld accepted=%ld syntax=%ld limit=%ld shape_diff=%ld c_only_accept=%ld\n",
          L->name, n_inputs, n_accept, n_syntax, n_limit, n_shape_diff, n_c_only);
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
  (void)argc; (void)argv;
  L = test_lang(STR(FUZZ_LANG));
  lang = L->tsasm();
  keep = tsasm_parser_new();
  if (!keep) abort();
  atexit(report);
  return 0;
}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); abort(); } } while (0)

static void check_tree(const TsasmNode *t, int64_t n, uint64_t len) {
  uint32_t nsym = tsasm_language_symbol_count(lang);
  CHECK(n >= 1);
  CHECK(t[0].parent == -1);
  for (int64_t i = 0; i < n; i++) {
    CHECK(t[i].symbol < nsym);
    CHECK(t[i].named <= 1 && t[i].pad == 0);
    CHECK(t[i].start_byte <= t[i].end_byte && t[i].end_byte <= len);
    if (i == 0) continue;
    int32_t p = t[i].parent;
    CHECK(p >= 0 && p < i);                                     // pre-order: parent first
    CHECK(t[p].start_byte <= t[i].start_byte && t[i].end_byte <= t[p].end_byte);
    // pre-order: a node's parent is the previous node or one of its ancestors
    int32_t a = (int32_t)(i - 1);
    while (a != -1 && a != p) a = t[a].parent;
    CHECK(a == p);
  }
}

static int same(int64_t r1, const TsasmNode *a, int64_t r2, const TsasmNode *b) {
  return r1 == r2 && (r1 <= 0 || !memcmp(a, b, (size_t)r1 * sizeof *a));
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size > (1 << 20)) return 0;
  n_inputs++;
  uint8_t *src = guard_alloc(size);
  memcpy(src, data, size);
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < size; i++) h = (h ^ data[i]) * 1099511628211ull;

  // 1. the reused parser
  const TsasmNode *t1;
  int64_t r1 = tsasm_parse(keep, lang, src, size, &t1);
  CHECK(r1 > 0 || r1 == TSASM_ERR_SYNTAX || r1 == TSASM_ERR_LIMIT);
  CHECK((r1 > 0) == (t1 != NULL));
  if (r1 > 0) { check_tree(t1, r1, size); n_accept++; }
  else if (r1 == TSASM_ERR_SYNTAX) n_syntax++;
  else n_limit++;
  TsasmNode *copy = NULL;
  if (r1 > 0) { copy = malloc((size_t)r1 * sizeof *copy); memcpy(copy, t1, (size_t)r1 * sizeof *copy); }

  // 2. a fresh parser agrees byte for byte
  long a0 = guard_allocs;
  TsasmParser *p = tsasm_parser_new();
  CHECK(p);
  const TsasmNode *t2;
  int64_t r2 = tsasm_parse(p, lang, src, size, &t2);
  CHECK(same(r1, copy, r2, t2));
  long used = guard_allocs - a0;
  tsasm_parser_free(p);

  // 3. an allocation failure at a point chosen by the input, then a clean reparse
  long k = (long)(h % (uint64_t)(used + 1));
  guard_fail_at = guard_allocs + k;
  p = tsasm_parser_new();
  if (p) {
    int64_t r3 = tsasm_parse(p, lang, src, size, &t2);
    guard_fail_at = -1;
    CHECK(r3 == TSASM_ERR_NOMEM || same(r1, copy, r3, t2));
    if (r3 == TSASM_ERR_NOMEM) CHECK(t2 == NULL);
    r3 = tsasm_parse(p, lang, src, size, &t2);
    CHECK(same(r1, copy, r3, t2));
    tsasm_parser_free(p);
  }
  guard_fail_at = -1;

  // 4. a low memory limit: the same result or TSASM_ERR_LIMIT, then the limit is lifted
  uint64_t lim = (h >> 20) % (1u << 20);
  tsasm_parser_set_memory_limit(keep, lim);
  int64_t r4 = tsasm_parse(keep, lang, src, size, &t2);
  CHECK(r4 == TSASM_ERR_LIMIT || same(r1, copy, r4, t2));
  tsasm_parser_set_memory_limit(keep, TSASM_DEFAULT_MEMORY_LIMIT);

  // 5. C tree-sitter: never accept what it rejects
  int64_t rc;
  for (;;) {
    rc = ref_parse(L->ts(), src, size, ref, refcap);
    if (rc != -2) break;
    refcap = refcap ? refcap * 2 : 1 << 16;
    ref = realloc(ref, refcap * sizeof *ref);
  }
  if (r1 > 0) {
    CHECK(rc >= 0);
    if (!same(r1, copy, rc, ref)) n_shape_diff++;
  } else if (rc >= 0 && r1 == TSASM_ERR_SYNTAX) {
    n_c_only++;
  }

  free(copy);
  guard_free(src);
  return 0;
}
