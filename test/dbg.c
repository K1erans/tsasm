#include <stdio.h>
#include <stdint.h>
typedef struct { uint16_t symbol; uint8_t prod, extra; uint32_t start, end, cc, ci; int32_t dyn; uint32_t pad[2]; } N;
typedef struct { uint32_t *e; uint32_t size, cap, status; int32_t dyn; uint64_t pad; } V;
extern const char *const tsg_symbol_names[];
void tsasm_dbg(char *ctx) {
  N *nodes = *(N **)ctx; uint32_t nv = *(uint32_t *)(ctx + 48);
  V *vs = (V *)(ctx + 192);
  printf("round nv=%u\n", nv);
  for (uint32_t i = 0; i < nv; i++) {
    printf(" v%u st%u dyn%d:", i, vs[i].status, vs[i].dyn);
    for (uint32_t k = 1; k < vs[i].size; k++) { N *x = &nodes[vs[i].e[2*k]]; printf(" %s@%u%s/%u", tsg_symbol_names[x->symbol], x->start, x->extra?"*":"", vs[i].e[2*k+1] & 0xffff); }
    printf("\n");
  }
}
