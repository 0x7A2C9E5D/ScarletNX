/* near misses: none of these is a candidate */
#include <stdalign.h>
#include <stdbool.h>
#include <stdlib.h>
typedef unsigned u32;
typedef struct {
    int has_divisor; /* a feature, no pointer of that name */
    bool is_present;  /* two pointers: which one would it describe? */
    void* a;
    void* b;
} Features;
typedef struct {
    _Atomic u32 lock_1;
    alignas(64) _Atomic u32 lock_2;
} PaddedLocks;
typedef struct {
    u32 count;
    _Atomic u32 only;
} OneAtomic;
extern _Atomic int g_elsewhere_1;
extern _Atomic int g_elsewhere_2;
_Atomic int g_alone;
int g_plain;
typedef struct {
    u32* in;
} One;
static const char* two_owners(One* a, One* b) {
    a->in = calloc(1, 4);
    b->in = calloc(1, 4);
    /* a->x = calloc(1, 4096); a->y = calloc(1, 4096); in a comment */
    return "a->x = malloc(1); a->y = malloc(1);";
}
