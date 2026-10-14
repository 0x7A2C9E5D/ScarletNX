/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* swipc: the IPC interface generator (#155). It reads interface definitions written in SwIPC's language (.swipc,
   see docs/IPC.md) and writes for each file a header, with the types and the prototype of every command's handler,
   and a C file with the dispatchers. A dispatcher unpacks a CMIF request into its handler's arguments and packs the
   reply, laid out as the SDK's service framework lays them out (Atmosphere's sf_impl_command_serialization.hpp,
   read, not ported, and libnx's cmif.h describe the same layout):
   - raw arguments are stable-sorted by alignment, ascending, each placed at the next multiple of its alignment. The
     outputs are placed the same way, and the reply's raw size is rounded up to 4
   - pid (ClientProcessId) is an 8 byte raw argument whose value is the client's process id, aruid
     (ClientAppletResourceUserId) one whose value is kept. Both make the client send its process id
   - a buffer's attributes give its descriptor list (A, B, X, C, or A and X / B and C for AutoSelect). Each list is
     numbered in declaration order, and an AutoSelect buffer takes the next index of both of its lists
   - handles in and out in declaration order. Out objects become move handles ahead of the declared ones (ipc.c)
   It is strict C90 and depends on nothing, so a cross build can build it for the machine it runs on.

   Usage: swipc --root DIR --out DIR FILE.swipc...
   Each FILE is relative to --root, and its outputs are --out/FILE.h and --out/FILE.c. A type has to be defined
   before it is used, so files that define shared types come first. Outputs are only rewritten when they change. */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NAME_LEN   128
#define MAX_FILES  64
#define MAX_TYPES  4096
#define MAX_FIELDS 8192
#define MAX_IFACES 512
#define MAX_CMDS   8192
#define MAX_ARGS   32768
#define MAX_SVCS   16
#define MAX_MSG    0x100 /* the IPC message buffer a reply is written into (ipc.c's respond) */

enum {
    TK_INT,
    TK_BOOL,
    TK_FLOAT,
    TK_PID,
    TK_ARUID,
    TK_BYTES,
    TK_ARRAY,
    TK_STRUCT,
    TK_ALIAS,
    TK_BUFFER,
    TK_OBJECT,
    TK_HANDLE
};
/* buffer attributes (nn::sf::cmif BufferAttribute) */
#define BA_IN          0x01u
#define BA_OUT         0x02u
#define BA_MAP_ALIAS   0x04u
#define BA_POINTER     0x08u
#define BA_FIXED_SIZE  0x10u
#define BA_AUTO_SELECT 0x20u
#define BA_NON_SECURE  0x40u
#define BA_NON_DEVICE  0x80u
#define BA_KNOWN       0xFFu
/* descriptor lists: the second list of an AutoSelect buffer is its pointer one */
enum { L_NONE, L_A, L_B, L_X, L_C, L_AUTO_IN, L_AUTO_OUT };

typedef struct {
    char name[NAME_LEN]; /* the .swipc name, empty for a type written in place */
    char ref[NAME_LEN];  /* TK_OBJECT: the interface's name */
    int kind;
    unsigned long size, align;
    int base;            /* TK_ALIAS, TK_ARRAY, TK_BUFFER: the type it is made of */
    unsigned long count; /* TK_ARRAY: elements. TK_BUFFER: the fixed size, 0 when variable */
    unsigned attr;       /* TK_BUFFER */
    int is_signed;       /* TK_INT */
    int field0, nfields; /* TK_STRUCT */
    int move;            /* TK_HANDLE */
    int file;
    int line;
} Type;
typedef struct {
    char name[NAME_LEN];
    int type;
    unsigned long offset;
} Field;
typedef struct {
    char name[NAME_LEN];
    int type;
    int out;
    unsigned long offset;    /* raw arguments */
    int list, index, index2; /* buffers: the descriptor list and the index in it, index2 the pointer list's */
} Arg;
typedef struct {
    unsigned long id;
    char name[NAME_LEN];
    int arg0, nargs;
    int stub;
    unsigned long in_size, out_size;
    int line;
} Cmd;
typedef struct {
    char name[NAME_LEN];
    char svc[MAX_SVCS][NAME_LEN];
    int nsvc;
    int cmd0, ncmds;
    char fallback[NAME_LEN]; /* what answers a command the interface doesn't declare */
    int file;
    int line;
} Iface;
typedef struct {
    char path[512];      /* relative to the root */
    int uses[MAX_FILES]; /* the files whose types it uses */
} File;

static Type types[MAX_TYPES];
static Field fields[MAX_FIELDS];
static Iface ifaces[MAX_IFACES];
static Cmd cmds[MAX_CMDS];
static Arg args[MAX_ARGS];
static File files[MAX_FILES];
static int ntypes, nfields, nifaces, ncmds, nargs, nfiles;

/* ============================================================ errors */
static const char* cur_path = "";
static int cur_line;
static void fail(const char* fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s:%d: error: ", cur_path, cur_line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}
static void copy_name(char* dst, const char* src) {
    if (strlen(src) >= NAME_LEN) fail("name too long: %.40s...", src);
    strcpy(dst, src);
}

/* ============================================================ lexer */
enum { T_EOF, T_IDENT, T_NUM, T_ARROW, T_PUNCT };
static const char* src;
static size_t pos;
static int tok;
static char tok_text[NAME_LEN];
static unsigned long tok_num;

static void skip_space(void) {
    for (;;) {
        while (src[pos] && isspace((unsigned char)src[pos])) {
            if (src[pos] == '\n') cur_line++;
            pos++;
        }
        if (src[pos] == '#' || (src[pos] == '/' && src[pos + 1] == '/')) {
            while (src[pos] && src[pos] != '\n') pos++;
        } else if (src[pos] == '/' && src[pos + 1] == '*') {
            pos += 2;
            while (src[pos] && !(src[pos] == '*' && src[pos + 1] == '/')) {
                if (src[pos] == '\n') cur_line++;
                pos++;
            }
            if (!src[pos]) fail("unterminated comment");
            pos += 2;
        } else {
            return;
        }
    }
}
static int ident_char(int c) { return isalnum(c) || c == '_'; }
/* an identifier takes in "::" scopes: nn::account::Uid is one token */
static void next(void) {
    size_t n = 0;
    skip_space();
    if (!src[pos]) {
        tok = T_EOF;
        tok_text[0] = 0;
        return;
    }
    if (isalpha((unsigned char)src[pos]) || src[pos] == '_') {
        while (ident_char((unsigned char)src[pos]) ||
               (src[pos] == ':' && src[pos + 1] == ':' && ident_char((unsigned char)src[pos + 2]))) {
            int step = src[pos] == ':' ? 2 : 1;
            if (n + step >= NAME_LEN) fail("identifier too long");
            memcpy(tok_text + n, src + pos, (size_t)step);
            n += (size_t)step;
            pos += (size_t)step;
        }
        tok_text[n] = 0;
        tok = T_IDENT;
        return;
    }
    if (isdigit((unsigned char)src[pos])) {
        char* end;
        tok_num = strtoul(src + pos, &end, src[pos] == '0' && (src[pos + 1] == 'x' || src[pos + 1] == 'X') ? 16 : 10);
        if (ident_char((unsigned char)*end)) fail("bad number");
        n = (size_t)(end - (src + pos));
        if (n >= NAME_LEN) fail("number too long");
        memcpy(tok_text, src + pos, n);
        tok_text[n] = 0;
        pos += n;
        tok = T_NUM;
        return;
    }
    if (src[pos] == '-' && src[pos + 1] == '>') {
        strcpy(tok_text, "->");
        pos += 2;
        tok = T_ARROW;
        return;
    }
    tok_text[0] = src[pos++];
    tok_text[1] = 0;
    tok = T_PUNCT;
}
static int is_punct(int c) { return tok == T_PUNCT && tok_text[0] == c; }
static int is_word(const char* w) { return tok == T_IDENT && !strcmp(tok_text, w); }
static void expect(int c) {
    if (!is_punct(c)) fail("expected '%c', found '%s'", c, tok_text);
    next();
}
static void expect_ident(char* out) {
    if (tok != T_IDENT) fail("expected a name, found '%s'", tok_text);
    copy_name(out, tok_text);
    next();
}
static unsigned long expect_num(void) {
    unsigned long v;
    if (tok != T_NUM) fail("expected a number, found '%s'", tok_text);
    v = tok_num;
    next();
    return v;
}
/* a service name after "is": sm:, ldr:ro, fsp-srv, nvdrv:a. sm's names are 8 bytes */
static void service_name(char* out) {
    size_t n = 0;
    skip_space();
    while (src[pos] && (ident_char((unsigned char)src[pos]) || src[pos] == ':' || src[pos] == '-')) {
        if (n == 8) fail("a service name has at most 8 characters");
        out[n++] = src[pos++];
    }
    out[n] = 0;
    if (!n) fail("expected a service name");
    next();
}

/* ============================================================ types */
static int new_type(const char* name, int kind, unsigned long size, unsigned long align) {
    Type* t;
    if (ntypes == MAX_TYPES) fail("too many types");
    t = &types[ntypes];
    memset(t, 0, sizeof *t);
    copy_name(t->name, name);
    t->kind = kind;
    t->size = size;
    t->align = align;
    t->base = -1;
    t->file = nfiles - 1;
    t->line = cur_line;
    return ntypes++;
}
static int find_type(const char* name) {
    int i;
    for (i = 0; i < ntypes; i++)
        if (types[i].name[0] && !strcmp(types[i].name, name)) return i;
    return -1;
}
static void builtin_types(void) {
    static const struct {
        const char* name;
        int kind;
        unsigned long size;
        int is_signed;
    } b[] = {{"u8", TK_INT, 1, 0},  {"u16", TK_INT, 2, 0},    {"u32", TK_INT, 4, 0},   {"u64", TK_INT, 8, 0},
             {"i8", TK_INT, 1, 1},  {"i16", TK_INT, 2, 1},    {"i32", TK_INT, 4, 1},   {"i64", TK_INT, 8, 1},
             {"s8", TK_INT, 1, 1},  {"s16", TK_INT, 2, 1},    {"s32", TK_INT, 4, 1},   {"s64", TK_INT, 8, 1},
             {"b8", TK_BOOL, 1, 0}, {"bool", TK_BOOL, 1, 0},  {"f32", TK_FLOAT, 4, 0}, {"f64", TK_FLOAT, 8, 0},
             {"pid", TK_PID, 8, 0}, {"aruid", TK_ARUID, 8, 0}};
    size_t i;
    for (i = 0; i < sizeof b / sizeof b[0]; i++)
        types[new_type(b[i].name, b[i].kind, b[i].size, b[i].size)].is_signed = b[i].is_signed;
}
/* the type an alias stands for */
static int resolve(int t) {
    while (types[t].kind == TK_ALIAS) t = types[t].base;
    return t;
}
static int is_raw(int t) {
    int k = types[resolve(t)].kind;
    return k != TK_BUFFER && k != TK_OBJECT && k != TK_HANDLE;
}
static int is_scalar(int t) {
    int k = types[resolve(t)].kind;
    return k == TK_INT || k == TK_BOOL || k == TK_FLOAT || k == TK_PID || k == TK_ARUID;
}
static void note_use(int t) {
    int f = types[t].file;
    if (f >= 0 && f != nfiles - 1) files[nfiles - 1].uses[f] = 1;
}
static int parse_type(void);
static int struct_here; /* set by a type definition: the type it parses may be a struct */
static int parse_struct(void) {
    int t = new_type("", TK_STRUCT, 0, 1), f0 = nfields;
    unsigned long off = 0;
    expect('{');
    while (!is_punct('}')) {
        int ft = parse_type();
        Field* f;
        if (!is_raw(ft)) fail("a struct holds raw data only");
        if (nfields == MAX_FIELDS) fail("too many struct fields");
        f = &fields[nfields++];
        f->type = ft;
        expect_ident(f->name);
        expect(';');
        off = (off + types[ft].align - 1) / types[ft].align * types[ft].align;
        f->offset = off;
        off += types[ft].size;
        if (types[ft].align > types[t].align) types[t].align = types[ft].align;
    }
    next();
    if (nfields == f0) fail("empty struct");
    types[t].size = (off + types[t].align - 1) / types[t].align * types[t].align;
    types[t].field0 = f0;
    types[t].nfields = nfields - f0;
    return t;
}
static int parse_type(void) {
    int t;
    char name[NAME_LEN];
    if (tok != T_IDENT) fail("expected a type, found '%s'", tok_text);
    copy_name(name, tok_text);
    next();
    if (!strcmp(name, "struct")) {
        if (!struct_here) fail("a struct is written in a type definition: give it a name with type");
        struct_here = 0;
        t = parse_struct();
    } else if (!strcmp(name, "bytes") || !strcmp(name, "unknown")) {
        unsigned long size = 0, align = 1;
        if (is_punct('<')) {
            next();
            size = expect_num();
            if (is_punct(',')) {
                next();
                align = expect_num();
            }
            expect('>');
        } else if (!strcmp(name, "bytes")) {
            fail("bytes needs a size");
        }
        if (!size) fail("%s needs a size here", name);
        if (!align || align & (align - 1) || align > 16) fail("bad alignment %lu", align);
        if (size % align) fail("size %lu is not a multiple of the alignment %lu", size, align);
        t = new_type("", TK_BYTES, size, align);
    } else if (!strcmp(name, "buffer") || !strcmp(name, "array")) {
        int elem, is_array = !strcmp(name, "array");
        unsigned attr;
        unsigned long size = 0;
        expect('<');
        elem = parse_type();
        if (!is_raw(elem)) fail("a buffer holds raw data");
        expect(',');
        attr = (unsigned)expect_num();
        if (!is_array && is_punct(',')) {
            next();
            if (tok == T_IDENT && (is_word("variable") || is_word("unknown")))
                next();
            else
                size = expect_num();
        }
        expect('>');
        if (attr & ~BA_KNOWN) fail("unknown buffer attribute bits %x", attr & ~BA_KNOWN);
        if (!(attr & (BA_IN | BA_OUT))) fail("buffer attribute %x is neither in nor out", attr);
        if ((attr & BA_IN) && (attr & BA_OUT)) fail("buffer attribute %x: exchange (W) buffers are not served", attr);
        if (((attr & BA_MAP_ALIAS) != 0) + ((attr & BA_POINTER) != 0) + ((attr & BA_AUTO_SELECT) != 0) != 1)
            fail("buffer attribute %x needs exactly one of MapAlias, Pointer, AutoSelect", attr);
        if ((attr & BA_FIXED_SIZE) && !size) size = types[elem].size;
        t = new_type("", TK_BUFFER, 0, 1);
        types[t].base = elem;
        types[t].attr = attr;
        types[t].count = size;
    } else if (!strcmp(name, "object")) {
        expect('<');
        t = new_type("", TK_OBJECT, 0, 1);
        expect_ident(types[t].ref);
        expect('>');
    } else if (!strcmp(name, "handle") || !strcmp(name, "KHandle")) {
        expect('<');
        t = new_type("", TK_HANDLE, 0, 1);
        if (is_word("move"))
            types[t].move = 1;
        else if (!is_word("copy"))
            fail("a handle is copy or move");
        next();
        if (is_punct(',')) {
            next();
            expect_ident(name);
        }
        expect('>');
    } else {
        t = find_type(name);
        if (t < 0) fail("unknown type %s", name);
        note_use(t);
    }
    struct_here = 0;
    while (is_punct('[')) { /* T[N] */
        int a;
        unsigned long n;
        if (!is_raw(t)) fail("an array holds raw data");
        if (!types[t].name[0] && (types[t].kind == TK_BYTES || types[t].kind == TK_ARRAY || types[t].kind == TK_STRUCT))
            fail("an array of bytes, arrays or a struct written in place: give the element a name with type");
        next();
        n = expect_num();
        expect(']');
        if (!n) fail("empty array");
        a = new_type("", TK_ARRAY, types[t].size * n, types[t].align);
        types[a].base = t;
        types[a].count = n;
        t = a;
    }
    return t;
}

/* ============================================================ interfaces */
/* @stub before a command, @fallback(function) before an interface; @version(...) and @undocumented are skipped */
static void decorators(int* stub, char* fallback) {
    while (is_punct('@')) {
        char name[NAME_LEN];
        next();
        expect_ident(name);
        if (!strcmp(name, "fallback") && fallback) {
            expect('(');
            expect_ident(fallback);
            if (strstr(fallback, "::")) fail("@fallback names a C function");
            expect(')');
        } else if (!strcmp(name, "stub") && stub) {
            *stub = 1;
        } else if (!strcmp(name, "version") || !strcmp(name, "undocumented")) {
            if (is_punct('(')) {
                while (!is_punct(')')) {
                    if (tok == T_EOF) fail("unterminated decorator");
                    next();
                }
                next();
            }
        } else {
            fail("unknown decorator @%s here", name);
        }
    }
}
static void parse_arg(int out) {
    Arg* a;
    if (nargs == MAX_ARGS) fail("too many arguments");
    a = &args[nargs];
    memset(a, 0, sizeof *a);
    a->out = out;
    a->type = parse_type();
    if (tok == T_IDENT)
        expect_ident(a->name);
    else if (types[a->type].kind == TK_PID || types[a->type].kind == TK_ARUID)
        strcpy(a->name, types[a->type].name);
    else
        fail("every argument and output has a name (pid and aruid may go without)");
    nargs++;
}
/* the offsets of the raw arguments of one direction: stable insertion sort by alignment, ascending */
static unsigned long place_raw(int arg0, int n, int out) {
    int order[64], count = 0, i, j;
    unsigned long off = 0;
    for (i = arg0; i < arg0 + n; i++)
        if (args[i].out == out && is_raw(args[i].type)) {
            if (count == 64) fail("too many raw arguments");
            order[count++] = i;
        }
    for (i = 1; i < count; i++)
        for (j = i; j > 0 && types[args[order[j - 1]].type].align > types[args[order[j]].type].align; j--) {
            int k = order[j];
            order[j] = order[j - 1];
            order[j - 1] = k;
        }
    for (i = 0; i < count; i++) {
        Arg* a = &args[order[i]];
        unsigned long al = types[a->type].align;
        off = (off + al - 1) / al * al;
        a->offset = off;
        off += types[a->type].size;
    }
    return off;
}
static void check_cmd(Cmd* c) {
    int i, na = 0, nb = 0, nx = 0, nc = 0, ncopy = 0, nmove = 0, pid = 0, in_objs = 0;
    int out_copy = 0, out_move = 0, out_objs = 0;
    for (i = c->arg0; i < c->arg0 + c->nargs; i++) {
        Arg* a = &args[i];
        int j, t = resolve(a->type);
        for (j = c->arg0; j < i; j++)
            if (!strcmp(args[j].name, a->name)) fail("%s: two arguments named %s", c->name, a->name);
        if (!strcmp(a->name, "self")) fail("%s: self is the handler's first parameter", c->name);
        switch (types[t].kind) {
            case TK_PID:
            case TK_ARUID:
                if (a->out) fail("%s: %s is an input", c->name, types[t].name);
                if (pid++) fail("%s: one pid or aruid at most", c->name);
                break;
            case TK_BUFFER: {
                unsigned attr = types[t].attr;
                if (a->out) fail("%s: buffers go with the inputs, their attributes give the direction", c->name);
                if (attr & BA_AUTO_SELECT) {
                    a->list = attr & BA_IN ? L_AUTO_IN : L_AUTO_OUT;
                    a->index = attr & BA_IN ? na++ : nb++;
                    a->index2 = attr & BA_IN ? nx++ : nc++;
                } else if (attr & BA_MAP_ALIAS) {
                    a->list = attr & BA_IN ? L_A : L_B;
                    a->index = attr & BA_IN ? na++ : nb++;
                } else {
                    a->list = attr & BA_IN ? L_X : L_C;
                    a->index = attr & BA_IN ? nx++ : nc++;
                }
                break;
            }
            case TK_OBJECT:
                if (a->out)
                    out_objs++;
                else
                    a->index = in_objs++;
                break;
            case TK_HANDLE:
                if (a->out) {
                    if (types[t].move)
                        out_move++;
                    else
                        out_copy++;
                } else {
                    a->index = types[t].move ? nmove++ : ncopy++;
                }
                break;
            default: break;
        }
    }
    if (na > 8 || nb > 8 || nx > 8 || nc > 8) fail("%s: more than 8 buffers of one kind", c->name);
    if (ncopy > 8 || nmove > 8 || in_objs > 8 || out_copy > 8 || out_move > 8 || out_objs > 8)
        fail("%s: more than 8 handles or objects of one kind", c->name);
    c->in_size = place_raw(c->arg0, c->nargs, 0);
    c->out_size = place_raw(c->arg0, c->nargs, 1);
    c->out_size = (c->out_size + 3) & ~3ul;
    {
        /* the reply as respond() writes it, on a plain session (out objects are move handles) and in a domain (they
           are ids after the data, behind a domain header): the header words, the handles, padding to 16, the
           CmifOutHeader and the raw data */
        unsigned long handles = (unsigned long)(out_copy + out_move), plain, domain;
        plain = (8 + (handles + out_objs ? 4 + 4 * (handles + out_objs) : 0) + 15) / 16 * 16 + 16 + c->out_size;
        domain = (8 + (handles ? 4 + 4 * handles : 0) + 15) / 16 * 16 + 16 + 16 + c->out_size + 4ul * out_objs;
        if (plain > MAX_MSG || domain > MAX_MSG)
            fail("%s: the reply takes %lu bytes, the message holds %d", c->name, plain > domain ? plain : domain,
                 MAX_MSG);
    }
    if (c->stub)
        for (i = c->arg0; i < c->arg0 + c->nargs; i++) {
            int k = types[resolve(args[i].type)].kind;
            if (args[i].out && (k == TK_OBJECT || k == TK_HANDLE))
                fail("%s: a @stub answers zeros, and cannot give an object or a handle", c->name);
        }
}
static void parse_interface(const char* fallback) {
    Iface* f;
    int i;
    if (nifaces == MAX_IFACES) fail("too many interfaces");
    f = &ifaces[nifaces];
    memset(f, 0, sizeof *f);
    strcpy(f->fallback, fallback[0] ? fallback : "stub_ok");
    f->file = nfiles - 1;
    f->line = cur_line;
    expect_ident(f->name);
    for (i = 0; i < nifaces; i++)
        if (!strcmp(ifaces[i].name, f->name)) fail("interface %s is defined twice", f->name);
    if (is_word("is")) {
        do {
            if (f->nsvc == MAX_SVCS) fail("too many service names");
            service_name(f->svc[f->nsvc++]);
            if (!is_punct(',')) break;
        } while (1);
    }
    expect('{');
    f->cmd0 = ncmds;
    while (!is_punct('}')) {
        Cmd* c;
        int stub = 0;
        decorators(&stub, NULL);
        if (ncmds == MAX_CMDS) fail("too many commands");
        c = &cmds[ncmds];
        memset(c, 0, sizeof *c);
        c->stub = stub;
        c->line = cur_line;
        expect('[');
        c->id = expect_num();
        expect(']');
        expect_ident(c->name);
        for (i = f->cmd0; i < ncmds; i++) {
            if (cmds[i].id == c->id) fail("command %lu is defined twice", c->id);
            if (!strcmp(cmds[i].name, c->name)) fail("%s is defined twice", c->name);
        }
        c->arg0 = nargs;
        expect('(');
        while (!is_punct(')')) {
            parse_arg(0);
            if (!is_punct(',')) break;
            next();
        }
        expect(')');
        if (tok == T_ARROW) {
            next();
            if (is_punct('(')) {
                next();
                while (!is_punct(')')) {
                    parse_arg(1);
                    if (!is_punct(',')) break;
                    next();
                }
                expect(')');
            } else {
                parse_arg(1);
            }
        }
        expect(';');
        c->nargs = nargs - c->arg0;
        check_cmd(c);
        ncmds++;
    }
    next();
    if (is_punct(';')) next();
    f->ncmds = ncmds - f->cmd0;
    nifaces++;
}
static void parse_file(void) {
    next();
    while (tok != T_EOF) {
        char fallback[NAME_LEN];
        fallback[0] = 0;
        decorators(NULL, fallback);
        if (is_word("interface")) {
            next();
            parse_interface(fallback);
        } else if (is_word("type")) {
            char name[NAME_LEN];
            int t, before, line = cur_line;
            if (fallback[0]) fail("@fallback goes with an interface");
            next();
            expect_ident(name);
            if (find_type(name) >= 0) fail("type %s is defined twice", name);
            expect('=');
            before = ntypes;
            struct_here = 1;
            t = parse_type();
            expect(';');
            /* a type written in place takes the name, one that has a name already gets an alias */
            if (t >= before && !types[t].name[0]) {
                copy_name(types[t].name, name);
            } else {
                int a = new_type(name, TK_ALIAS, types[t].size, types[t].align);
                types[a].base = t;
                t = a;
            }
            types[t].line = line;
            if (!is_raw(t)) fail("type %s: only raw data has a type name", name);
        } else {
            fail("expected interface or type, found '%s'", tok_text);
        }
    }
}

/* ============================================================ output */
static char* obuf;
static size_t olen, ocap;
static void emit(const char* fmt, ...) {
    char line[4096];
    va_list ap;
    size_t n;
    va_start(ap, fmt);
    vsprintf(line, fmt, ap);
    va_end(ap);
    n = strlen(line);
    if (olen + n + 1 > ocap) {
        ocap = (olen + n + 1) * 2;
        obuf = (char*)realloc(obuf, ocap);
        if (!obuf) fail("out of memory");
    }
    memcpy(obuf + olen, line, n + 1);
    olen += n;
}
/* the file is only written when it changes, so the build recompiles what includes it only then */
static void flush(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) {
        size_t n = olen + 1;
        char* old = (char*)malloc(n);
        size_t got = old ? fread(old, 1, n, f) : 0;
        int same = old && got == olen && !memcmp(old, obuf, olen);
        free(old);
        fclose(f);
        if (same) {
            olen = 0;
            return;
        }
    }
    f = fopen(path, "wb");
    if (!f || fwrite(obuf, 1, olen, f) != olen || fclose(f)) {
        fprintf(stderr, "swipc: cannot write %s\n", path);
        exit(1);
    }
    olen = 0;
}
static void mangle(char* out, const char* name) {
    size_t i, n = 0;
    for (i = 0; name[i]; i++) {
        if (name[i] == ':') {
            out[n++] = '_';
            i++;
        } else {
            out[n++] = name[i];
        }
    }
    out[n] = 0;
}
/* the C type of a raw type, and for an array the element's */
static void ctype(char* out, int t) {
    Type* ty = &types[t];
    if (ty->name[0] && ty->kind != TK_INT && ty->kind != TK_BOOL && ty->kind != TK_FLOAT && ty->kind != TK_PID &&
        ty->kind != TK_ARUID) {
        mangle(out, ty->name);
        return;
    }
    switch (ty->kind) {
        case TK_INT: sprintf(out, "%c%lu", ty->is_signed ? 'i' : 'u', ty->size * 8); break;
        case TK_BOOL: strcpy(out, "u8"); break;
        case TK_FLOAT: strcpy(out, ty->size == 4 ? "float" : "double"); break;
        case TK_PID:
        case TK_ARUID: strcpy(out, "u64"); break;
        case TK_BYTES: strcpy(out, "u8"); break;
        case TK_ARRAY: ctype(out, ty->base); break;
        default: fail("internal: no C type for kind %d", ty->kind);
    }
}
/* anonymous bytes and arrays are passed as a pointer to their elements */
static int is_inline_array(int t) {
    return !types[t].name[0] && (types[t].kind == TK_BYTES || types[t].kind == TK_ARRAY);
}
static unsigned long inline_count(int t) { return types[t].kind == TK_BYTES ? types[t].size : types[t].count; }
static void emit_type(int t) {
    Type* ty = &types[t];
    char cn[NAME_LEN], base[NAME_LEN];
    int i;
    mangle(cn, ty->name);
    switch (ty->kind) {
        case TK_ALIAS:
            ctype(base, ty->base);
            if (is_inline_array(ty->base))
                emit("typedef struct {\n    %s v[%lu];\n} %s;\n", base, inline_count(ty->base), cn);
            else
                emit("typedef %s %s;\n", base, cn);
            break;
        case TK_BYTES: emit("typedef struct {\n    u8 v[%lu];\n} %s;\n", ty->size, cn); break;
        case TK_ARRAY:
            ctype(base, ty->base);
            emit("typedef struct {\n    %s v[%lu];\n} %s;\n", base, ty->count, cn);
            break;
        case TK_STRUCT: {
            /* the padding is spelled out, so the C layout is the wire layout whatever the members' C alignment */
            unsigned long off = 0;
            int pad = 0;
            emit("typedef struct %s {\n", cn);
            for (i = ty->field0; i < ty->field0 + ty->nfields; i++) {
                Field* f = &fields[i];
                if (f->offset > off) emit("    u8 pad%d_[%lu];\n", pad++, f->offset - off);
                ctype(base, f->type);
                if (is_inline_array(f->type))
                    emit("    %s %s[%lu];\n", base, f->name, inline_count(f->type));
                else
                    emit("    %s %s;\n", base, f->name);
                off = f->offset + types[f->type].size;
            }
            if (ty->size > off) emit("    u8 pad%d_[%lu];\n", pad, ty->size - off);
            emit("} %s;\n", cn);
            for (i = ty->field0; i < ty->field0 + ty->nfields; i++)
                emit("typedef char %s_%s_at_%lx_[offsetof(%s, %s) == %lu ? 1 : -1];\n", cn, fields[i].name,
                     fields[i].offset, cn, fields[i].name, fields[i].offset);
            break;
        }
        default: return;
    }
    emit("typedef char %s_is_%lx_bytes_[sizeof(%s) == %lu ? 1 : -1];\n\n", cn, ty->size, cn, ty->size);
}
/* the handler's parameter for an argument */
static void param(char* out, const Arg* a) {
    int t = resolve(a->type);
    char ct[NAME_LEN];
    switch (types[t].kind) {
        case TK_BUFFER: sprintf(out, "IpcBuf %s", a->name); return;
        case TK_OBJECT: sprintf(out, "SvcObj*%s %s", a->out ? "*" : "", a->name); return;
        case TK_HANDLE: sprintf(out, "u32%s %s", a->out ? "*" : "", a->name); return;
        default: break;
    }
    ctype(ct, a->type);
    if (is_scalar(a->type) && !a->out)
        sprintf(out, "%s %s", ct, a->name);
    else
        sprintf(out, "%s%s* %s", a->out ? "" : "const ", ct, a->name);
}
static void handler_name(char* out, const Iface* f, const Cmd* c) {
    mangle(out, f->name);
    strcat(out, "_");
    strcat(out, c->name);
}
static void emit_prototype(const Iface* f, const Cmd* c, const char* end) {
    char name[NAME_LEN * 2], p[NAME_LEN * 3];
    int i;
    handler_name(name, f, c);
    emit("int %s(SvcObj* self", name);
    for (i = c->arg0; i < c->arg0 + c->nargs; i++) {
        param(p, &args[i]);
        emit(", %s", p);
    }
    emit(")%s", end);
}
static void include_name(char* out, const char* path, const char* ext) { sprintf(out, "%s%s", path, ext); }
static void guard_name(char* out, const char* path) {
    size_t i;
    strcpy(out, "SWIPC_");
    for (i = 0; path[i]; i++) {
        char ch = path[i];
        size_t n = strlen(out);
        out[n] = isalnum((unsigned char)ch) ? (char)toupper((unsigned char)ch) : '_';
        out[n + 1] = 0;
    }
    strcat(out, "_H");
}
static void write_header(int fi, const char* out_dir) {
    char path[1024], guard[600], inc[600], cn[NAME_LEN];
    int i, j;
    guard_name(guard, files[fi].path);
    emit("/* Generated by tools/swipc/swipc.c from %s: edit that file instead */\n", files[fi].path);
    emit("#ifndef %s\n#define %s\n#include \"hle/kernel/ipc.h\"\n#include <stddef.h>\n", guard, guard);
    for (i = 0; i < nfiles; i++)
        if (files[fi].uses[i]) {
            include_name(inc, files[i].path, ".h");
            emit("#include \"%s\"\n", inc);
        }
    emit("\n");
    for (i = 0; i < ntypes; i++)
        if (types[i].file == fi && types[i].name[0]) emit_type(i);
    for (i = 0; i < nifaces; i++) {
        Iface* f = &ifaces[i];
        if (f->file != fi) continue;
        mangle(cn, f->name);
        emit("/* %s", f->name);
        for (j = 0; j < f->nsvc; j++) emit("%s%s", j ? ", " : ", served as ", f->svc[j]);
        emit(" */\n");
        for (j = f->cmd0; j < f->cmd0 + f->ncmds; j++)
            if (!cmds[j].stub) emit_prototype(f, &cmds[j], ";\n");
        if (strcmp(f->fallback, "stub_ok") && strcmp(f->fallback, "quiet_ok") && strcmp(f->fallback, "ipc_unknown"))
            emit("int %s(SvcObj* self, IpcCtx* c); /* the commands it doesn't declare */\n", f->fallback);
        emit("int %s_dispatch(SvcObj* self, IpcCtx* c);\n", cn);
        emit("SvcObj* %s_new(size_t data_size); /* an object of this interface, with data_size bytes of data */\n", cn);
        if (f->nsvc) emit("void %s_register(SvcCtor ctor); /* registers ctor as its services */\n", cn);
        emit("\n");
    }
    emit("#endif\n");
    sprintf(path, "%s/%s.h", out_dir, files[fi].path);
    flush(path);
}
static const char* list_name(int list) {
    switch (list) {
        case L_A: return "a";
        case L_B: return "b";
        case L_X: return "x";
        case L_C: return "c";
        default: return "";
    }
}
/* how the unpacker passes an argument's local: scalars, handles, buffers and in objects by value, inline arrays as
   their first element, the rest by address */
static const char* pass(const Arg* a) {
    int k = types[resolve(a->type)].kind;
    if (k == TK_BUFFER || is_inline_array(a->type)) return "";
    if (!a->out && (k == TK_OBJECT || k == TK_HANDLE || is_scalar(a->type))) return "";
    return "&";
}
static void emit_unpacker(const Iface* f, const Cmd* c) {
    char cn[NAME_LEN], ct[NAME_LEN], name[NAME_LEN * 2];
    int i, direct = 1, refs = 0;
    mangle(cn, f->name);
    handler_name(name, f, c);
    /* a command without outputs or objects in answers what its handler does. refs: outputs to drop on failure */
    for (i = c->arg0; i < c->arg0 + c->nargs; i++) {
        int k = types[resolve(args[i].type)].kind;
        if (args[i].out || k == TK_OBJECT) direct = 0;
        if (args[i].out && (k == TK_OBJECT || k == TK_HANDLE)) refs = 1;
    }
    emit("/* [%lu] %s: 0x%lx bytes of raw data in, 0x%lx out */\nstatic int %s__%lu(SvcObj* self, IpcCtx* c) {\n",
         c->id, c->name, c->in_size, c->out_size, cn, c->id);
    for (i = c->arg0; i < c->arg0 + c->nargs; i++) {
        Arg* a = &args[i];
        switch (types[resolve(a->type)].kind) {
            case TK_BUFFER: emit("    IpcBuf a_%s;\n", a->name); break;
            case TK_OBJECT: emit("    SvcObj* a_%s = NULL;\n", a->name); break;
            case TK_HANDLE: emit("    u32 a_%s = 0;\n", a->name); break;
            default:
                ctype(ct, a->type);
                if (is_inline_array(a->type))
                    emit("    %s a_%s[%lu];\n", ct, a->name, inline_count(a->type));
                else
                    emit("    %s a_%s;\n", ct, a->name);
        }
    }
    if (!direct) emit("    int rc;\n");
    /* the inputs, and the outputs zeroed */
    for (i = c->arg0; i < c->arg0 + c->nargs; i++) {
        Arg* a = &args[i];
        int t = resolve(a->type);
        const char* amp = is_inline_array(a->type) ? "" : "&";
        int auto_in = a->list == L_AUTO_IN;
        if (a->out) {
            if (is_raw(a->type)) emit("    memset(%sa_%s, 0, sizeof a_%s);\n", amp, a->name, a->name);
            continue;
        }
        switch (types[t].kind) {
            case TK_PID: emit("    a_%s = c->has_pid ? c->pid : 0;\n", a->name); break;
            case TK_BUFFER:
                if (a->list == L_AUTO_IN || a->list == L_AUTO_OUT)
                    emit("    a_%s = ipc_buf_auto(c->%s, c->n%s, %d, c->%s, c->n%s, %d);\n", a->name,
                         auto_in ? "a" : "b", auto_in ? "a" : "b", a->index, auto_in ? "x" : "c", auto_in ? "x" : "c",
                         a->index2);
                else
                    emit("    a_%s = ipc_buf(c->%s, c->n%s, %d);\n", a->name, list_name(a->list), list_name(a->list),
                         a->index);
                break;
            case TK_OBJECT: emit("    a_%s = ipc_in_obj(c, %d);\n", a->name, a->index); break;
            case TK_HANDLE: {
                const char* kind = types[t].move ? "move" : "copy";
                emit("    a_%s = %d < c->n%s ? c->%s_h[%d] : 0;\n", a->name, a->index, kind, kind, a->index);
                break;
            }
            default: emit("    ipc_in(c, 0x%lx, %sa_%s, sizeof a_%s);\n", a->offset, amp, a->name, a->name);
        }
    }
    emit(direct ? "    return %s(self" : "    rc = %s(self", name);
    for (i = c->arg0; i < c->arg0 + c->nargs; i++) emit(", %sa_%s", pass(&args[i]), args[i].name);
    emit(");\n");
    if (direct) {
        emit("}\n");
        return;
    }
    /* objects in: the handler took a reference of its own if it keeps one */
    for (i = c->arg0; i < c->arg0 + c->nargs; i++)
        if (!args[i].out && types[resolve(args[i].type)].kind == TK_OBJECT)
            emit("    svcobj_unref(a_%s);\n", args[i].name);
    /* a failed request answers no data, handles or objects */
    if (refs) {
        emit("    if (rc) {\n");
        for (i = c->arg0; i < c->arg0 + c->nargs; i++) {
            Arg* a = &args[i];
            int k = types[resolve(a->type)].kind;
            if (a->out && k == TK_OBJECT) emit("        svcobj_unref(a_%s);\n", a->name);
            if (a->out && k == TK_HANDLE) emit("        if (a_%s) ipc_close(a_%s);\n", a->name, a->name);
        }
        emit("        return rc;\n    }\n");
    } else {
        emit("    if (rc) return rc;\n");
    }
    for (i = c->arg0; i < c->arg0 + c->nargs; i++) {
        Arg* a = &args[i];
        int t = resolve(a->type);
        if (!a->out) continue;
        switch (types[t].kind) {
            case TK_OBJECT: emit("    out_obj(c, a_%s);\n", a->name); break;
            case TK_HANDLE: emit("    out_%s_h(c, a_%s);\n", types[t].move ? "move" : "copy", a->name); break;
            default:
                emit("    ipc_out(c, 0x%lx, %sa_%s, sizeof a_%s);\n", a->offset, is_inline_array(a->type) ? "" : "&",
                     a->name, a->name);
        }
    }
    if (c->out_size) emit("    c->out_size = 0x%lx;\n", c->out_size);
    emit("    return 0;\n}\n");
}
static void write_source(int fi, const char* out_dir) {
    char path[1024], inc[600], cn[NAME_LEN];
    int i, j;
    include_name(inc, files[fi].path, ".h");
    emit("/* Generated by tools/swipc/swipc.c from %s: edit that file instead */\n#include \"%s\"\n\n", files[fi].path,
         inc);
    for (i = 0; i < nifaces; i++) {
        Iface* f = &ifaces[i];
        if (f->file != fi) continue;
        mangle(cn, f->name);
        emit("/* ================================================================ %s */\n", f->name);
        for (j = f->cmd0; j < f->cmd0 + f->ncmds; j++)
            if (!cmds[j].stub && cmds[j].nargs) emit_unpacker(f, &cmds[j]);
        emit("int %s_dispatch(SvcObj* self, IpcCtx* c) {\n    switch (c->cmd) {\n", cn);
        for (j = f->cmd0; j < f->cmd0 + f->ncmds; j++) {
            Cmd* c = &cmds[j];
            if (!c->stub && !c->nargs) {
                char name[NAME_LEN * 2];
                handler_name(name, f, c);
                emit("        case %lu: return %s(self);\n", c->id, name);
            } else if (!c->stub) {
                emit("        case %lu: return %s__%lu(self, c);\n", c->id, cn, c->id);
            } else if (c->out_size)
                emit("        case %lu: c->out_size = 0x%lx; return 0; /* %s: @stub */\n", c->id, c->out_size, c->name);
            else
                emit("        case %lu: return 0; /* %s: @stub */\n", c->id, c->name);
        }
        emit("    }\n    return %s(self, c);\n}\n", f->fallback);
        emit("SvcObj* %s_new(size_t data_size) { return svcobj_new(\"%s\", %s_dispatch, data_size); }\n", cn, f->name,
             cn);
        if (f->nsvc) {
            emit("void %s_register(SvcCtor ctor) {\n", cn);
            for (j = 0; j < f->nsvc; j++) emit("    svc_register(\"%s\", ctor);\n", f->svc[j]);
            emit("}\n");
        }
        emit("\n");
    }
    sprintf(path, "%s/%s.c", out_dir, files[fi].path);
    flush(path);
}

/* ============================================================ main */
static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    char* d;
    long n;
    if (!f) {
        fprintf(stderr, "swipc: cannot open %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = (char*)malloc((size_t)n + 1);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "swipc: cannot read %s\n", path);
        exit(1);
    }
    d[n] = 0;
    fclose(f);
    return d;
}
/* every object<I> names an interface some file defines */
static void check_refs(void) {
    int i, j;
    for (i = 0; i < ntypes; i++) {
        if (types[i].kind != TK_OBJECT) continue;
        for (j = 0; j < nifaces; j++)
            if (!strcmp(ifaces[j].name, types[i].ref)) break;
        if (j == nifaces) {
            cur_path = files[types[i].file].path;
            cur_line = types[i].line;
            fail("object<%s>: no such interface", types[i].ref);
        }
    }
}
int main(int argc, char** argv) {
    const char *root = NULL, *out = NULL;
    char path[1024];
    int i;
    builtin_types();
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc) {
            root = argv[++i];
        } else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
            out = argv[++i];
        } else {
            if (!root || !out) break;
            if (nfiles == MAX_FILES) {
                fprintf(stderr, "swipc: too many files\n");
                return 1;
            }
            if (strlen(argv[i]) + strlen(root) + 2 > sizeof path || strlen(argv[i]) >= sizeof files[0].path) {
                fprintf(stderr, "swipc: path too long: %s\n", argv[i]);
                return 1;
            }
            strcpy(files[nfiles].path, argv[i]);
            nfiles++;
            sprintf(path, "%s/%s", root, argv[i]);
            src = read_file(path);
            pos = 0;
            cur_path = files[nfiles - 1].path;
            cur_line = 1;
            parse_file();
            free((void*)src);
        }
    }
    if (!root || !out || !nfiles) {
        fprintf(stderr, "usage: swipc --root DIR --out DIR FILE.swipc...\n");
        return 1;
    }
    for (i = 0; i < nfiles; i++)
        if (strlen(out) + strlen(files[i].path) + 4 > sizeof path) {
            fprintf(stderr, "swipc: path too long: %s/%s\n", out, files[i].path);
            return 1;
        }
    check_refs();
    for (i = 0; i < nfiles; i++) {
        write_header(i, out);
        write_source(i, out);
    }
    return 0;
}
