---
name: snx-code-quality
description: "Write and review C for ScarletNX by the code quality baseline of RFC #142: no state that another value already says, indices into arrays instead of pointer chains, one allocation per owner carved by offsets, atomics that different threads write on cache lines of their own, and data structures without stored pointers. Use when: writing or reviewing C in src/, adding a struct, an allocation, an atomic or a linked structure, or when asked to check code against #142."
---

# Code quality (RFC #142)

Five rules, each with what to do, why, a wrong and a right form, and when it does not apply. A rule that
does not apply is not a finding: say why and move on.

## Writing

Before adding a struct, an allocation, an atomic or a link between objects, check it against the rules
below. When a rule does not apply, a short comment saying why saves the next reader the same question.

## Reviewing

1. Run the checker on the files the change touches:
   `python3 .claude/skills/snx-code-quality/check.py src/path/a.c src/path/b.h` (no path: all of `src/`).
   It prints candidates for rules 1, 3 and 4 as `file:line: rule: what it saw`. It never fails a build,
   and a candidate is not a finding until you have read the code.
2. Judge each candidate against its rule's "does not apply". Drop the ones that do not hold.
3. Read the diff for rules 2 and 5, which no script sees: a new `next`, `prev` or `parent` pointer, a
   struct that points at its siblings, a struct written to a file or shared memory with a pointer in it.
4. Report each finding as `file:line`, the rule, and the fix in one line.

## 1. No state another value already says

**Do:** let the value carry its own presence. A pointer is absent when it is NULL.

**Why:** two fields saying one thing drift apart. Once `is_present` and `p` disagree, every reader has
to guess which one is true.

<!-- snippet: flag-wrong -->
```c
typedef struct {
    void* p;
    bool is_present;
} Opt;
bool opt_present(const Opt* o) { return o->is_present; }
```

<!-- snippet: flag-right -->
```c
typedef struct {
    void* p; /* NULL: not present */
} Opt;
bool opt_present(const Opt* o) { return o->p != NULL; }
```

**Does not apply when:**
- NULL is a value of its own ("present and empty").
- Every value of the type is valid (a `u32` offset where 0 is a real offset). Then use a sentinel
  (`UINT32_MAX`), or keep a flag if no value is free.
- The flag says something else: `has_keys` in `extract.c` says the keys file exists, while `keys`
  always holds a path.

## 2. Indices, not pointer chains

**Do:** keep the objects in one array and link them by index (`u32`, with a named "none" value such as
`UINT32_MAX`). To sort a list, sort an array of (key, index) pairs with `qsort` instead of
merge-sorting `next` pointers.

**Why:** walking an array touches memory in order, while a chain of pointers jumps across the heap. An
index stays valid when the array is reallocated and when the array is written out (rule 5). A pointer
does neither.

<!-- snippet: indices-wrong -->
```c
typedef struct FsNode {
    struct FsNode* parent;
    bool attached;
} FsNode;
typedef struct {
    FsNode* root;
} FsTree;
bool belongs_to(const FsTree* tree, const FsNode* n) {
    if (!n || !n->attached) return false;
    while (n->parent) {
        n = n->parent;
        if (!n->attached) return false;
    }
    return n == tree->root;
}
```

<!-- snippet: indices-right -->
```c
#define SNX_FS_NONE UINT32_MAX
typedef struct {
    u32 parent; /* SNX_FS_NONE at the top */
    bool attached;
} FsNode;
typedef struct {
    FsNode* nodes; /* every node of the tree */
    u32 root;
} FsTree;
bool belongs_to(const FsTree* tree, u32 n) {
    if (n == SNX_FS_NONE || !tree->nodes[n].attached) return false;
    while (tree->nodes[n].parent != SNX_FS_NONE) {
        n = tree->nodes[n].parent;
        if (!tree->nodes[n].attached) return false;
    }
    return n == tree->root;
}
```

**Does not apply when:**
- The code holding the index cannot reach the array, or the array dies first. An index means nothing
  without its array, so pass the owner along.
- Objects come and go in the middle of the array. Removal leaves a hole (keep a free list) or
  renumbers everything after it (fix every index that pointed there).
- Objects of different owners link to each other, like a thread waiting on several kernel objects.
  Use a pointer there, or an index into one table they all share.

## 3. One allocation per owner

**Do:** an object's arrays come from one allocation carved by offsets, and are freed with one `free`.
When the sizes are fixed, a struct holding the arrays is the simplest block. When they are known only
at run time, align each offset for its type.

**Why:** one block keeps an owner's data together and leaves less to fragment. It also turns a
cleanup path with one `free` per array into a single `free`.

<!-- snippet: block-wrong -->
```c
typedef struct {
    Voice* voice;
    Mix* mix;
} Mixer;
bool mixer_init(Mixer* m, size_t voices, size_t mixes) {
    m->voice = calloc(voices, sizeof *m->voice);
    m->mix = calloc(mixes, sizeof *m->mix);
    return m->voice && m->mix; /* a failure leaks the one that worked */
}
void mixer_free(Mixer* m) {
    free(m->voice);
    free(m->mix);
}
```

<!-- snippet: block-right -->
```c
typedef struct {
    Voice* voice; /* the block's start: free(voice) frees both */
    Mix* mix;     /* after the voices, aligned for Mix */
} Mixer;
bool mixer_init(Mixer* m, size_t voices, size_t mixes) {
    size_t voice_bytes, mix_bytes, total;
    if (ckd_mul(&voice_bytes, voices, sizeof *m->voice) || ckd_mul(&mix_bytes, mixes, sizeof *m->mix))
        return false;
    size_t mix_at = (voice_bytes + alignof(Mix) - 1) / alignof(Mix) * alignof(Mix);
    if (ckd_add(&total, mix_at, mix_bytes)) return false;
    char* block = calloc(1, total ? total : 1);
    if (!block) return false;
    m->voice = (Voice*)block;
    m->mix = (Mix*)(block + mix_at);
    return true;
}
void mixer_free(Mixer* m) { free(m->voice); }
```

`calloc(n, size)` checked each product for overflow. A sum of your own does not, hence the `ckd_` calls,
which matter when a size comes from the guest.

**Does not apply when:**
- One of the arrays grows on its own. A `realloc` moves the whole block, so give that array a block of
  its own.
- The parts live for different lengths of time.
- A part is large and rarely used: allocate it when it is first needed.
- What you are building starts to look like a general allocator (free lists across owners, size
  classes). The RFC asks for chunking, not for a new malloc.

## 4. Atomics on cache lines of their own

**Do:** when two atomics (or a lock and the data next to it) are written by different threads, give each
its own cache line with `alignas`. Defining them next to each other at file scope puts them together too.

**Why:** a core writes a whole 64-byte line. Two threads writing two variables in the same line take
that line from each other on every write, as slowly as if they wrote one shared variable. This is
false sharing.

<!-- snippet: sharing-wrong -->
```c
typedef struct {
    _Atomic u32 submitted; /* written by the CPU thread */
    _Atomic u32 completed; /* written by the GPU thread */
} Queue;
```

<!-- snippet: sharing-right -->
```c
#define SNX_CACHE_LINE 64 /* x86-64 */
typedef struct {
    alignas(SNX_CACHE_LINE) _Atomic u32 submitted; /* written by the CPU thread */
    alignas(SNX_CACHE_LINE) _Atomic u32 completed; /* written by the GPU thread */
} Queue;
static_assert(offsetof(Queue, completed) - offsetof(Queue, submitted) >= SNX_CACHE_LINE);
```

**Does not apply when:**
- One thread writes both, or they are flags read often and written rarely. Padding then buys nothing.
- There are many of them. Each padded atomic costs a full line, and in an array of per-object atomics
  that adds up.

Note that `malloc` aligns to 16, not 64. The fields above stay 64 bytes apart, so apart from each
other, but they may share a line with a neighbouring allocation. Use `aligned_alloc(SNX_CACHE_LINE, ...)`
when that matters.

## 5. No stored pointers in data

**Do:** a structure that is data (tables, trees, anything that could be saved or shared) refers to its
parts by index or by offset from its own start, not by address. Native byte order and widths are fine.
The point is that the block means the same wherever it is copied.

**Why:** an address is only valid in this process and at this moment. Data with offsets can be copied,
written out, read back or mapped elsewhere without fixing it up.

<!-- snippet: serial-wrong -->
```c
typedef struct Node {
    const char* name;    /* a string somewhere on the heap */
    struct Node* parent; /* an address: wrong in a copy, meaningless once written out */
} Node;
```

<!-- snippet: serial-right -->
```c
typedef struct {
    u32 name;   /* offset of its NUL-terminated name in the names that follow the nodes */
    u32 parent; /* index in the nodes; UINT32_MAX at the top */
} Node;
typedef struct {
    u32 count; /* then Node[count], then the names: one block, copied or written out as is */
} NodeTable;
const Node* table_node(const NodeTable* t, u32 i) { return (const Node*)(t + 1) + i; }
const char* table_name(const NodeTable* t, u32 i) {
    return (const char*)((const Node*)(t + 1) + t->count) + table_node(t, i)->name;
}
```

**Does not apply when:**
- The member is a handle to something outside the data: `FILE*`, `SDL_Window*`, a Vulkan object, a
  function pointer. Keep those in a runtime struct beside the data, not inside it.
- The structure is never copied, written out or shared, and a pointer is plainly simpler. The RFC's
  words are "unless absolutely necessary", so say why in a comment.
- The access is hot enough that an add per access shows up in a measurement. Measure first.
