// Guard-page allocator for the fuzz build. The asm's realloc/free/aligned_alloc calls are
// routed here (scripts/covinstr.py), so every buffer the asm uses touches a PROT_NONE page:
// mode "over" (default) ends each buffer exactly at a guard page, mode "under"
// (TSASM_GUARD=under) starts it right after one. New memory is filled with 0xA5 so code that
// relies on zeroed memory misbehaves visibly. guard_fail_at = k makes the k-th allocation fail.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "guard.h"

#define PAGE 4096ul
#define SLOTS 8192
typedef struct { uint8_t *p, *base; size_t maplen, n; } Rec;
static Rec recs[SLOTS];
static int under = -1;
long guard_allocs, guard_fail_at = -1, guard_live;

static Rec *find(void *p, int insert) {
  size_t h = ((uintptr_t)p >> 4) * 0x9E3779B97F4A7C15ull >> 51;
  for (size_t i = 0; i < SLOTS; i++) {
    Rec *r = &recs[(h + i) % SLOTS];
    if (r->p == p) return r;
    if (insert && !r->p) return r;
    if (!insert && !r->p && !r->base) return NULL;
  }
  abort();
}

void *guard_alloc(size_t n) {
  if (under < 0) { const char *m = getenv("TSASM_GUARD"); under = m && !strcmp(m, "under"); }
  size_t pages = (n + PAGE - 1) / PAGE;
  if (!pages) pages = 1;
  size_t maplen = (pages + 2) * PAGE;
  uint8_t *base = mmap(NULL, maplen, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED) abort();
  if (mprotect(base + PAGE, pages * PAGE, PROT_READ | PROT_WRITE)) abort();
  uint8_t *p = under ? base + PAGE : base + PAGE + pages * PAGE - n;
  memset(base + PAGE, 0xA5, pages * PAGE);
  Rec *r = find(p, 1);
  r->p = p; r->base = base; r->maplen = maplen; r->n = n;
  guard_live++;
  return p;
}

void guard_free(void *p) {
  if (!p) return;
  Rec *r = find(p, 0);
  if (!r) abort();                       // free of a pointer the asm never got from us
  munmap(r->base, r->maplen);
  r->p = NULL;                           // keep base set as a tombstone for probing
  guard_live--;
}

static int fail_now(void) { return guard_allocs++ == guard_fail_at; }

void *tsasm_fuzz_realloc(void *old, size_t n) {
  if (fail_now()) return NULL;
  uint8_t *q = guard_alloc(n);
  if (old) {
    Rec *r = find(old, 0);
    if (!r) abort();
    memcpy(q, old, r->n < n ? r->n : n);
    guard_free(old);
  }
  return q;
}
void tsasm_fuzz_free(void *p) { guard_free(p); }
void *tsasm_fuzz_aligned_alloc(size_t align, size_t n) {
  if (fail_now()) return NULL;
  void *p = guard_alloc(n);
  if ((uintptr_t)p % align) abort();
  return p;
}
