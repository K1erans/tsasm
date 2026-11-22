// C model of the tsasm algorithm: the executable spec the asm parser follows.
// Uses the generated tables (build/tables.o) and the generated asm lexer (build/lexer.o).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../build/tables.h"
#include "../tsasm.h"

int32_t tsl_lex_c(uint32_t state, const uint8_t *p, const uint8_t *end, uint64_t out[2]);
int32_t tsl_kw_c(const uint8_t *p, const uint8_t *end, uint64_t out[2]);

typedef struct {
  uint16_t symbol; uint8_t prod; uint8_t extra;
  uint32_t start, end;
  uint32_t child_count, child_index;
  int32_t dynprec;
} Node;

typedef struct { uint32_t node; uint32_t state; } Entry;
typedef struct { Entry *e; uint32_t size, cap; int status; } Version; // status 0 active, 1 dead, 2 accepted

typedef struct { int32_t sym; uint32_t start, end, pos; int is_keyword; uint32_t parse_state; int valid; uint32_t leaf; } Token;

static Node *nodes; static uint32_t nn, ncap;
static uint32_t *kids; static uint32_t nk, kcap;
static Version vers[128]; static int nv;
static const uint8_t *SRC; static uint64_t LEN;
static Token cache;
static int32_t finished = -1;
int model_stat_forks, model_stat_merges, model_stat_maxv;

#define NOACT 0
static inline uint16_t lookup(uint32_t state, uint32_t sym) { return tsg_parse_table[state * TSG_SYMBOL_COUNT + sym]; }
static inline int is_reserved(uint32_t state, uint32_t sym) {
  uint32_t set = tsg_reserved_set[state];
  if (sym >= 128) return 0;
  return (tsg_reserved_bits[set * 2 + (sym >> 6)] >> (sym & 63)) & 1;
}

static uint32_t new_node(void) {
  if (nn == ncap) { ncap = ncap ? ncap * 2 : 4096; nodes = realloc(nodes, ncap * sizeof(Node)); }
  return nn++;
}
static void push(Version *v, uint32_t node, uint32_t state) {
  if (v->size == v->cap) { v->cap = v->cap ? v->cap * 2 : 256; v->e = realloc(v->e, v->cap * sizeof(Entry)); }
  v->e[v->size].node = node; v->e[v->size].state = state; v->size++;
}
static inline uint32_t vstate(Version *v) { return v->e[v->size - 1].state; }
static inline uint32_t vpos(Version *v) { return v->size > 1 ? nodes[v->e[v->size - 1].node].end : 0; }
static int32_t vdyn(Version *v) { int32_t d = 0; for (uint32_t i = 1; i < v->size; i++) d += nodes[v->e[i].node].dynprec; return d; }

static int lex(uint32_t state, uint32_t pos, Token *t) {
  uint64_t r[2];
  int32_t sym = tsl_lex_c(tsg_lex_state[state], SRC + pos, SRC + LEN, r);
  if (sym < 0) return 0;
  t->start = (uint32_t)((const uint8_t *)r[0] - SRC);
  t->end = (uint32_t)((const uint8_t *)r[1] - SRC);
  t->is_keyword = 0;
  if (sym == TSG_WORD_TOKEN) {
    uint64_t k[2];
    int32_t kw = tsl_kw_c(SRC + t->start, SRC + LEN, k);
    t->is_keyword = kw >= 0;
    if (kw >= 0 && (const uint8_t *)k[1] - SRC == t->end && (lookup(state, kw) || is_reserved(state, kw))) sym = kw;
  }
  t->sym = sym; t->pos = pos; t->parse_state = state; t->valid = 1; t->leaf = 0;
  return 1;
}

static int can_reuse(uint32_t state, Token *t) {
  uint16_t a = lookup(state, t->sym);
  uint64_t hdr = tsg_actions[a];
  int count = hdr & 0xFF;
  if (count > 0 && tsg_lex_state[t->parse_state] == tsg_lex_state[state] &&
      tsg_reserved_set[t->parse_state] == tsg_reserved_set[state] &&
      (t->sym != TSG_WORD_TOKEN || (!t->is_keyword && t->parse_state == state))) return 1;
  if (t->end == t->start && t->sym != 0) return 0;
  return (hdr >> 8) & 1;
}

static void do_shift(Version *v, uint32_t state, Token *t, int extra) {
  int ex = extra || t->sym == 0;
  uint32_t n = new_node();
  Node *x = &nodes[n];
  x->symbol = t->sym; x->prod = 0; x->extra = ex;
  x->start = t->start; x->end = t->end; x->child_count = 0; x->child_index = 0; x->dynprec = 0;
  push(v, n, state);
}

static void do_reduce(Version *v, uint64_t a) {
  uint32_t count = (a >> 8) & 0xFF, sym = (a >> 16) & 0xFFFF, prod = (a >> 32) & 0xFFFF;
  int32_t dyn = (int16_t)(a >> 48);
  uint32_t i = v->size, c = 0;
  while (c < count) { i--; if (!nodes[v->e[i].node].extra) c++; }
  uint32_t j = v->size;
  while (j > i && nodes[v->e[j - 1].node].extra) j--;
  uint32_t n = new_node();
  Node *x = &nodes[n];
  x->symbol = sym; x->prod = prod; x->extra = 0;
  x->child_count = j - i; x->child_index = nk;
  if (nk + (j - i) > kcap) { kcap = (kcap + (j - i)) * 2; kids = realloc(kids, kcap * 4); }
  int32_t d = dyn;
  for (uint32_t k = i; k < j; k++) { kids[nk++] = v->e[k].node; d += nodes[v->e[k].node].dynprec; }
  x->dynprec = d;
  if (j > i) { x->start = nodes[v->e[i].node].start; x->end = nodes[v->e[j - 1].node].end; }
  else { uint32_t p = i > 1 ? nodes[v->e[i - 1].node].end : 0; x->start = x->end = p; }
  uint32_t next = lookup(v->e[i - 1].state, sym);
  // new node at slot i, trailing extras move to i+1.. (state = next)
  uint32_t ntrail = v->size - j;
  if (v->size + 1 > v->cap) { v->cap *= 2; v->e = realloc(v->e, v->cap * sizeof(Entry)); }
  memmove(&v->e[i + 1], &v->e[j], ntrail * sizeof(Entry));
  v->e[i].node = n; v->e[i].state = next;
  for (uint32_t k = 0; k < ntrail; k++) v->e[i + 1 + k].state = next;
  v->size = i + 1 + ntrail;
}

static int copy_version(int src) {
  if (nv == 128) return -1;
  Version *d = &vers[nv];
  Version *s = &vers[src];
  if (d->cap < s->size) { d->cap = s->cap; d->e = realloc(d->e, d->cap * sizeof(Entry)); }
  memcpy(d->e, s->e, s->size * sizeof(Entry));
  d->size = s->size; d->status = 0;
  model_stat_forks++;
  return nv++;
}
static void remove_version(int i) {
  Version t = vers[i];
  memmove(&vers[i], &vers[i + 1], (nv - i - 1) * sizeof(Version));
  vers[nv - 1] = t; // keep buffer for reuse
  nv--;
}

// compare two subtrees like ts_subtree_compare
static int subtree_compare(uint32_t l, uint32_t r) {
  Node *a = &nodes[l], *b = &nodes[r];
  if (a->symbol < b->symbol) return -1;
  if (b->symbol < a->symbol) return 1;
  if (a->child_count < b->child_count) return -1;
  if (b->child_count < a->child_count) return 1;
  for (uint32_t i = 0; i < a->child_count; i++) {
    int c = subtree_compare(kids[a->child_index + i], kids[b->child_index + i]);
    if (c) return c;
  }
  return 0;
}


#define MAXV 64
static int equivalent(uint32_t a, uint32_t b) {
  Node *x = &nodes[a], *y = &nodes[b];
  return x->symbol == y->symbol && x->start == y->start && x->end == y->end &&
         x->child_count == y->child_count && x->extra == y->extra;
}
// Two versions with equal state and position. If their stacks have converged (identical below a
// contiguous divergent segment), pick one the way C tree-sitter's merge + select_children would.
// returns 0 = not converged, 1 = keep j (X), 2 = keep i (Y)
static int try_converge(int j, int i) {
  Version *X = &vers[j], *Y = &vers[i];
  if (X->size != Y->size) return 0;
  int k = (int)X->size - 1;
  int hi = k;
  while (k > 0 && X->e[k].node != Y->e[k].node) {
    if (X->e[k].state != Y->e[k].state) return 0;
    k--;
  }
  if (k == 0 && X->e[0].node != Y->e[0].node) return 0;
  int lo = k + 1;
  if (lo > hi) return 1; // identical
  for (int l = lo + 1; l <= hi; l++) if (!equivalent(X->e[l].node, Y->e[l].node)) return 0;
  model_stat_merges++;
  uint32_t xn = X->e[lo].node, yn = Y->e[lo].node;
  if (getenv("MTRACE")) fprintf(stderr, "converge j=%d i=%d levels %d..%d  X %s d%d | Y %s d%d\n", j, i, lo, hi,
      tsg_symbol_names[nodes[xn].symbol], nodes[xn].dynprec, tsg_symbol_names[nodes[yn].symbol], nodes[yn].dynprec);
  int ywins;
  if (nodes[yn].dynprec != nodes[xn].dynprec) ywins = nodes[yn].dynprec > nodes[xn].dynprec;
  else if (equivalent(xn, yn)) ywins = 0;
  else ywins = subtree_compare(xn, yn) > 0;
  if (ywins) X->e[lo] = Y->e[lo];
  return 1;
}

static void accept(Version *v, Token *t) {
  // root = top non-extra node, with all other stack nodes spliced around its children
  uint32_t eof = new_node();
  nodes[eof] = (Node){0, 0, 1, t->start, t->end, 0, 0, 0};
  push(v, eof, 1);
  int r = -1;
  for (uint32_t k = v->size - 1; k >= 1; k--) if (!nodes[v->e[k].node].extra) { r = (int)k; break; }
  Node rootn = nodes[v->e[r].node];
  uint32_t n = new_node();
  Node *x = &nodes[n];
  *x = rootn;
  uint32_t total = (v->size - 1) - 1 + rootn.child_count;
  if (nk + total > kcap) { kcap = (kcap + total) * 2; kids = realloc(kids, kcap * 4); }
  x->child_index = nk; x->child_count = total;
  for (uint32_t k = 1; k < v->size; k++) {
    if ((int)k == r) { for (uint32_t c = 0; c < rootn.child_count; c++) kids[nk++] = kids[rootn.child_index + c]; }
    else kids[nk++] = v->e[k].node;
  }
  x->start = nodes[kids[x->child_index]].start;
  x->end = nodes[kids[x->child_index + total - 1]].end;
  if (finished < 0) finished = n;
  else {
    // select_tree(finished, root)
    Node *f = &nodes[finished];
    if (x->dynprec > f->dynprec || (x->dynprec == f->dynprec && subtree_compare(finished, n) > 0)) finished = n;
  }
}

// returns 0 normally
static void advance(int vi) {
  Version *v = &vers[vi];
  uint32_t state = vstate(v), pos = vpos(v);
  Token t;
  if (cache.valid && cache.pos == pos && can_reuse(state, &cache)) t = cache;
  else {
    if (!lex(state, pos, &t)) { v->status = 1; return; }
    cache = t;
  }
  uint64_t hdr = tsg_actions[lookup(state, t.sym)];
  uint32_t ai = lookup(state, t.sym);
  for (;;) {
    int count = hdr & 0xFF;
    int last_red = -1;
    for (int k = 0; k < count; k++) {
      uint64_t a = tsg_actions[ai + 1 + k];
      switch (a & 3) {
        case 0: {
          int extra = (a >> 2) & 1;
          do_shift(&vers[vi], extra ? state : (uint32_t)((a >> 16) & 0xFFFF), &t, extra);
          return;
        }
        case 1: {
          if (count == 1) { do_reduce(&vers[vi], a); last_red = vi; }
          else {
            int nvi = copy_version(vi);
            if (nvi < 0) { vers[vi].status = 1; return; }
            do_reduce(&vers[nvi], a);
            last_red = nvi;
          }
          break;
        }
        case 2: accept(&vers[vi], &t); vers[vi].status = 2; return;
        case 3: vers[vi].status = 1; return;
      }
    }
    if (last_red >= 0) {
      if (last_red != vi) {
        // renumber: last reduction version replaces vi
        Version tmp = vers[vi]; vers[vi] = vers[last_red]; vers[last_red] = tmp;
        remove_version(last_red);
      }
      v = &vers[vi];
      state = vstate(v);
      ai = lookup(state, t.sym);
      hdr = tsg_actions[ai];
      continue;
    }
    if (t.is_keyword && t.sym != TSG_WORD_TOKEN && !is_reserved(state, t.sym)) {
      uint32_t a2 = lookup(state, TSG_WORD_TOKEN);
      if (tsg_actions[a2] & 0xFF) { t.sym = TSG_WORD_TOKEN; ai = a2; hdr = tsg_actions[ai]; continue; }
    }
    vers[vi].status = 1;
    return;
  }
}

static int64_t emit(uint32_t root, TsasmNode *out, uint64_t cap) {
  typedef struct { uint32_t node, ci, si; int32_t parent; } Frame;
  static Frame *st; static uint32_t stcap;
  if (cap < 1) return -2;
  int64_t n = 0;
  Node *r = &nodes[root];
  out[0] = (TsasmNode){tsg_public_symbol[r->symbol], (tsg_symbol_flags[r->symbol] >> 1) & 1, 0, r->start, r->end, -1};
  n = 1;
  uint32_t sp = 0;
  if (!stcap) { stcap = 1024; st = malloc(stcap * sizeof(Frame)); }
  st[sp++] = (Frame){root, 0, 0, 0};
  while (sp) {
    Frame *f = &st[sp - 1];
    Node *p = &nodes[f->node];
    if (f->ci == p->child_count) { sp--; continue; }
    uint32_t c = kids[p->child_index + f->ci++];
    Node *x = &nodes[c];
    uint16_t alias = 0;
    if (!x->extra) { alias = tsg_alias_seq[p->prod * TSG_ALIAS_STRIDE + f->si]; f->si++; }
    uint16_t s = alias ? alias : x->symbol;
    uint8_t fl = tsg_symbol_flags[s];
    int32_t parent = f->parent;
    if (alias || (fl & 1)) {
      if ((uint64_t)n >= cap) return -2;
      out[n] = (TsasmNode){tsg_public_symbol[s], (fl >> 1) & 1, 0, x->start, x->end, parent};
      parent = (int32_t)n++;
    }
    if (x->child_count) {
      if (sp == stcap) { stcap *= 2; st = realloc(st, stcap * sizeof(Frame)); f = NULL; }
      st[sp++] = (Frame){c, 0, 0, parent};
    }
  }
  return n;
}

int64_t model_parse_go(const uint8_t *src, uint64_t len, TsasmNode *out, uint64_t cap) {
  SRC = src; LEN = len; nn = 0; nk = 0; finished = -1; cache.valid = 0;
  new_node(); // node 0 unused (stack base)
  nv = 1;
  vers[0].size = 0; vers[0].status = 0;
  push(&vers[0], 0, 1);
  uint32_t last_position = 0;
  while (nv > 0) {
    for (int vi = 0; vi < nv; vi++) {
      while (vers[vi].status == 0) {
        advance(vi);
        if (vers[vi].status) break;
        uint32_t pos = vpos(&vers[vi]);
        if (pos > last_position || (vi > 0 && pos == last_position)) { last_position = pos; break; }
      }
    }
    if (nv > model_stat_maxv) model_stat_maxv = nv;
    // condense
    for (int i = 0; i < nv; i++) {
      if (vers[i].status) { remove_version(i); i--; continue; }
      for (int j = 0; j < i; j++) {
        Version *X = &vers[j], *Y = &vers[i];
        if (vstate(X) == vstate(Y) && vpos(X) == vpos(Y)) {
          int r = try_converge(j, i);
          if (r == 1) { /* j wins */ remove_version(i); i--; break; }
          if (r == 2) { Version t = vers[j]; vers[j] = vers[i]; vers[i] = t; remove_version(i); i--; break; }
        } else if (vdyn(Y) > vdyn(X)) { Version t = vers[j]; vers[j] = vers[i]; vers[i] = t; }
      }
    }
    while (nv > MAXV) remove_version(MAXV);
  }
  if (finished < 0) return -1;
  return emit(finished, out, cap);
}
