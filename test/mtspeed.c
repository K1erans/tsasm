// Throughput of tsasm or C tree-sitter at N worker threads over a file list held in memory.
// usage: mtspeed <lang> <filelist> <asm|c> <workers> <runs> [cpu,cpu,...]
// Workers pull files largest-first from a shared counter, each with its own parser, the way
// bench/ does. One untimed warm-up pass, then <runs> timed passes. Prints each pass, the median,
// and per-pass work counters (accepted files, nodes) so a pass that did no work is visible.
// -DOLD_API builds against the baseline single-context API (1 worker only).
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "tree_sitter/api.h"
#include "langs.h"

static int nfiles, use_c, workers, runs;
static const TestLang *L;
static uint8_t **srcs;
static uint64_t *lens;
static int *order;
static atomic_int next;
static pthread_barrier_t start_bar, done_bar;
static int cpus[256], ncpus;

typedef struct { pthread_t th; int id; long accepted, nodes; char pad[64]; } Worker;
static Worker *W;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static void *work(void *arg) {
  Worker *w = arg;
  if (ncpus) {
    cpu_set_t m; CPU_ZERO(&m); CPU_SET(cpus[w->id % ncpus], &m);
    pthread_setaffinity_np(pthread_self(), sizeof m, &m);
  }
#ifdef OLD_API
  TsasmNode *out = malloc(16ul << 22);
#else
  TsasmParser *p = tsasm_parser_new();
#endif
  TSParser *tp = ts_parser_new();
  ts_parser_set_language(tp, L->ts());
#ifndef OLD_API
  const TsasmLanguage *lang = L->tsasm();
#endif
  for (int r = 0; r <= runs; r++) {
    pthread_barrier_wait(&start_bar);
    long acc = 0, nodes = 0;
    for (;;) {
      int i = atomic_fetch_add_explicit(&next, 1, memory_order_relaxed);
      if (i >= nfiles) break;
      int f = order[i];
      if (use_c) {
        TSTree *t = ts_parser_parse_string(tp, NULL, (const char *)srcs[f], (uint32_t)lens[f]);
        if (!ts_node_has_error(ts_tree_root_node(t))) acc++;
        ts_tree_delete(t);
      } else {
#ifdef OLD_API
        int64_t n = tsasm_parse_go(srcs[f], lens[f], out, 1ul << 22);
#else
        const TsasmNode *out;
        int64_t n = tsasm_parse(p, lang, srcs[f], lens[f], &out);
#endif
        if (n > 0) { acc++; nodes += n; }
      }
    }
    w->accepted = acc; w->nodes = nodes;
    pthread_barrier_wait(&done_bar);
  }
  ts_parser_delete(tp);
#ifndef OLD_API
  tsasm_parser_free(p);
#endif
  return NULL;
}

static int bylen(const void *a, const void *b) {
  uint64_t x = lens[*(const int *)a], y = lens[*(const int *)b];
  return x < y ? 1 : x > y ? -1 : 0;
}
static int dcmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

int main(int argc, char **argv) {
  if (argc < 6) { fprintf(stderr, "usage: mtspeed <lang> <filelist> <asm|c> <workers> <runs> [cpus]\n"); return 2; }
  L = test_lang(argv[1]);
  use_c = strcmp(argv[3], "c") == 0; workers = atoi(argv[4]); runs = atoi(argv[5]);
  if (workers < 1 || runs < 1) { fprintf(stderr, "mtspeed: workers and runs must be at least 1\n"); return 2; }
  if (argc > 6) for (char *t = strtok(argv[6], ","); t && ncpus < 256; t = strtok(NULL, ",")) cpus[ncpus++] = atoi(t);
  FILE *fl = fopen(argv[2], "r"); char path[4096]; int cap = 1024; uint64_t total = 0;
  srcs = malloc(cap * sizeof *srcs); lens = malloc(cap * sizeof *lens);
  while (fgets(path, sizeof path, fl)) {
    path[strcspn(path, "\n")] = 0; FILE *f = fopen(path, "rb"); if (!f) continue;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    if (nfiles == cap) { cap *= 2; srcs = realloc(srcs, cap * sizeof *srcs); lens = realloc(lens, cap * sizeof *lens); }
    srcs[nfiles] = malloc(len + 1); if (fread(srcs[nfiles], 1, len, f) != (size_t)len) return 1; fclose(f);
    lens[nfiles++] = len; total += len;
  }
  order = malloc(nfiles * sizeof *order);
  for (int i = 0; i < nfiles; i++) order[i] = i;
  qsort(order, nfiles, sizeof *order, bylen);
  pthread_barrier_init(&start_bar, NULL, workers + 1);
  pthread_barrier_init(&done_bar, NULL, workers + 1);
  W = aligned_alloc(64, ((workers * sizeof *W) + 63) & ~63ul); memset(W, 0, workers * sizeof *W);
  for (int i = 0; i < workers; i++) { W[i].id = i; pthread_create(&W[i].th, NULL, work, &W[i]); }
  double *t = malloc((runs + 1) * sizeof *t);
  printf("%s workers=%d files=%d bytes=%.1fMB\n", use_c ? "c" : "asm", workers, nfiles, total / 1e6);
  for (int r = 0; r <= runs; r++) {
    atomic_store(&next, 0);
    double t0 = now();
    pthread_barrier_wait(&start_bar);
    pthread_barrier_wait(&done_bar);
    t[r] = now() - t0;
    long acc = 0, nodes = 0;
    for (int i = 0; i < workers; i++) { acc += W[i].accepted; nodes += W[i].nodes; }
    printf("  %s %.3fs %.1f MB/s accepted=%ld nodes=%ld\n", r ? "run" : "warm", t[r], total / 1e6 / t[r], acc, nodes);
  }
  for (int i = 0; i < workers; i++) pthread_join(W[i].th, NULL);
  qsort(t + 1, runs, sizeof *t, dcmp);
  double med = runs % 2 ? t[1 + runs / 2] : (t[runs / 2] + t[runs / 2 + 1]) / 2;
  printf("RESULT %s workers=%d median=%.3fs MB/s=%.1f min=%.3f max=%.3f runs=%d\n", use_c ? "c" : "asm", workers, med, total / 1e6 / med, t[1], t[runs], runs);
  free(t);
  return 0;
}
