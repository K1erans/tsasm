#include <stddef.h>
extern long guard_allocs, guard_fail_at, guard_live;
void *guard_alloc(size_t n);
void guard_free(void *p);
