// Stress suite for libtsasm.a (reentrant API in tsasm.h).
//
//   build/stress [-smoke] [-threads N] [-secs S] [-seed X] [-only SUBSTR] [-files LIST]
//   build/stress -list                 list the case names
//   build/stress -dump NAME            write a generated input to build/stress-inputs/NAME.go
//   build/stress -mut FILE SEED VALID  replay one mutation from the threading check
//
// Three checks, each printing PASS/FAIL with numbers; the exit status is 1 if anything failed.
//  1. Pathological inputs (generated in memory here): deep nesting, flat chains, GLR-heavy
//     input, lexer extremes, a 220 MB valid file, len >= 0xFFFFFFF0 over a PROT_NONE page,
//     tiny memory limits. Every case runs in its own forked child (so a crash or a hang is
//     reported, not fatal); the parse runs on a pthread with a 128 KiB stack, so deep native
//     recursion would crash. Per case: result class, parse and wall time, peak RSS (wait4
//     rusage, includes the input buffer). A case fails on a wrong class, a malformed node
//     array, a crash, more than 60 s of wall time, or a kill at 90 s. Small instances (<= 64 KiB)
//     are also parsed by C tree-sitter (build/libref.a) to confirm the expected class. A -3 at
//     the default limit is re-run with a 6 GiB limit, which must give the expected class
//     (inputs >= 8 MB may stay -3); GLR-heavy families may return -3 at any limit.
//  2. Retention (resident memory, /proc/self; each case runs in its own process): a parse that
//     needs hundreds of MB peaks that high, gives its working buffers back before returning
//     (only the returned node array stays), and the next small parse drops the node array too, so
//     usage falls to baseline + TSASM_DEFAULT_RETAIN; again with tsasm_parser_set_retain(p, 0).
//  3. Threads: a single-threaded reference (hash of the node array, or the error code) for
//     every file in the list, then N threads for S seconds checking every parse against it,
//     with roles: persistent parser, parser per file, random memory limits, and random
//     mutations followed by a valid file on the same parser.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../tsasm.h"

typedef struct TSLanguage TSLanguage;
const TSLanguage *tree_sitter_go(void);
int64_t ref_parse_only(const TSLanguage *lang, const uint8_t *src, uint64_t len); // test/ref.c: 0 ok, -1 error
#define GO tsasm_language_go()

#define MiB (1024.0 * 1024.0)
#define SMALL_STACK (128 * 1024)
#define CASE_MAX_S 60.0
#define CASE_KILL_S 90.0
#define ORACLE_MAX 65536
#define CONFIRM_LIMIT (6ull << 30)
#define INPUT_DIR "build/stress-inputs"

enum { E_ACC = 1, E_SYN = 2, E_LIM = 4, E_OTHER = 8 };

static int smoke, nthreads = 20, mt_secs = -1;
static uint64_t base_seed = 1;
static const char *only, *files_path = "test/files.txt";
static const char *huge_src = "/usr/lib/go-1.22/src/strings/strings.go";
static uint64_t huge_bytes = 220000000ull;
// The parser charges buffer capacity (about 2x the pages it touches) to its limit, so the raised
// limit sits far above the 8 GB cgroup cap; the cap is what bounds real memory.
static uint64_t huge_limit = 32ull << 30;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void sleep_ms(int ms) { struct timespec t = {ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&t, NULL); }
static uint64_t splitmix(uint64_t *s) {
  uint64_t z = (*s += 0x9e3779b97f4a7c15ull);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull; z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}
static int cls(int64_t r) { return r >= 0 ? E_ACC : r == TSASM_ERR_SYNTAX ? E_SYN : r == TSASM_ERR_LIMIT ? E_LIM : E_OTHER; }
static const char *cls_str(int m) {
  static const char *t[] = {"none", "acc", "-1", "acc|-1", "-3", "acc|-3", "-1|-3", "any"};
  return m & E_OTHER ? "other" : t[m & 7];
}
static void got_str(char *b, size_t n, int64_t r) {
  if (r >= 0) snprintf(b, n, "acc %ld", (long)r); else snprintf(b, n, "%ld", (long)r);
}
// Resident bytes, and the peak since the last reset_peak (tsasm mmaps big buffers itself, which
// malloc statistics do not see).
static double rss_bytes(void) {
  long pages = 0; FILE *f = fopen("/proc/self/statm", "r");
  if (f) { if (fscanf(f, "%*s %ld", &pages) != 1) pages = 0; fclose(f); }
  return (double)pages * sysconf(_SC_PAGESIZE);
}
static void reset_peak(void) { FILE *f = fopen("/proc/self/clear_refs", "w"); if (f) { fputs("5", f); fclose(f); } }
static double peak_bytes(void) {
  char l[256]; long kb = 0; FILE *f = fopen("/proc/self/status", "r");
  while (f && fgets(l, sizeof l, f)) sscanf(l, "VmHWM: %ld kB", &kb);
  if (f) fclose(f);
  return kb * 1024.0;
}

// ---- input buffers: mmap-backed, so inputs never show up in malloc statistics ----
typedef struct { uint8_t *p; size_t len, cap; } Buf;
static void breserve(Buf *b, size_t extra) {
  if (b->len + extra <= b->cap) return;
  size_t nc = b->cap ? b->cap : 1 << 16;
  while (nc < b->len + extra) nc *= 2;
  void *q = b->p ? mremap(b->p, b->cap, nc, MREMAP_MAYMOVE) : mmap(NULL, nc, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (q == MAP_FAILED) { perror("mmap"); _exit(3); }
  b->p = q; b->cap = nc;
}
static void bput(Buf *b, const void *s, size_t n) { breserve(b, n); memcpy(b->p + b->len, s, n); b->len += n; }
static void bstr(Buf *b, const char *s) { bput(b, s, strlen(s)); }
static void brep(Buf *b, const char *s, long times) {
  size_t n = strlen(s); breserve(b, n * times);
  for (long i = 0; i < times; i++) { memcpy(b->p + b->len, s, n); b->len += n; }
}
static void bfill(Buf *b, int c, size_t n) { breserve(b, n); memset(b->p + b->len, c, n); b->len += n; }
static void bfree(Buf *b) { if (b->p) munmap(b->p, b->cap); memset(b, 0, sizeof *b); }

static uint8_t *read_file(const char *path, uint64_t *len) {
  FILE *f = fopen(path, "rb"); if (!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *s = malloc(n + 1);
  if (fread(s, 1, n, f) != (size_t)n) { fclose(f); free(s); return NULL; }
  fclose(f); *len = n; return s;
}

// ---- generators (n = repeat count, or bytes for the lexer families) ----
#define PKG "package p\n"
static void g_paren(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "var x = "); bfill(b, '(', n); bstr(b, "1"); bfill(b, ')', n); bstr(b, "\n"); }
static void g_paren_open(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "var x = "); bfill(b, '(', n); bstr(b, "1"); }
static void g_block(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "func f() "); bfill(b, '{', n); bfill(b, '}', n); bstr(b, "\n"); }
static void g_block_open(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "func f() "); bfill(b, '{', n); }
static void g_slice(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "var x "); brep(b, "[]", n); bstr(b, "int\n"); }
static void g_slice_open(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "var x "); brep(b, "[]", n); }
static void g_map(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "var x "); brep(b, "map[", n); bstr(b, "int"); brep(b, "]int", n); bstr(b, "\n"); }
static void g_map_open(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "var x "); brep(b, "map[", n); }
static void g_func(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "var x "); brep(b, "func(", n); bfill(b, ')', n); bstr(b, "\n"); }
static void g_func_open(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "var x "); brep(b, "func(", n); }
static void g_lit(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "var x = "); brep(b, "T{", n); bfill(b, '}', n); bstr(b, "\n"); }
static void g_lit_open(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "var x = "); brep(b, "T{", n); }
static void g_unary(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "var x = "); brep(b, "!-^+", n); bstr(b, "x\n"); }
static void g_unary_open(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "var x = "); brep(b, "!-^+", n); }
static void g_sum(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "var x = a"); brep(b, " + a", n); bstr(b, "\n"); }
static void g_sum_open(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "var x = a"); brep(b, " + a", n); bstr(b, " +"); }
static void g_sel(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "var x = a"); brep(b, ".b", n); bstr(b, "\n"); }
static void g_call(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "var x = f"); brep(b, "()", n); bstr(b, "\n"); }
static void g_params(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "func f(a"); brep(b, ", a", n - 1); bstr(b, " int) {}\n"); }
static void g_params_open(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "func f(a"); brep(b, ", a", n - 1); }
static void g_generic(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "var x = a"); brep(b, "[b](c)", n); bstr(b, "\n"); }
static void g_generic_stmt(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "func f() {\n"); brep(b, "a[b](c)\n", n); bstr(b, "}\n"); }
static void g_typeargs(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "var x = a[b"); brep(b, ", b", n - 1); bstr(b, "](c)\n"); }
static void g_string(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "var s = \""); bfill(b, 'a', n); bstr(b, "\"\n"); }
static void g_rawstring(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "var s = `"); brep(b, "raw \"text\" \\ line\n", n / 20); bstr(b, "`\n"); }
static void g_comment(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "/*"); brep(b, "* / *x/ ", n / 8); bstr(b, "*/\n"); }
static void g_comment_open(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "/*"); brep(b, "* / *x/ ", n / 8); }
static void g_ident(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "var "); bfill(b, 'a', n); bstr(b, " int\n"); }
static void g_utf8_string(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "var x = \""); brep(b, "\xff\xfe\xc0\x80\xed\xa0\x80\xf4\x90\x80\x80", n); bstr(b, "\"\n"); }
static void g_utf8_ident(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG "var x"); brep(b, "a\xff", n); bstr(b, " int\n"); }
static void g_utf8_comment(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG); brep(b, "// \xff\xfe\xc0\x80\n", n); }
static void g_utf8_trunc(Buf *b, long n, uint64_t s) { (void)s; bstr(b, PKG); brep(b, "var y = 2\n", n); bstr(b, "var z = \"\xe2\x82"); }
static void g_nul(Buf *b, long n, uint64_t s) { (void)s; bfill(b, 0, n); }
static void g_newlines(Buf *b, long n, uint64_t s) { (void)s; bfill(b, '\n', n); }
static void g_random(Buf *b, long n, uint64_t s) {
  breserve(b, n + 8);
  for (long i = 0; i < n; i += 8) { uint64_t r = splitmix(&s); memcpy(b->p + b->len + i, &r, 8); }
  b->len += n;
}
static const char *SOUP[] = {"(", ")", "{", "}", "[", "]", ",", ";", "\n", ":=", "=", "func", "type", "var", "int",
  "a", "b", "map", "chan", "<-", "...", ".", "*", "&", "+", "if", "for", "range", "return", "go", "struct",
  "interface", "\"s\"", "`r`", "'c'", "1", "0x1p-2", "/* c */", "// c\n", "package p\n", "import \"x\"\n", "~", "|"};
static void g_soup(Buf *b, long n, uint64_t s) {
  bstr(b, PKG);
  int nt = sizeof SOUP / sizeof *SOUP;
  while ((long)b->len < n) { bstr(b, SOUP[splitmix(&s) % nt]); bstr(b, " "); }
}

typedef void (*GenFn)(Buf *, long, uint64_t);
// FEW: few nodes whatever the size, so -3 is never acceptable. GLRCAP: -3 is acceptable at any
// size, because the input forks past the 64-version cap or the work budget (tsasm.h lists both).
enum { FEW = 1, SEEDED = 2, GLRCAP = 4 };
typedef struct { const char *name; GenFn gen; int expect, flags; long full[6], smoke[6]; } Family;
#define NEST {1000, 100000, 1000000, 10000000}, {1000, 1000000}
#define CHAIN {1000, 10000, 1000000, 10000000}, {1000, 1000000}
#define GLR {1000, 10000, 100000}, {1000, 10000, 100000}
#define LEXB {1000, 100000000}, {1000, 10000000}
static const Family FAMS[] = {
  {"paren", g_paren, E_ACC, 0, NEST}, {"paren_open", g_paren_open, E_SYN, 0, NEST},
  {"block", g_block, E_ACC, 0, NEST}, {"block_open", g_block_open, E_SYN, 0, NEST},
  {"slice", g_slice, E_ACC, 0, NEST}, {"slice_open", g_slice_open, E_SYN, 0, NEST},
  {"map", g_map, E_ACC, 0, NEST}, {"map_open", g_map_open, E_SYN, 0, NEST},
  {"func", g_func, E_ACC, 0, NEST}, {"func_open", g_func_open, E_SYN, 0, NEST},
  {"lit", g_lit, E_ACC, 0, NEST}, {"lit_open", g_lit_open, E_SYN, 0, NEST},
  {"unary", g_unary, E_ACC, 0, NEST}, {"unary_open", g_unary_open, E_SYN, 0, NEST},
  {"sum", g_sum, E_ACC, 0, CHAIN}, {"sum_open", g_sum_open, E_SYN, 0, CHAIN},
  {"sel", g_sel, E_ACC, 0, CHAIN}, {"call", g_call, E_ACC, 0, CHAIN},
  {"params", g_params, E_ACC, 0, GLR}, {"params_open", g_params_open, E_SYN, 0, GLR},
  {"generic", g_generic, E_ACC, 0, {1000, 100000, 1000000}, {1000, 100000}},
  {"generic_stmt", g_generic_stmt, E_ACC, 0, {1000, 100000, 1000000}, {1000, 100000}},
  {"typeargs", g_typeargs, E_ACC, 0, GLR},
  {"string", g_string, E_ACC, FEW, LEXB}, {"rawstring", g_rawstring, E_ACC, FEW, LEXB},
  {"comment", g_comment, E_ACC, FEW, LEXB}, {"comment_open", g_comment_open, E_SYN, FEW, LEXB},
  {"ident", g_ident, E_ACC, FEW, {1000, 10000000}, {1000, 10000000}},
  {"utf8_string", g_utf8_string, E_ACC, FEW, {100, 1000000}, {100, 1000000}},
  {"utf8_ident", g_utf8_ident, E_SYN, 0, {100, 1000000}, {100, 1000000}},
  {"utf8_comment", g_utf8_comment, E_ACC, 0, {100, 1000000}, {100, 1000000}},
  {"utf8_trunc", g_utf8_trunc, E_SYN, 0, {100, 1000000}, {100, 1000000}},
  {"nul", g_nul, E_SYN, 0, LEXB}, {"newlines", g_newlines, E_ACC, 0, LEXB},
  {"random", g_random, E_SYN, SEEDED, {1000, 4000, 16000, 64000, 10000000, 100000000}, {1000, 4000, 16000, 64000, 10000000}},
  {"soup", g_soup, E_SYN, SEEDED, {1000, 4000, 16000, 64000, 10000000}, {1000, 4000, 16000, 64000, 10000000}},
};
#define NFAMS ((int)(sizeof FAMS / sizeof *FAMS))
#define ORACLE_N 100 // small instance of every unseeded family checked against C tree-sitter

// ---- node array checks ----
static uint64_t hash_nodes(const TsasmNode *a, int64_t n) {
  uint64_t h = 1469598103934665603ull ^ (uint64_t)n;
  for (int64_t i = 0; i < n; i++) {
    uint64_t f[4] = {a[i].symbol | (uint64_t)a[i].named << 16, a[i].start_byte, a[i].end_byte, (uint32_t)a[i].parent};
    for (int k = 0; k < 4; k++) { h ^= f[k]; h *= 1099511628211ull; h ^= h >> 29; }
  }
  return h;
}
// Pre-order, parents before children, child byte range inside the parent's, all inside [0,len].
static const char *check_tree(const TsasmNode *a, int64_t n, uint64_t len, int64_t *maxdepth) {
  if (n < 1) return "empty node array";
  if (a[0].parent != -1) return "root parent != -1";
  int32_t *d = n <= 50000000 ? malloc(n * sizeof *d) : NULL;
  const char *err = NULL; int64_t md = 0;
  uint32_t nsym = tsasm_language_symbol_count(GO);
  if (d) d[0] = 0;
  for (int64_t i = 0; i < n && !err; i++) {
    if (a[i].start_byte > a[i].end_byte || a[i].end_byte > len) err = "node byte range outside input";
    else if (a[i].symbol >= nsym) err = "symbol id out of range";
    else if (i) {
      int32_t p = a[i].parent;
      if (p < 0 || p >= i) err = "parent index not before child";
      else if (a[i].start_byte < a[p].start_byte || a[i].end_byte > a[p].end_byte) err = "child range outside parent";
      else if (d) { d[i] = d[p] + 1; if (d[i] > md) md = d[i]; }
    }
  }
  if (maxdepth) *maxdepth = d ? md : -1;
  free(d);
  return err;
}

// ---- cases ----
enum { K_GEN, K_LEN, K_LIMITS, K_HUGE_DEFAULT, K_HUGE_RAISED, K_RETAIN, K_MT };
typedef struct { char name[48]; int kind; const Family *fam; long n; uint64_t seed; } Case;
typedef struct {
  int done, pass, expect;
  int64_t got;
  double parse_s, bytes;
  char note[240];
} Res;
typedef struct { const Case *c; Res *res; Buf in; int expect; } Ctx;

static void note(Res *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void note(Res *r, const char *fmt, ...) {
  size_t l = strlen(r->note);
  if (l >= sizeof r->note - 2) return;
  if (l) { r->note[l++] = ';'; r->note[l++] = ' '; r->note[l] = 0; }
  va_list ap; va_start(ap, fmt); vsnprintf(r->note + l, sizeof r->note - l, fmt, ap); va_end(ap);
}
static void fail(Res *r, const char *fmt, const char *arg) { r->pass = 0; note(r, fmt, arg); }

static int64_t timed_parse(Res *res, TsasmParser *p, const uint8_t *s, uint64_t n, const TsasmNode **out) {
  double t0 = now(); int64_t r = tsasm_parse(p, GO, s, n, out); res->parse_s += now() - t0; return r;
}

static void *run_gen(void *arg) {
  Ctx *x = arg; Res *res = x->res;
  TsasmParser *p = tsasm_parser_new();
  const TsasmNode *nodes = (const TsasmNode *)1;
  int64_t r = timed_parse(res, p, x->in.p, x->in.len, &nodes);
  res->got = r;
  if (!(cls(r) & x->expect)) { res->pass = 0; note(res, "WRONG CLASS: want %s", cls_str(x->expect)); }
  if (r < 0 && nodes) fail(res, "%s", "*nodes not NULL after an error");
  if (r >= 0) {
    int64_t depth; const char *e = check_tree(nodes, r, x->in.len, &depth);
    if (e) fail(res, "BAD TREE: %s", e); else note(res, "depth %ld", (long)depth);
    if (x->in.len <= (16u << 20)) { // back-to-back on the same parser, nodes == NULL
      int64_t r2 = timed_parse(res, p, x->in.p, x->in.len, NULL);
      if (r2 != r) { res->pass = 0; note(res, "REPARSE GAVE %ld", (long)r2); }
    }
  }
  tsasm_parser_free(p);
  // A -3 at the default limit must be a real budget: the same input with a 6 GiB limit (at most
  // ~6 GB touched, under the 8 GB cap) must give the family's class, or -3 again only
  // for inputs too big to confirm (>= 8 MB). GLRCAP families stay -3 at any limit (work budget).
  const Family *fm = x->c->fam;
  if (r == TSASM_ERR_LIMIT && !(fm->flags & GLRCAP)) {
    p = tsasm_parser_new();
    tsasm_parser_set_memory_limit(p, CONFIRM_LIMIT);
    int64_t r3 = timed_parse(res, p, x->in.p, x->in.len, &nodes);
    if (cls(r3) == fm->expect) {
      const char *e = r3 >= 0 ? check_tree(nodes, r3, x->in.len, NULL) : NULL;
      if (e) fail(res, "BAD TREE at 6 GiB: %s", e);
      char g[32]; got_str(g, sizeof g, r3); note(res, "at 6 GiB limit: %s", g);
    } else if (r3 == TSASM_ERR_LIMIT && x->in.len >= (8u << 20)) note(res, "-3 at 6 GiB too (input too big to confirm)");
    else { res->pass = 0; note(res, "WRONG: at 6 GiB limit gave %ld, want %s", (long)r3, cls_str(fm->expect)); }
    tsasm_parser_free(p);
  }
  return NULL;
}

static void *run_len(void *arg) {
  Ctx *x = arg; Res *res = x->res;
  size_t region = 1 << 20;
  uint8_t *none = mmap(NULL, region, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  TsasmParser *p = tsasm_parser_new();
  uint64_t lens[] = {0xFFFFFFF0ull, 0xFFFFFFFFull, 1ull << 32, 1ull << 40, UINT64_MAX};
  res->got = 0;
  for (int i = 0; i < 5; i++) {
    const TsasmNode *nodes = (const TsasmNode *)1;
    int64_t r = timed_parse(res, p, none, lens[i], &nodes);
    if (r != TSASM_ERR_LIMIT || nodes) { res->pass = 0; note(res, "len %#lx gave %ld", (unsigned long)lens[i], (long)r); res->got = r; }
  }
  if (res->pass) res->got = TSASM_ERR_LIMIT;
  static const char ok[] = "package p\nvar x = 1\n";
  int64_t r = tsasm_parse(p, GO, (const uint8_t *)ok, sizeof ok - 1, NULL);
  if (r <= 0) { res->pass = 0; note(res, "parser unusable after: %ld", (long)r); }
  note(res, "5 lengths 0xFFFFFFF0..2^64-1 -> -3, bytes untouched, then a valid parse ok");
  tsasm_parser_free(p); munmap(none, region);
  return NULL;
}

typedef struct { char path[1024]; uint64_t size; } Cand;
static int bysize(const void *a, const void *b) { uint64_t x = ((const Cand *)a)->size, y = ((const Cand *)b)->size; return x < y ? 1 : x > y ? -1 : 0; }

static void *run_limits(void *arg) {
  Ctx *x = arg; Res *res = x->res;
  FILE *fl = fopen(files_path, "r");
  if (!fl) { fail(res, "cannot open %s", files_path); return NULL; }
  Cand top[8] = {0}; char line[1024]; struct stat st;
  while (fgets(line, sizeof line, fl)) {
    line[strcspn(line, "\n")] = 0;
    if (stat(line, &st) || (uint64_t)st.st_size <= top[7].size) continue;
    snprintf(top[7].path, sizeof top[7].path, "%s", line); top[7].size = st.st_size;
    qsort(top, 8, sizeof *top, bysize);
  }
  fclose(fl);
  uint64_t lims[] = {0, 4096, 1 << 20};
  int used = 0; long checks = 0;
  for (int i = 0; i < 8 && used < 3; i++) {
    uint64_t len; uint8_t *s = read_file(top[i].path, &len);
    if (!s) continue;
    TsasmParser *ref = tsasm_parser_new(); const TsasmNode *rn;
    int64_t rr = tsasm_parse(ref, GO, s, len, &rn);
    if (rr < 0) { tsasm_parser_free(ref); free(s); continue; }
    uint64_t rh = hash_nodes(rn, rr);
    tsasm_parser_free(ref);
    used++;
    TsasmParser *p = tsasm_parser_new(); const TsasmNode *nodes;
    for (int k = 0; k < 3; k++) {
      tsasm_parser_set_memory_limit(p, lims[k]);
      nodes = (const TsasmNode *)1;
      int64_t r = timed_parse(res, p, s, len, &nodes); checks++;
      if (r != TSASM_ERR_LIMIT || nodes) { res->pass = 0; note(res, "%s limit %lu gave %ld", strrchr(top[i].path, '/') + 1, (unsigned long)lims[k], (long)r); }
    }
    tsasm_parser_set_memory_limit(p, TSASM_DEFAULT_MEMORY_LIMIT);
    int64_t r = timed_parse(res, p, s, len, &nodes); checks++;
    if (r != rr || hash_nodes(nodes, r) != rh) { res->pass = 0; note(res, "%s after raising: %ld vs ref %ld", strrchr(top[i].path, '/') + 1, (long)r, (long)rr); }
    // lower below current use (frees the retained buffers), then raise again
    tsasm_parser_set_memory_limit(p, 4096);
    r = timed_parse(res, p, s, len, &nodes); checks++;
    if (r != TSASM_ERR_LIMIT) { res->pass = 0; note(res, "lowered limit gave %ld", (long)r); }
    tsasm_parser_set_memory_limit(p, TSASM_DEFAULT_MEMORY_LIMIT);
    r = timed_parse(res, p, s, len, &nodes); checks++;
    if (r != rr || hash_nodes(nodes, r) != rh) { res->pass = 0; note(res, "re-raised: %ld vs %ld", (long)r, (long)rr); }
    r = timed_parse(res, p, s, len, NULL); checks++;
    if (r != rr) { res->pass = 0; note(res, "nodes=NULL gave %ld vs %ld", (long)r, (long)rr); }
    tsasm_parser_free(p); free(s);
    note(res, "%s %.0fKB", strrchr(top[i].path, '/') + 1, len / 1e3);
  }
  tsasm_parser_free(NULL);
  if (used < 3) { res->pass = 0; note(res, "only %d usable files", used); }
  note(res, "%ld checks: limits 0/4K/1M -> -3, then match ref after raise", checks);
  res->got = res->pass ? TSASM_ERR_LIMIT : 0;
  return NULL;
}

// Body of huge_src without its package clause and imports.
static int huge_body(Buf *body) {
  uint64_t len; uint8_t *s = read_file(huge_src, &len);
  if (!s) return -1;
  int seen_pkg = 0, in_imp = 0;
  for (uint64_t i = 0; i < len;) {
    uint64_t j = i; while (j < len && s[j] != '\n') j++;
    const char *l = (const char *)s + i; size_t ll = j - i;
    int keep = 1;
    if (!seen_pkg) { keep = 0; if (ll >= 8 && !memcmp(l, "package ", 8)) seen_pkg = 1; }
    else if (in_imp) { keep = 0; if (ll >= 1 && l[0] == ')') in_imp = 0; }
    else if (ll >= 7 && !memcmp(l, "import ", 7)) { keep = 0; if (memchr(l, '(', ll) && !memchr(l, ')', ll)) in_imp = 1; }
    if (keep) { bput(body, l, ll); bstr(body, "\n"); }
    i = j + 1;
  }
  free(s);
  return 0;
}
static void huge_input(Buf *in, const Buf *body, long k) { bstr(in, PKG); for (long i = 0; i < k; i++) bput(in, body->p, body->len); }

static void *run_huge(void *arg) {
  Ctx *x = arg; Res *res = x->res;
  Buf body = {0}; if (huge_body(&body)) { fail(res, "cannot read %s", huge_src); return NULL; }
  long k = (huge_bytes + body.len - 1) / body.len;
  Buf one = {0}, two = {0}, in = {0};
  huge_input(&one, &body, 1); huge_input(&two, &body, 2); huge_input(&in, &body, k);
  res->bytes = in.len;
  TsasmParser *p = tsasm_parser_new(); const TsasmNode *nodes;
  int64_t c1 = tsasm_parse(p, GO, one.p, one.len, NULL), c2 = tsasm_parse(p, GO, two.p, two.len, NULL);
  if (c1 <= 0 || c2 <= c1) { res->pass = 0; note(res, "base copies do not parse: %ld %ld", (long)c1, (long)c2); goto out; }
  int64_t want = c1 + (k - 1) * (c2 - c1);
  if (x->c->kind == K_HUGE_DEFAULT) {
    x->expect = E_LIM;
    nodes = (const TsasmNode *)1;
    int64_t r = res->got = timed_parse(res, p, in.p, in.len, &nodes);
    if (r != TSASM_ERR_LIMIT || nodes) { res->pass = 0; note(res, "WRONG: want -3 under the default 1 GiB limit"); }
    int64_t r1 = tsasm_parse(p, GO, one.p, one.len, NULL);
    if (r1 != c1) { res->pass = 0; note(res, "small parse after -3 gave %ld vs %ld", (long)r1, (long)c1); }
    note(res, "%ld copies of %s; would be %ld nodes; parser ok after", k, strrchr(huge_src, '/') + 1, (long)want);
  } else {
    x->expect = E_ACC | E_LIM;
    FILE *mi = fopen("/proc/meminfo", "r"); char l[256]; long avail_kb = 0;
    while (mi && fgets(l, sizeof l, mi)) sscanf(l, "MemAvailable: %ld kB", &avail_kb);
    if (mi) fclose(mi);
    if (avail_kb < 7l << 20) { res->got = 0; note(res, "SKIPPED: MemAvailable %.1f GB < 7 GB", avail_kb / 1e6); goto out; }
    tsasm_parser_set_memory_limit(p, huge_limit);
    // The full input may need more than the 2^27 tree nodes tsasm.h allows (TSASM_ERR_LIMIT); a
    // prefix of 10/11 of the copies (~200 MB) stays under it and must parse exactly.
    long k2 = k * 10 / 11;
    int64_t want2 = c1 + (k2 - 1) * (c2 - c1);
    int64_t r2 = tsasm_parse(p, GO, in.p, strlen(PKG) + k2 * body.len, NULL);
    if (r2 != want2) { res->pass = 0; note(res, "WRONG: the first %ld copies (%.0f MB) give %ld, want %ld", k2, (strlen(PKG) + k2 * body.len) / 1e6, (long)r2, (long)want2); }
    int64_t r = res->got = timed_parse(res, p, in.p, in.len, &nodes);
    if (r == TSASM_ERR_LIMIT) note(res, "full input -3 at the node cap; %ld-copy prefix parses exactly (%ld nodes)", k2, (long)r2);
    else if (r != want) { res->pass = 0; note(res, "WRONG: want %ld nodes or -3 (limit %.1f GiB)", (long)want, huge_limit / MiB / 1024); }
    else { const char *e = check_tree(nodes, r, in.len, NULL); if (e) fail(res, "BAD TREE: %s", e); }
    note(res, "limit %.1f GiB, %ld copies", huge_limit / MiB / 1024, k);
  }
out:
  tsasm_parser_free(p); bfree(&body); bfree(&one); bfree(&two); bfree(&in);
  return NULL;
}

static void *run_retain(void *arg) {
  Ctx *x = arg; Res *res = x->res;
  Buf body = {0}; if (huge_body(&body)) { fail(res, "cannot read %s", huge_src); return NULL; }
  Buf big = {0}, small = {0};
  huge_input(&big, &body, (20000000 + body.len - 1) / body.len); // ~550 MB RSS, under 1 GiB
  huge_input(&small, &body, 1);
  res->bytes = big.len;
  TsasmParser *p = tsasm_parser_new(); const TsasmNode *nodes;
  double base = rss_bytes();
  reset_peak();
  int64_t r = timed_parse(res, p, big.p, big.len, &nodes);
  double peak = peak_bytes(), after_big = rss_bytes(), out = r > 0 ? r * (double)sizeof(TsasmNode) : 0;
  int64_t r2 = timed_parse(res, p, small.p, small.len, &nodes);
  double after_small = rss_bytes();
  tsasm_parser_free(p);
  double after_free = rss_bytes();
  res->got = r;
  double slack = 4 * MiB; // malloc's own free lists keep small blocks resident
  printf("  retention (resident): baseline %.2f MiB, peak in %.0f MB parse +%.0f MiB, after it +%.1f MiB "
         "(its %.1f MiB of nodes), after small parse +%.2f MiB, after free +%.2f MiB\n",
         base / MiB, big.len / 1e6, (peak - base) / MiB, (after_big - base) / MiB, out / MiB,
         (after_small - base) / MiB, (after_free - base) / MiB);
  if (r <= 0 || r2 <= 0) { res->pass = 0; note(res, "parses failed: %ld %ld", (long)r, (long)r2); }
  if (peak - base < 200 * MiB) { res->pass = 0; note(res, "big parse peaked at only %.0f MiB, not a test", (peak - base) / MiB); }
  if (after_big - base > out + TSASM_DEFAULT_RETAIN + slack) { res->pass = 0; note(res, "KEPT %.1f MiB after the big parse beyond its nodes", (after_big - base - out) / MiB); }
  if (after_small - base > TSASM_DEFAULT_RETAIN + slack) { res->pass = 0; note(res, "RETAINED %.1f MiB after the small parse", (after_small - base) / MiB); }
  if (after_free - base > slack) { res->pass = 0; note(res, "LEAK %.1f MiB after free", (after_free - base) / MiB); }
  note(res, "resident MiB over base: peak %.0f, after big %.1f (nodes %.1f), small %.2f, freed %.2f",
       (peak - base) / MiB, (after_big - base) / MiB, out / MiB, (after_small - base) / MiB, (after_free - base) / MiB);
  // retain 0: lowering it frees the kept node array at once, and every parse keeps nothing
  TsasmParser *q = tsasm_parser_new();
  double qbase = rss_bytes();
  int64_t r3 = timed_parse(res, q, big.p, big.len, &nodes);
  double q_big = rss_bytes();
  tsasm_parser_set_retain(q, 0);
  double q_lowered = rss_bytes();
  int64_t r4 = timed_parse(res, q, small.p, small.len, &nodes);
  double q_small = rss_bytes();
  tsasm_parser_free(q);
  printf("  retain 0: after big parse +%.1f MiB, after set_retain(0) +%.2f MiB, after small parse +%.2f MiB\n",
         (q_big - qbase) / MiB, (q_lowered - qbase) / MiB, (q_small - qbase) / MiB);
  if (r3 != r || r4 != r2) { res->pass = 0; note(res, "second parser gave %ld %ld, want %ld %ld", (long)r3, (long)r4, (long)r, (long)r2); }
  if (q_lowered - qbase > slack) { res->pass = 0; note(res, "set_retain(0) left %.1f MiB", (q_lowered - qbase) / MiB); }
  if (q_small - qbase > slack) { res->pass = 0; note(res, "retain 0 kept %.1f MiB after a small parse", (q_small - qbase) / MiB); }
  bfree(&body); bfree(&big); bfree(&small);
  x->expect = E_ACC;
  return NULL;
}

// ---- check 3: threads ----
typedef struct { uint8_t *p; uint64_t len; int64_t code; uint64_t hash; const char *path; } File;
static File *F; static int NF;
static int load_files(void) {
  FILE *fl = fopen(files_path, "r"); if (!fl) return -1;
  char line[4096]; int cap = 1024; F = malloc(cap * sizeof *F);
  while (fgets(line, sizeof line, fl)) {
    line[strcspn(line, "\n")] = 0;
    uint64_t len; uint8_t *s = read_file(line, &len); if (!s) continue;
    if (NF == cap) { cap *= 2; F = realloc(F, cap * sizeof *F); }
    F[NF].p = s; F[NF].len = len; F[NF].path = strdup(line); NF++;
  }
  fclose(fl);
  return NF ? 0 : -1;
}
static void ref_one(TsasmParser *p, int i, int64_t *code, uint64_t *hash) {
  const TsasmNode *n; int64_t r = tsasm_parse(p, GO, F[i].p, F[i].len, &n);
  *code = r; *hash = r >= 0 ? hash_nodes(n, r) : 0;
}

static const char *TOKS[] = {"(", ")", "{", "}", "[", "]", ",", ";", "\n", ":=", "func ", "\"", "`", "/*", "*/", "//",
  "'", "...", "<-", "\xff", " int", "[]", "map[", "go ", "type ", ".", "\\", "0x", "1e"};
// Deterministic in (file, seed); the splice source is F[seed % NF].
static size_t mutate(int fi, uint64_t seed, uint8_t **buf, size_t *cap) {
  const File *src = &F[fi], *oth = &F[seed % NF];
  size_t need = src->len + 4 * 4200 + 16;
  if (*cap < need) { *buf = realloc(*buf, need); *cap = need; }
  uint8_t *b = *buf, tmp[4096]; size_t n = src->len;
  memcpy(b, src->p, n);
  uint64_t s = seed; int ops = 1 + splitmix(&s) % 4;
  for (int k = 0; k < ops; k++) {
    size_t pos = n ? splitmix(&s) % (n + 1) : 0, l;
    switch (splitmix(&s) % 7) {
    case 0: if (n) { b[pos % n] = splitmix(&s); } break;                   // byte flip (may be NUL)
    case 1: { const char *t = TOKS[splitmix(&s) % (sizeof TOKS / sizeof *TOKS)]; l = strlen(t);
              memmove(b + pos + l, b + pos, n - pos); memcpy(b + pos, t, l); n += l; break; }
    case 2: l = 1 + splitmix(&s) % 64; if (l > n - pos) l = n - pos;        // delete
            memmove(b + pos, b + pos + l, n - pos - l); n -= l; break;
    case 3: if (!n) break; { size_t a = splitmix(&s) % n; l = 1 + splitmix(&s) % 4096; if (l > n - a) l = n - a;
              memcpy(tmp, b + a, l); memmove(b + pos + l, b + pos, n - pos); memcpy(b + pos, tmp, l); n += l; } break; // duplicate
    case 4: n = pos; break;                                                 // truncate
    case 5: if (!oth->len) { break; }
            { size_t a = splitmix(&s) % oth->len, d;                          // splice other file
              l = 1 + splitmix(&s) % 4096;
              if (l > oth->len - a) l = oth->len - a;
              d = splitmix(&s) % 64;
              if (d > n - pos) d = n - pos;
              memmove(b + pos + l, b + pos + d, n - pos - d); memcpy(b + pos, oth->p + a, l); n = n - d + l; } break;
    case 6: for (size_t q = pos; q < n && q < pos + 256; q++)               // swap a bracket kind
              if (strchr("(){}[]", b[q]) && b[q]) { b[q] = "(){}[]"[splitmix(&s) % 6]; break; }
            break;
    }
  }
  return n;
}

enum { R_KEEP, R_FRESH, R_LIMIT, R_MUT };
static const char *ROLE[] = {"persistent", "fresh-per-file", "random-limit", "mutation"};
typedef struct {
  pthread_t th; int id; uint64_t seed;
  _Atomic int busy; _Atomic double op_start; _Atomic int cur_role, cur_file; _Atomic uint64_t cur_seed, cur_limit;
  long parses[4], mism, anomalies, limited, mut_cls[4];
  char pad[64];
} Th;
static Th *TH; static atomic_int stop; static atomic_long mism_total; static pthread_mutex_t out_mu = PTHREAD_MUTEX_INITIALIZER;
static const uint64_t LIMITS[] = {0, 4096, 65536, 1 << 20, 4 << 20, 16 << 20, 64 << 20, TSASM_DEFAULT_MEMORY_LIMIT, 4ull << 30};

static void mismatch(Th *t, int role, int f, int64_t r, uint64_t lim, uint64_t mseed, int mf, const char *why) {
  t->mism++;
  if (atomic_fetch_add(&mism_total, 1) >= 20) return;
  pthread_mutex_lock(&out_mu);
  printf("  MISMATCH thread %d role %s: %s file[%d] %s got %ld ref %ld limit %lu", t->id, ROLE[role], why, f, F[f].path, (long)r, (long)F[f].code, (unsigned long)lim);
  if (role == R_MUT) printf(" after mutation of file[%d] seed %lu; repro: build/stress -mut %d %lu %d", mf, (unsigned long)mseed, mf, (unsigned long)mseed, f);
  printf("\n"); fflush(stdout);
  pthread_mutex_unlock(&out_mu);
}
static int matches(int f, int64_t r, const TsasmNode *n) { return r == F[f].code && (r < 0 || hash_nodes(n, r) == F[f].hash); }

static void begin_op(Th *t, int role, int f, uint64_t seed, uint64_t lim) {
  t->cur_role = role; t->cur_file = f; t->cur_seed = seed; t->cur_limit = lim; t->op_start = now(); t->busy = 1;
}
static void *mt_worker(void *arg) {
  Th *t = arg; uint64_t rng = t->seed;
  TsasmParser *p = tsasm_parser_new(); uint8_t *mb = NULL; size_t mcap = 0;
  const TsasmNode *n;
  for (long op = 0; !atomic_load_explicit(&stop, memory_order_relaxed); op++) {
    int role = nthreads >= 4 ? t->id % 4 : (int)((t->id + op) % 4);
    int f = splitmix(&rng) % NF;
    int64_t r;
    switch (role) {
    case R_KEEP:
      begin_op(t, role, f, 0, TSASM_DEFAULT_MEMORY_LIMIT);
      tsasm_parser_set_memory_limit(p, TSASM_DEFAULT_MEMORY_LIMIT);
      tsasm_parser_set_retain(p, TSASM_DEFAULT_RETAIN);
      r = tsasm_parse(p, GO, F[f].p, F[f].len, &n);
      if (!matches(f, r, n)) mismatch(t, role, f, r, TSASM_DEFAULT_MEMORY_LIMIT, 0, 0, "");
      break;
    case R_FRESH: {
      begin_op(t, role, f, 0, TSASM_DEFAULT_MEMORY_LIMIT);
      TsasmParser *q = tsasm_parser_new();
      r = tsasm_parse(q, GO, F[f].p, F[f].len, &n);
      if (!matches(f, r, n)) mismatch(t, role, f, r, TSASM_DEFAULT_MEMORY_LIMIT, 0, 0, "");
      tsasm_parser_free(q);
      break; }
    case R_LIMIT: {
      uint64_t lim = LIMITS[splitmix(&rng) % (sizeof LIMITS / sizeof *LIMITS)];
      if (lim > 1 << 20 && lim < TSASM_DEFAULT_MEMORY_LIMIT) lim += splitmix(&rng) % lim; // vary within the band
      begin_op(t, role, f, 0, lim);
      tsasm_parser_set_memory_limit(p, lim);
      static const uint64_t RETAINS[] = {0, 4096, 1 << 20, 8 << 20, TSASM_DEFAULT_RETAIN, 1ull << 40};
      tsasm_parser_set_retain(p, RETAINS[splitmix(&rng) % (sizeof RETAINS / sizeof *RETAINS)]);
      r = tsasm_parse(p, GO, F[f].p, F[f].len, &n);
      if (r == TSASM_ERR_LIMIT && r != F[f].code) {
        if (lim < TSASM_DEFAULT_MEMORY_LIMIT) t->limited++; else mismatch(t, role, f, r, lim, 0, 0, "-3 at a limit >= default");
      } else if (!matches(f, r, n)) mismatch(t, role, f, r, lim, 0, 0, "");
      break; }
    case R_MUT: {
      uint64_t ms = splitmix(&rng);
      begin_op(t, role, f, ms, TSASM_DEFAULT_MEMORY_LIMIT);
      tsasm_parser_set_memory_limit(p, TSASM_DEFAULT_MEMORY_LIMIT);
      size_t ml = mutate(f, ms, &mb, &mcap);
      r = tsasm_parse(p, GO, mb, ml, &n);
      t->mut_cls[cls(r) == E_ACC ? 0 : cls(r) == E_SYN ? 1 : cls(r) == E_LIM ? 2 : 3]++;
      if (cls(r) == E_OTHER || (r < 0 && n)) { t->anomalies++; mismatch(t, role, f, r, 0, ms, f, "mutated parse returned an unexpected code"); }
      if (r >= 0) { const char *e = check_tree(n, r, ml, NULL); if (e) { t->anomalies++; mismatch(t, role, f, r, 0, ms, f, e); } }
      t->parses[role]++;
      int g = splitmix(&rng) % NF;
      begin_op(t, role, g, ms, TSASM_DEFAULT_MEMORY_LIMIT);
      r = tsasm_parse(p, GO, F[g].p, F[g].len, &n);
      if (!matches(g, r, n)) mismatch(t, role, g, r, TSASM_DEFAULT_MEMORY_LIMIT, ms, f, "valid file after a mutated one");
      break; }
    }
    t->busy = 0;
    t->parses[role]++;
  }
  tsasm_parser_free(p); free(mb);
  return NULL;
}

static void crash_report(int sig) {
  char b[512];
  int l = snprintf(b, sizeof b, "\n  CRASH: signal %d in the threading check; ops in flight:\n", sig);
  if (write(1, b, l) < 0) return;
  for (int i = 0; TH && i < nthreads; i++) if (TH[i].busy) {
    l = snprintf(b, sizeof b, "    thread %d role %s file[%d] %s seed %lu limit %lu\n", i, ROLE[TH[i].cur_role], TH[i].cur_file,
                 F[TH[i].cur_file].path, (unsigned long)TH[i].cur_seed, (unsigned long)TH[i].cur_limit);
    if (write(1, b, l) < 0) break;
  }
  signal(sig, SIG_DFL); raise(sig);
}

static void mt_main(Res *res) {
  double t0 = now();
  if (load_files()) { fail(res, "cannot load %s", files_path); return; }
  uint64_t total = 0; for (int i = 0; i < NF; i++) total += F[i].len;
  res->bytes = total;
  TsasmParser *p = tsasm_parser_new();
  long acc = 0, syn = 0, lim = 0;
  for (int i = 0; i < NF; i++) {
    ref_one(p, i, &F[i].code, &F[i].hash);
    acc += F[i].code >= 0; syn += F[i].code == TSASM_ERR_SYNTAX; lim += F[i].code == TSASM_ERR_LIMIT;
  }
  tsasm_parser_free(p);
  double tref = now() - t0;
  // determinism: a fresh parser, reverse order
  p = tsasm_parser_new(); long rep_mism = 0;
  for (int i = NF - 1; i >= 0; i--) { int64_t c; uint64_t h; ref_one(p, i, &c, &h); if (c != F[i].code || h != F[i].hash) rep_mism++; }
  tsasm_parser_free(p);
  printf("  reference: %d files %.1f MB, acc %ld, -1 %ld, -3 %ld, %.1f s; repeat pass (fresh parser, reverse order) mismatches %ld\n",
         NF, total / 1e6, acc, syn, lim, tref, rep_mism);
  fflush(stdout);
  signal(SIGSEGV, crash_report); signal(SIGBUS, crash_report); signal(SIGABRT, crash_report); signal(SIGILL, crash_report); signal(SIGFPE, crash_report);
  TH = calloc(nthreads, sizeof *TH);
  pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, SMALL_STACK);
  double t1 = now();
  for (int i = 0; i < nthreads; i++) { TH[i].id = i; TH[i].seed = base_seed * 1000003 + i; pthread_create(&TH[i].th, &a, mt_worker, &TH[i]); }
  int hung = -1;
  while (now() - t1 < mt_secs && hung < 0) {
    sleep_ms(200);
    for (int i = 0; i < nthreads; i++) if (TH[i].busy && now() - TH[i].op_start > CASE_MAX_S) hung = i;
  }
  if (hung >= 0) {
    Th *t = &TH[hung];
    printf("  HANG: thread %d role %s stuck > %.0f s on file[%d] %s seed %lu limit %lu\n", hung, ROLE[t->cur_role], CASE_MAX_S,
           t->cur_file, F[t->cur_file].path, (unsigned long)t->cur_seed, (unsigned long)t->cur_limit);
    fflush(stdout); _exit(1);
  }
  atomic_store(&stop, 1);
  for (int i = 0; i < nthreads; i++) pthread_join(TH[i].th, NULL);
  double dur = now() - t1;
  long parses[4] = {0}, mism = 0, anom = 0, limited = 0, mc[4] = {0};
  for (int i = 0; i < nthreads; i++) {
    for (int k = 0; k < 4; k++) { parses[k] += TH[i].parses[k]; mc[k] += TH[i].mut_cls[k]; }
    mism += TH[i].mism; anom += TH[i].anomalies; limited += TH[i].limited;
  }
  long all = parses[0] + parses[1] + parses[2] + parses[3];
  printf("  threads %d for %.1f s: %ld parses (persistent %ld, fresh-per-file %ld, random-limit %ld [%ld hit -3], mutation+valid %ld [mutants acc %ld, -1 %ld, -3 %ld, other %ld]), mismatches %ld, anomalies %ld\n",
         nthreads, dur, all, parses[0], parses[1], parses[2], limited, parses[3], mc[0], mc[1], mc[2], mc[3], mism, anom);
  res->got = all; res->parse_s = dur;
  if (mism || anom || rep_mism || all == 0) res->pass = 0;
  note(res, "%d threads %.0fs %ld parses, %ld mismatches, repeat-pass %ld", nthreads, dur, all, mism, rep_mism);
}

// ---- runner ----
static Case *cases; static int ncases;
static Case *add_case(int kind, const char *name) {
  cases = realloc(cases, (ncases + 1) * sizeof *cases);
  Case *c = &cases[ncases++]; memset(c, 0, sizeof *c); c->kind = kind;
  snprintf(c->name, sizeof c->name, "%s", name);
  return c;
}
static void fmt_n(char *b, size_t sz, long n) {
  int e = 0; long m = n; while (m >= 10 && m % 10 == 0) { m /= 10; e++; }
  if (e >= 3) snprintf(b, sz, "%lde%d", m, e); else snprintf(b, sz, "%ld", n);
}
static void build_cases(void) {
  for (int f = 0; f < NFAMS; f++) {
    const long *sz = smoke ? FAMS[f].smoke : FAMS[f].full;
    for (int i = 0; i < 6 && sz[i]; i++) {
      char nb[32], name[48]; fmt_n(nb, sizeof nb, sz[i]);
      snprintf(name, sizeof name, "%s-%s", FAMS[f].name, nb);
      Case *c = add_case(K_GEN, name); c->fam = &FAMS[f]; c->n = sz[i]; c->seed = base_seed ^ (uint64_t)sz[i] * 0x9e37ull;
    }
  }
  add_case(K_LEN, "len-4GiB-protnone");
  add_case(K_LIMITS, "tiny-limits");
  add_case(K_HUGE_DEFAULT, "huge-220MB-default");
  if (!smoke) add_case(K_HUGE_RAISED, "huge-220MB-raised");
  add_case(K_RETAIN, "retention");
  add_case(K_MT, "threads");
}

static void child_main(const Case *c, Res *res) {
  Ctx x = {.c = c, .res = res};
  res->pass = 1;
  if (c->kind == K_MT) { mt_main(res); res->done = 1; return; }
  void *(*fn)(void *) = c->kind == K_GEN ? run_gen : c->kind == K_LEN ? run_len : c->kind == K_LIMITS ? run_limits : c->kind == K_RETAIN ? run_retain : run_huge;
  if (c->kind == K_GEN) {
    const Family *fm = c->fam;
    fm->gen(&x.in, c->n, c->seed);
    res->bytes = x.in.len;
    x.expect = fm->expect;
    if (!(fm->flags & FEW)) x.expect |= E_LIM; // a -3 is then confirmed in run_gen
    // C tree-sitter (on this 8 MB main stack) confirms the expected class on a small instance
    Buf ob = {0}; const Buf *o = &x.in;
    if (!(fm->flags & SEEDED)) { fm->gen(&ob, ORACLE_N, c->seed); o = &ob; }
    if (o->len <= ORACLE_MAX) {
      int oc = ref_parse_only(tree_sitter_go(), o->p, o->len) == 0 ? E_ACC : E_SYN;
      if (fm->flags & SEEDED) x.expect = oc | (x.expect & E_LIM);
      else if (oc != fm->expect) { res->pass = 0; note(res, "C tree-sitter says %s for n=%d, table says %s", cls_str(oc), ORACLE_N, cls_str(fm->expect)); }
    }
    bfree(&ob);
  }
  pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, SMALL_STACK);
  pthread_t th;
  if (pthread_create(&th, &a, fn, &x)) { fail(res, "%s", "pthread_create failed"); return; }
  pthread_join(th, NULL);
  if (!res->expect) res->expect = x.expect;
  bfree(&x.in);
  res->done = 1;
}

static int match_only(const char *name) { return !only || strstr(name, only); }

int main(int argc, char **argv) {
  const char *dump = NULL; int list = 0, mut = 0; long mut_f = 0, mut_v = 0; uint64_t mut_s = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-smoke")) smoke = 1;
    else if (!strcmp(argv[i], "-threads") && i + 1 < argc) nthreads = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-secs") && i + 1 < argc) mt_secs = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-seed") && i + 1 < argc) base_seed = strtoull(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "-only") && i + 1 < argc) only = argv[++i];
    else if (!strcmp(argv[i], "-files") && i + 1 < argc) files_path = argv[++i];
    else if (!strcmp(argv[i], "-huge-src") && i + 1 < argc) huge_src = argv[++i];
    else if (!strcmp(argv[i], "-huge-limit") && i + 1 < argc) huge_limit = strtoull(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "-dump") && i + 1 < argc) dump = argv[++i];
    else if (!strcmp(argv[i], "-list")) list = 1;
    else if (!strcmp(argv[i], "-mut") && i + 3 < argc) { mut = 1; mut_f = atol(argv[++i]); mut_s = strtoull(argv[++i], NULL, 0); mut_v = atol(argv[++i]); }
    else { fprintf(stderr, "unknown or incomplete option %s (see the comment at the top of test/stress.c)\n", argv[i]); return 2; }
  }
  if (mt_secs < 0) mt_secs = smoke ? 10 : 60;
  if (nthreads < 1) nthreads = 1;
  setvbuf(stdout, NULL, _IOLBF, 0);

  if (mut) { // replay one mutation from the threading check
    if (load_files() || mut_f < 0 || mut_f >= NF || mut_v < 0 || mut_v >= NF) { fprintf(stderr, "bad file list or index\n"); return 2; }
    uint8_t *mb = NULL; size_t mc = 0, ml = mutate(mut_f, mut_s, &mb, &mc);
    char path[256]; mkdir(INPUT_DIR, 0755); snprintf(path, sizeof path, INPUT_DIR "/mut-%ld-%lu.go", mut_f, (unsigned long)mut_s);
    FILE *o = fopen(path, "wb"); if (o) { fwrite(mb, 1, ml, o); fclose(o); }
    TsasmParser *ref = tsasm_parser_new(), *p = tsasm_parser_new(); int64_t rc; uint64_t rh;
    ref_one(ref, mut_v, &rc, &rh);
    const TsasmNode *n; int64_t r1 = tsasm_parse(p, GO, mb, ml, &n);
    const char *e = r1 >= 0 ? check_tree(n, r1, ml, NULL) : NULL;
    int64_t r2 = tsasm_parse(p, GO, F[mut_v].p, F[mut_v].len, &n);
    int ok = r2 == rc && (r2 < 0 || hash_nodes(n, r2) == rh) && !e && cls(r1) != E_OTHER;
    printf("mutant %s (%zu bytes): %ld%s%s; then %s: %ld vs ref %ld -> %s\n", path, ml, (long)r1, e ? " BAD TREE: " : "", e ? e : "",
           F[mut_v].path, (long)r2, (long)rc, ok ? "PASS" : "FAIL");
    return !ok;
  }

  build_cases();
  if (list) { for (int i = 0; i < ncases; i++) printf("%s\n", cases[i].name); return 0; }
  if (dump) {
    for (int i = 0; i < ncases; i++) if (!strcmp(cases[i].name, dump) && cases[i].kind == K_GEN) {
      Buf b = {0}; cases[i].fam->gen(&b, cases[i].n, cases[i].seed);
      char path[256]; mkdir("build", 0755); mkdir(INPUT_DIR, 0755); snprintf(path, sizeof path, INPUT_DIR "/%s.go", dump);
      FILE *o = fopen(path, "wb"); if (!o || fwrite(b.p, 1, b.len, o) != b.len) { perror(path); return 1; }
      fclose(o); printf("wrote %s (%zu bytes)\n", path, b.len); return 0;
    }
    fprintf(stderr, "no generated case named %s (try -list)\n", dump); return 2;
  }

  Res *R = mmap(NULL, ncases * sizeof(Res), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  printf("tsasm stress (%s): seed %lu, files %s, threads %d x %d s\n", smoke ? "smoke" : "full", (unsigned long)base_seed, files_path, nthreads, mt_secs);
  printf("%-26s %10s %7s %-14s %8s %7s %8s  %s\n", "case", "MB", "expect", "got", "parse_s", "wall_s", "maxRSS", "result / notes");
  int fails = 0, ran = 0, patho_fail = 0, patho_ran = 0;
  double tstart = now();
  for (int i = 0; i < ncases; i++) {
    Case *c = &cases[i]; Res *res = &R[i];
    if (!match_only(c->name)) continue;
    memset(res, 0, sizeof *res);
    if (c->kind == K_RETAIN) printf("== check 2: retention ==\n");
    if (c->kind == K_MT) printf("== check 3: threads and determinism ==\n");
    double t0 = now(), kill_after = c->kind == K_MT ? mt_secs + 600 : CASE_KILL_S;
    pid_t pid = fork();
    if (pid == 0) { child_main(c, res); fflush(stdout); _exit(0); }
    struct rusage ru; int st = 0, killed = 0;
    for (;;) {
      pid_t w = wait4(pid, &st, WNOHANG, &ru);
      if (w == pid) break;
      if (w < 0 && errno != EINTR) { perror("wait4"); return 2; }
      if (!killed && now() - t0 > kill_after) { kill(pid, SIGKILL); killed = 1; }
      sleep_ms(10);
    }
    double wall = now() - t0;
    int pass = !killed && WIFEXITED(st) && WEXITSTATUS(st) == 0 && res->done && res->pass;
    if (killed) note(res, "HANG: killed after %.0f s", kill_after);
    else if (WIFSIGNALED(st)) note(res, "CRASH: signal %d%s", WTERMSIG(st), WTERMSIG(st) == SIGKILL ? " (OOM kill?)" : "");
    else if (WIFEXITED(st) && WEXITSTATUS(st)) note(res, "child exit %d", WEXITSTATUS(st));
    if (c->kind != K_MT && wall > CASE_MAX_S) { pass = 0; note(res, "TOO SLOW: %.1f s > %.0f s", wall, CASE_MAX_S); }
    char got[32]; got_str(got, sizeof got, res->got);
    if (!res->done) snprintf(got, sizeof got, "-");
    if (c->kind == K_MT) snprintf(got, sizeof got, "%ld parses", (long)res->got);
    printf("%-26s %10.2f %7s %-14s %8.3f %7.2f %8.1f  %s %s\n", c->name, res->bytes / 1e6, c->kind == K_MT ? "-" : cls_str(res->expect), got,
           res->parse_s, wall, ru.ru_maxrss / 1024.0, pass ? "PASS" : "FAIL", res->note);
    if (!pass && (c->kind == K_GEN)) printf("    repro: build/stress -only %s   (input: build/stress -dump %s)\n", c->name, c->name);
    else if (!pass) printf("    repro: build/stress -only %s -seed %lu -threads %d -secs %d\n", c->name, (unsigned long)base_seed, nthreads, mt_secs);
    ran++; fails += !pass;
    if (c->kind != K_RETAIN && c->kind != K_MT) { patho_ran++; patho_fail += !pass; }
  }
  printf("== summary: check 1 pathological %d/%d pass; %d/%d checks pass overall in %.0f s: %s ==\n", patho_ran - patho_fail, patho_ran,
         ran - fails, ran, now() - tstart, fails ? "FAIL" : "PASS");
  return fails ? 1 : 0;
}
