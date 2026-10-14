/* every rule the checker sees, once each */
#include <stdbool.h>
#include <stdlib.h>
typedef unsigned u32;
typedef struct {
    void* p;
    bool is_present;
} Opt;
typedef struct {
    char* name;
    int has_name;
    u32 size;
} Named;
typedef struct {
    _Atomic u32 lock_1;
    _Atomic u32 lock_2;
} Locks;
typedef struct {
    u8* in;
    u8* out;
} Buffers;
_Atomic int g_first;
_Atomic int g_second;
static Buffers* buffers_new(void) {
    Buffers* b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->in = calloc(1, 4096);
    b->out = (u8*)malloc(4096);
    return b;
}
