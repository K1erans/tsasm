// Prototype of a faithful port of C tree-sitter's graph-structured stack (stack.c) and its GLR
// driver (ts_parser__advance / __reduce / __accept / __condense_stack), on tsasm's data model:
// the same 32-byte tree nodes in an append-only arena, the same tables, and the asm lexer and
// token cache (through the PROTO shim in src/parser.s). Error recovery is out of scope, as in
// tsasm: a version with no action is dead (C pauses it, then drops it in condense whenever an
// active version exists, and otherwise recovers, which yields an error tree).
// This is the spec for the assembly port, not part of the library.
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../tsasm.h"

void tsasm_proto_begin(TsasmParser *p, const TsasmLanguage *lang, const uint8_t *src, uint64_t len);
int tsasm_proto_token(TsasmParser *p, uint32_t state, uint32_t pos);

typedef struct {
  const uint16_t *parse_table;
  const uint64_t *actions;
  const uint16_t *lex_state;
  const uint8_t *reserved_set;
  const uint64_t *reserved_bits;
  const uint8_t *symbol_flags;
  const uint16_t *public_symbol;
  const uint16_t *alias_seq;
  const char **names;
  void *lex_main, *lex_kw;
  uint32_t nsym, total_syms, word, rwords;
  uint64_t pad;
  const uint32_t *symbol_out;
} Lang;

typedef struct { uint16_t sym; uint8_t prod; uint8_t extra; uint32_t start, end, cc, ci; int32_t dyn; } Tree;
typedef struct { uint32_t node, tree; } Link;
#define MAX_LINKS 8
#define MAX_ITERS 64
#define MAX_VERSIONS 6
#define MAX_VERSIONS_OVERFLOW 4
typedef struct { uint32_t pos; uint16_t state; uint8_t nlinks, mark; int32_t dyn; uint32_t aux; Link links[MAX_LINKS]; } SNode;
typedef struct { uint32_t node, dead; } Head;
typedef struct { uint32_t *v; uint32_t n, cap; } Arr;
typedef struct { uint32_t node, count; Arr subs; } Iter;
typedef struct { Arr subs; uint32_t version; } Slice;

#define OFF_TSYM 64
#define OFF_TSTART 68
#define OFF_TEND 72
#define OFF_TKW 80

static struct {
  const Lang *L;
  TsasmParser *lex;
  Tree *t; uint32_t nt, tcap;
  uint32_t *kids; uint32_t nk, kcap;
  SNode *s; uint32_t ns, scap;
  Head h[64]; uint32_t nh;
  Iter it[MAX_ITERS + 1]; uint32_t ni;
  Slice *sl; uint32_t nsl, slcap;
  Arr trail, trail2, tmp;
  int32_t finished;
  TsasmNode *out; uint64_t ocap;
  uint32_t *stk; uint64_t stkcap;
} P;

uint32_t proto_stat_maxheads, proto_stat_maxiters, proto_stat_maxlinks, proto_stat_maxslices;

static void push(Arr *a, uint32_t x) {
  if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 16; a->v = realloc(a->v, a->cap * sizeof *a->v); }
  a->v[a->n++] = x;
}
static void copy_arr(Arr *dst, const Arr *src) {
  dst->n = 0;
  for (uint32_t i = 0; i < src->n; i++) push(dst, src->v[i]);
}

static uint32_t new_tree(void) {
  if (P.nt == P.tcap) { P.tcap = P.tcap ? P.tcap * 2 : 4096; P.t = realloc(P.t, P.tcap * sizeof *P.t); }
  memset(&P.t[P.nt], 0, sizeof *P.t);
  return P.nt++;
}
static int32_t tdyn(uint32_t t) { return P.t[t].cc ? P.t[t].dyn : 0; }   // ts_subtree_dynamic_precedence

// ts_subtree_new_node: children n from a[], start/end from them (or base position if none)
static uint32_t new_node(uint16_t sym, const uint32_t *a, uint32_t n, uint8_t prod, uint32_t basepos) {
  uint32_t t = new_tree();
  while (P.nk + n > P.kcap) { P.kcap = P.kcap ? P.kcap * 2 : 4096; P.kids = realloc(P.kids, P.kcap * 4); }
  Tree *x = &P.t[t];
  x->sym = sym; x->prod = prod; x->cc = n; x->ci = P.nk;
  int32_t d = 0;
  for (uint32_t i = 0; i < n; i++) { P.kids[P.nk++] = a[i]; d += tdyn(a[i]); }
  x->dyn = d;
  if (n) { x->start = P.t[a[0]].start; x->end = P.t[a[n - 1]].end; }
  else x->start = x->end = basepos;
  return t;
}

static uint32_t new_snode(uint32_t prev, uint32_t tree, uint16_t state) {
  if (P.ns == P.scap) { P.scap = P.scap ? P.scap * 2 : 4096; P.s = realloc(P.s, P.scap * sizeof *P.s); }
  SNode *n = &P.s[P.ns];
  memset(n, 0, sizeof *n);
  n->state = state;
  n->nlinks = 1;
  n->links[0] = (Link){prev, tree};
  n->pos = P.t[tree].end;
  n->dyn = P.s[prev].dyn + tdyn(tree);
  return P.ns++;
}

static int compare(uint32_t a, uint32_t b) {   // ts_subtree_compare
  uint64_t sp = 0;
#define SPUSH(x, y) do { if (sp + 2 > P.stkcap) { P.stkcap = P.stkcap ? P.stkcap * 2 : 1024; P.stk = realloc(P.stk, P.stkcap * 4); } P.stk[sp++] = (x); P.stk[sp++] = (y); } while (0)
  SPUSH(a, b);
  while (sp) {
    uint32_t r = P.stk[--sp], l = P.stk[--sp];
    Tree *L = &P.t[l], *R = &P.t[r];
    if (L->sym < R->sym) return -1;
    if (R->sym < L->sym) return 1;
    if (L->cc < R->cc) return -1;
    if (R->cc < L->cc) return 1;
    for (uint32_t i = L->cc; i > 0; i--) SPUSH(P.kids[L->ci + i - 1], P.kids[R->ci + i - 1]);
  }
  return 0;
}

// ts_parser__select_tree with no error costs: true to take right
static int select_tree(uint32_t left, int32_t right_dyn, int cmp_if_needed_right, const uint32_t *rk, uint32_t rn) {
  (void)cmp_if_needed_right;
  int32_t ld = tdyn(left);
  if (right_dyn > ld) return 1;
  if (ld > right_dyn) return 0;
  // compare(left, scratch(right children)): symbols equal, then child count, then children
  Tree *L = &P.t[left];
  if (L->cc < rn) return 0;
  if (rn < L->cc) return 1;
  for (uint32_t i = 0; i < rn; i++) {
    int c = compare(P.kids[L->ci + i], rk[i]);
    if (c) return c > 0;
  }
  return 0;
}

static int link_equivalent(Link a, Link b) {   // stack__subtree_is_equivalent
  if (a.tree == b.tree) return 1;
  Tree *x = &P.t[a.tree], *y = &P.t[b.tree];
  if (x->sym != y->sym) return 0;
  return x->start - P.s[a.node].pos == y->start - P.s[b.node].pos &&
         x->end - x->start == y->end - y->start && x->cc == y->cc && x->extra == y->extra;
}

static void add_link(uint32_t self, Link link) {   // stack_node_add_link
  if (link.node == self) return;
  for (int i = 0; i < P.s[self].nlinks; i++) {
    Link ex = P.s[self].links[i];
    if (link_equivalent(ex, link)) {
      if (ex.node == link.node) {
        if (tdyn(link.tree) > tdyn(ex.tree)) {
          P.s[self].links[i].tree = link.tree;
          P.s[self].dyn = P.s[link.node].dyn + tdyn(link.tree);
        }
        return;
      }
      if (P.s[ex.node].state == P.s[link.node].state && P.s[ex.node].pos == P.s[link.node].pos) {
        for (int j = 0; j < P.s[link.node].nlinks; j++) add_link(ex.node, P.s[link.node].links[j]);
        int32_t d = P.s[link.node].dyn + tdyn(link.tree);
        if (d > P.s[self].dyn) P.s[self].dyn = d;
        return;
      }
    }
  }
  if (P.s[self].nlinks == MAX_LINKS) return;
  P.s[self].links[P.s[self].nlinks++] = link;
  if (P.s[self].nlinks > proto_stat_maxlinks) proto_stat_maxlinks = P.s[self].nlinks;
  int32_t d = P.s[link.node].dyn + tdyn(link.tree);
  if (d > P.s[self].dyn) P.s[self].dyn = d;
}

static uint32_t add_version(uint32_t node) {
  assert(P.nh < 64);
  P.h[P.nh] = (Head){node, 0};
  if (P.nh + 1 > proto_stat_maxheads) proto_stat_maxheads = P.nh + 1;
  return P.nh++;
}
static void remove_version(uint32_t v) {
  memmove(&P.h[v], &P.h[v + 1], (P.nh - v - 1) * sizeof *P.h);
  P.nh--;
}
static void renumber(uint32_t v1, uint32_t v2) {
  if (v1 == v2) return;
  assert(v2 < v1);
  P.h[v2] = P.h[v1];
  remove_version(v1);
}
static int can_merge(uint32_t a, uint32_t b) {
  SNode *x = &P.s[P.h[a].node], *y = &P.s[P.h[b].node];
  return !P.h[a].dead && !P.h[b].dead && x->state == y->state && x->pos == y->pos;
}
static int merge(uint32_t a, uint32_t b) {
  if (!can_merge(a, b)) return 0;
  uint32_t n2 = P.h[b].node;
  for (uint32_t i = 0; i < P.s[n2].nlinks; i++) add_link(P.h[a].node, P.s[n2].links[i]);
  remove_version(b);
  return 1;
}

static void add_slice(uint32_t node, Arr *subs) {   // ts_stack__add_slice
  if (P.nsl == P.slcap) { P.slcap = P.slcap ? P.slcap * 2 : 16; P.sl = realloc(P.sl, P.slcap * sizeof *P.sl); }
  for (uint32_t i = P.nsl; i-- > 0;) {
    uint32_t v = P.sl[i].version;
    if (P.h[v].node == node) {
      memmove(&P.sl[i + 2], &P.sl[i + 1], (P.nsl - i - 1) * sizeof *P.sl);
      P.sl[i + 1] = (Slice){*subs, v};
      P.nsl++;
      goto out;
    }
  }
  P.sl[P.nsl++] = (Slice){*subs, add_version(node)};
out:
  if (P.nsl > proto_stat_maxslices) proto_stat_maxslices = P.nsl;
}

static void reverse(Arr *a) {
  for (uint32_t i = 0, j = a->n; i + 1 < j; i++, j--) { uint32_t x = a->v[i]; a->v[i] = a->v[j - 1]; a->v[j - 1] = x; }
}

// stack__iter with pop_count_callback (goal >= 0) or pop_all_callback (goal < 0)
static void pop(uint32_t version, int64_t goal) {
  P.nsl = 0;
  P.ni = 1;
  P.it[0].node = P.h[version].node;
  P.it[0].count = 0;
  P.it[0].subs.n = 0;
  while (P.ni) {
    for (uint32_t i = 0, size = P.ni; i < size; i++) {
      uint32_t node = P.it[i].node;
      int should_pop = goal >= 0 ? P.it[i].count == goal : P.s[node].nlinks == 0;
      int should_stop = (goal >= 0 && should_pop) || P.s[node].nlinks == 0;
      if (should_pop) {
        Arr subs = {0};
        copy_arr(&subs, &P.it[i].subs);   // each slice owns its array (C moves it when stopping)
        reverse(&subs);
        add_slice(node, &subs);
      }
      if (should_stop) {
        Arr keep = P.it[i].subs;
        memmove(&P.it[i], &P.it[i + 1], (P.ni - i - 1) * sizeof *P.it);
        P.ni--;
        P.it[P.ni].subs = keep;   // recycle the buffer
        P.it[P.ni].subs.n = 0;
        i--, size--;
        continue;
      }
      uint32_t nl = P.s[node].nlinks;
      for (uint32_t j = 1; j <= nl; j++) {
        uint32_t k;
        Link link;
        if (j == nl) { link = P.s[node].links[0]; k = i; }
        else {
          if (P.ni >= MAX_ITERS) continue;
          link = P.s[node].links[j];
          k = P.ni++;
          if (P.ni > proto_stat_maxiters) proto_stat_maxiters = P.ni;
          P.it[k].node = P.it[i].node;
          P.it[k].count = P.it[i].count;
          copy_arr(&P.it[k].subs, &P.it[i].subs);
        }
        P.it[k].node = link.node;
        push(&P.it[k].subs, link.tree);
        if (!P.t[link.tree].extra) P.it[k].count++;
      }
    }
  }
}

static void remove_trailing_extras(Arr *a, Arr *dst) {
  dst->n = 0;
  while (a->n && P.t[a->v[a->n - 1]].extra) push(dst, a->v[--a->n]);
  reverse(dst);
}

static uint16_t goto_state(uint16_t state, uint16_t sym) { return P.L->parse_table[(uint32_t)state * P.L->nsym + sym]; }

#define NONE 0xFFFFFFFFu
static uint32_t reduce(uint32_t version, uint16_t sym, uint32_t count, int32_t dyn, uint8_t prod) {
  uint32_t initial = P.nh;
  pop(version, count);
  uint32_t removed = 0;
  for (uint32_t i = 0; i < P.nsl; i++) {
    Slice slice = P.sl[i];
    uint32_t sv = slice.version - removed;
    if (sv > MAX_VERSIONS + MAX_VERSIONS_OVERFLOW) {
      remove_version(sv);
      removed++;
      free(slice.subs.v);
      while (i + 1 < P.nsl && P.sl[i + 1].version == slice.version) free(P.sl[++i].subs.v);
      continue;
    }
    Arr children = slice.subs;
    remove_trailing_extras(&children, &P.trail);
    uint32_t basepos = P.s[P.h[sv].node].pos;
    uint32_t parent = new_node(sym, children.v, children.n, prod, basepos);
    while (i + 1 < P.nsl && P.sl[i + 1].version == slice.version) {
      i++;
      Arr next = P.sl[i].subs;
      remove_trailing_extras(&next, &P.trail2);
      int32_t nd = 0;
      for (uint32_t k = 0; k < next.n; k++) nd += tdyn(next.v[k]);
      if (select_tree(parent, nd, 0, next.v, next.n)) {
        Arr t = P.trail; P.trail = P.trail2; P.trail2 = t;
        parent = new_node(sym, next.v, next.n, prod, basepos);
      }
      free(next.v);
    }
    uint16_t state = P.s[P.h[sv].node].state;
    uint16_t next_state = goto_state(state, sym);
    P.t[parent].dyn += dyn;
    P.h[sv].node = new_snode(P.h[sv].node, parent, next_state);
    for (uint32_t j = 0; j < P.trail.n; j++) P.h[sv].node = new_snode(P.h[sv].node, P.trail.v[j], next_state);
    free(children.v);
    for (uint32_t j = 0; j < sv; j++) {
      if (j == version) continue;
      if (merge(j, sv)) { removed++; break; }
    }
  }
  return P.nh > initial ? initial : NONE;
}

static void accept(uint32_t version) {
  uint32_t eof = new_tree();
  P.t[eof].sym = 0; P.t[eof].extra = 1;
  P.t[eof].start = *(uint32_t *)((char *)P.lex + OFF_TSTART);
  P.t[eof].end = *(uint32_t *)((char *)P.lex + OFF_TEND);
  P.h[version].node = new_snode(P.h[version].node, eof, 1);
  pop(version, -1);
  for (uint32_t i = 0; i < P.nsl; i++) {
    Arr trees = P.sl[i].subs;
    uint32_t root = NONE;
    for (uint32_t j = trees.n; j-- > 0;) {
      uint32_t tr = trees.v[j];
      if (!P.t[tr].extra) {
        Arr all = {0};
        for (uint32_t k = 0; k < j; k++) push(&all, trees.v[k]);
        for (uint32_t k = 0; k < P.t[tr].cc; k++) push(&all, P.kids[P.t[tr].ci + k]);
        for (uint32_t k = j + 1; k < trees.n; k++) push(&all, trees.v[k]);
        root = new_node(P.t[tr].sym, all.v, all.n, P.t[tr].prod, 0);
        free(all.v);
        break;
      }
    }
    assert(root != NONE);
    if (P.finished < 0) P.finished = root;
    else {
      int32_t ld = tdyn((uint32_t)P.finished), rd = tdyn(root);
      if (rd > ld || (rd == ld && compare((uint32_t)P.finished, root) > 0)) P.finished = root;
    }
    free(trees.v);
  }
  remove_version(P.sl[0].version);
  P.h[version].dead = 1;
}

static void advance(uint32_t version) {
  uint16_t state = P.s[P.h[version].node].state;
  uint32_t pos = P.s[P.h[version].node].pos;
  if (!tsasm_proto_token(P.lex, state, pos)) { P.h[version].dead = 1; return; }
  uint32_t sym = *(uint32_t *)((char *)P.lex + OFF_TSYM);
  uint32_t tstart = *(uint32_t *)((char *)P.lex + OFF_TSTART);
  uint32_t tend = *(uint32_t *)((char *)P.lex + OFF_TEND);
  int kw = *(uint32_t *)((char *)P.lex + OFF_TKW) != 0;
  uint32_t ai = P.L->parse_table[(uint32_t)state * P.L->nsym + sym];
  for (;;) {
    uint32_t last = NONE;
    uint32_t count = P.L->actions[ai] & 0xFF;
    for (uint32_t k = 0; k < count; k++) {
      uint64_t a = P.L->actions[ai + 1 + k];
      switch (a & 3) {
      case 0: {
        int extra = (a >> 2) & 1;
        uint16_t next = extra ? state : (uint16_t)(a >> 16);
        uint32_t leaf = new_tree();
        P.t[leaf].sym = sym; P.t[leaf].extra = extra || sym == 0;
        P.t[leaf].start = tstart; P.t[leaf].end = tend;
        P.h[version].node = new_snode(P.h[version].node, leaf, next);
        return;
      }
      case 1: {
        uint32_t r = reduce(version, (uint16_t)(a >> 16), (a >> 8) & 0xFF, (int16_t)(a >> 48), (uint8_t)(a >> 32));
        if (r != NONE) last = r;
        break;
      }
      case 2: accept(version); return;
      default: P.h[version].dead = 1; return;
      }
    }
    if (last != NONE) {
      renumber(last, version);
      state = P.s[P.h[version].node].state;
      ai = P.L->parse_table[(uint32_t)state * P.L->nsym + sym];
      continue;
    }
    if (kw && sym != P.L->word) {
      uint32_t set = P.L->reserved_set[state];
      uint64_t bits = P.L->reserved_bits[set * P.L->rwords + (sym >> 6)];
      if (!((bits >> (sym & 63)) & 1)) {
        uint32_t wi = P.L->parse_table[(uint32_t)state * P.L->nsym + P.L->word];
        if (P.L->actions[wi] & 0xFF) { sym = P.L->word; ai = wi; continue; }
      }
    }
    P.h[version].dead = 1;
    return;
  }
}

static void condense(void) {
  for (uint32_t i = 0; i < P.nh; i++) {
    if (P.h[i].dead) { remove_version(i); i--; continue; }
    for (uint32_t j = 0; j < i; j++) {
      int32_t a = P.s[P.h[j].node].dyn, b = P.s[P.h[i].node].dyn;
      if (a >= b) {
        if (merge(j, i)) { i--; j = i; }
      } else {
        if (merge(j, i)) { i--; j = i; }
        else { Head t = P.h[i]; P.h[i] = P.h[j]; P.h[j] = t; }
      }
    }
  }
  while (P.nh > MAX_VERSIONS) remove_version(MAX_VERSIONS);
}

static int64_t emit(TsasmNode **outp) {
  const Lang *L = P.L;
  typedef struct { uint32_t node, ci, si; int32_t parent; } Frame;
  uint64_t fcap = 1024, sp = 0, n = 0;
  Frame *f = malloc(fcap * sizeof *f);
#define OPUT(sym, start, end, parent) do { if (n == P.ocap) { P.ocap = P.ocap ? P.ocap * 2 : 4096; P.out = realloc(P.out, P.ocap * sizeof *P.out); } \
    uint32_t w = L->symbol_out[sym]; P.out[n] = (TsasmNode){(uint16_t)w, (uint8_t)((w >> 16) & 1), 0, start, end, parent}; n++; } while (0)
  Tree *root = &P.t[P.finished];
  OPUT(root->sym, root->start, root->end, -1);
  f[sp++] = (Frame){(uint32_t)P.finished, 0, 0, 0};
  while (sp) {
    Frame *fr = &f[sp - 1];
    Tree *par = &P.t[fr->node];
    if (fr->ci >= par->cc) { sp--; continue; }
    uint32_t c = P.kids[par->ci + fr->ci++];
    Tree *ch = &P.t[c];
    uint32_t alias = 0;
    if (!ch->extra) alias = L->alias_seq[par->prod * 16 + fr->si++];
    uint32_t s = alias ? alias : ch->sym;
    int32_t parent = fr->parent;
    if ((L->symbol_out[s] >> 31) || alias) { OPUT(s, ch->start, ch->end, parent); parent = (int32_t)(n - 1); }
    if (ch->cc) {
      if (sp == fcap) { fcap *= 2; f = realloc(f, fcap * sizeof *f); }
      f[sp++] = (Frame){c, 0, 0, parent};
    }
  }
  free(f);
  *outp = P.out;
  return (int64_t)n;
}

int64_t proto_parse(const TsasmLanguage *lang, const uint8_t *src, uint64_t len, const TsasmNode **nodes) {
  if (!P.lex) P.lex = tsasm_parser_new();
  P.L = (const Lang *)lang;
  tsasm_proto_begin(P.lex, lang, src, len);
  P.nt = 0; P.nk = 0; P.ns = 0;
  new_tree();                                  // tree 0: unused
  P.s = P.scap ? P.s : NULL;
  if (P.ns == P.scap) { P.scap = 4096; P.s = realloc(P.s, P.scap * sizeof *P.s); }
  memset(&P.s[0], 0, sizeof *P.s);
  P.s[0].state = 1;
  P.ns = 1;
  P.nh = 0;
  add_version(0);
  P.finished = -1;
  uint32_t last_pos = 0;
  uint32_t vc;
  do {
    for (uint32_t v = 0; vc = P.nh, v < vc; v++) {
      while (!P.h[v].dead) {
        advance(v);
        uint32_t pos = P.s[P.h[v].node].pos;
        if (pos > last_pos || (v > 0 && pos == last_pos)) { last_pos = pos; break; }
      }
    }
    condense();
    if (P.finished >= 0 && P.nh == 0) break;
  } while (vc != 0 && P.nh != 0);
  if (P.finished < 0) { if (nodes) *nodes = NULL; return TSASM_ERR_SYNTAX; }
  TsasmNode *out;
  int64_t n = emit(&out);
  if (nodes) *nodes = out;
  return n;
}
