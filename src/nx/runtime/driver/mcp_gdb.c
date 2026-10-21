/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GDB tools (mcp_int.h): GDB driven through its machine interface (GDB/MI 3), connected to a game's GDB stub
   (nx/runtime/gdbstub.c: gdbserver attached to the runtime), and the guest's state read through it from the
   runtime's own structures by their debug information. A guest thread's KThread is its host thread's tls_thread,
   with the registers in its cpu (a 64-bit game's) or arm (a 32-bit one's), and guest memory is at g_mem. One GDB at a
   time, from gdb_attach to gdb_detach or the server's end */
#define _POSIX_C_SOURCE 200809L
#include "nx/runtime/driver/mcp_int.h"
#include "common/utilities/common.h"
#include "nx/runtime/driver/hactool/cJSON.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
static const char unix_only[] = "the GDB tools are UNIX only, as the GDB stub is";
char* mcp_gdb_attach(long port, Text* err) {
    (void)port;
    mcp_put(err, unix_only);
    return NULL;
}
char* mcp_gdb_command(const char* command, long wait, McpNamer name, const void* ctx, Text* err) {
    (void)command;
    (void)wait;
    (void)name;
    (void)ctx;
    mcp_put(err, unix_only);
    return NULL;
}
char* mcp_gdb_threads(McpNamer name, const void* ctx, Text* err) {
    (void)name;
    (void)ctx;
    mcp_put(err, unix_only);
    return NULL;
}
char* mcp_gdb_guest(long thread, int frames, McpNamer name, const void* ctx, Text* err) {
    (void)thread;
    (void)frames;
    (void)name;
    (void)ctx;
    mcp_put(err, unix_only);
    return NULL;
}
char* mcp_gdb_memory(SnxU64 address, long size, Text* err) {
    (void)address;
    (void)size;
    mcp_put(err, unix_only);
    return NULL;
}
char* mcp_gdb_detach(Text* err) {
    mcp_put(err, unix_only);
    return NULL;
}
void mcp_gdb_close(void) {}
#else
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ATTACH_MS    180000 /* connecting: GDB reads the runtime's symbols and its libraries' */
#define COMMAND_MS   30000  /* a command that doesn't resume the game */
#define INTERRUPT_MS 10000  /* the game stopping once interrupted */
#define MAX_LINES    400    /* what a tool shows of GDB's output */
#define MAX_FRAMES   64
#define MAX_MEMORY   4096
#define THREAD_BYTES 8192 /* the most of a KThread read at once */

/* KThread's members the tools read, at offsets GDB gives from the runtime's debug information */
enum { K_SIZE, K_ID, K_NAME, K_STATE, K_IN_SVC, K_X, K_PC, K_N, K_TPIDRRO, K_R, K_CPSR, K_COUNT };
static const char* const layout[K_COUNT] = {
    "sizeof(KThread)",        "&((KThread*)0)->id",      "&((KThread*)0)->name",
    "&((KThread*)0)->state",  "&((KThread*)0)->in_svc",  "&((KThread*)0)->cpu.x",
    "&((KThread*)0)->cpu.pc", "&((KThread*)0)->cpu.n",   "&((KThread*)0)->cpu.tpidrro",
    "&((KThread*)0)->arm.r",  "&((KThread*)0)->arm.cpsr"};

/* what one command answered: its console output, unescaped, and its result record */
typedef struct {
    Text console;
    cJSON* result;
    char cls[16]; /* done, running, connected, error or exit; empty until the record came */
} Reply;

static struct {
    pid_t pid;  /* 0: no GDB */
    pid_t game; /* the game's process, which interrupt signals */
    int to, from;
    Text pending;    /* what GDB wrote past the last line taken */
    unsigned token;  /* the last command's: a result record with another is a command's that timed out */
    int running;     /* the game runs: resumed and not stopped since */
    int exited;      /* the game is gone */
    Text stop;       /* the last stop, described, for the command that waited for it */
    long k[K_COUNT]; /* -1 without the runtime's debug information */
} g_gdb;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* a message taken back: a step that may fail without failing the tool */
static void clear(Text* t) {
    t->n = 0;
    if (t->p) t->p[0] = 0;
}

static void reply_free(Reply* r) {
    free(r->console.p);
    cJSON_Delete(r->result);
    memset(r, 0, sizeof *r);
}

static void close_gdb(void) {
    if (!g_gdb.pid) return;
    close(g_gdb.to);
    close(g_gdb.from);
    kill(g_gdb.pid, SIGKILL);
    while (waitpid(g_gdb.pid, NULL, 0) < 0 && errno == EINTR) {}
    free(g_gdb.pending.p);
    free(g_gdb.stop.p);
    memset(&g_gdb, 0, sizeof g_gdb);
}

/* GDB's next line, without its end, in line: 1, 0 when the deadline passes first, -1 when GDB is gone */
static int next_line(Text* line, long long deadline) {
    for (;;) {
        char* end = g_gdb.pending.n ? (char*)memchr(g_gdb.pending.p, '\n', g_gdb.pending.n) : NULL;
        struct pollfd p;
        char chunk[4096];
        long long left;
        ssize_t n;
        int r;
        if (end) {
            size_t len = (size_t)(end - g_gdb.pending.p);
            line->n = 0;
            mcp_put_n(line, g_gdb.pending.p, len && end[-1] == '\r' ? len - 1 : len);
            g_gdb.pending.n -= len + 1;
            memmove(g_gdb.pending.p, end + 1, g_gdb.pending.n + 1);
            return 1;
        }
        left = deadline - now_ms();
        if (left <= 0) return 0;
        p.fd = g_gdb.from;
        p.events = POLLIN;
        p.revents = 0;
        r = poll(&p, 1, left > INT_MAX ? INT_MAX : (int)left);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return r < 0 ? -1 : 0;
        n = read(g_gdb.from, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        mcp_put_n(&g_gdb.pending, chunk, (size_t)n);
    }
}

/* ============================================================ GDB/MI's output syntax, into cJSON */
/* *s at a c-string's opening quote: the string unescaped, *s past it */
static char* mi_cstring(const char** s) {
    Text t = {0};
    const char* p = *s + 1;
    mcp_put_n(&t, "", 0);
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            c = *p++;
            if (c == 'n')
                c = '\n';
            else if (c == 't')
                c = '\t';
            else if (c == 'r')
                c = '\r';
            else if (c == 'e')
                c = 27;
            else if (c >= '0' && c <= '7') {
                int v = c - '0', k;
                for (k = 0; k < 2 && *p >= '0' && *p <= '7'; k++) v = v * 8 + *p++ - '0';
                c = (char)v;
            }
        }
        mcp_put_n(&t, &c, 1);
    }
    if (*p == '"') p++;
    *s = p;
    return t.p;
}
static cJSON* mi_value(const char** s);
/* name=value results up to close (0: the line's end) into object, *s at close */
static void mi_results(const char** s, char close, cJSON* object) {
    while (**s && **s != close) {
        const char* name = *s;
        char key[64];
        size_t n;
        if (**s == ',') {
            ++*s;
            continue;
        }
        while (**s && **s != '=' && **s != close) ++*s;
        if (**s != '=') return;
        n = (size_t)(*s - name);
        snprintf(key, sizeof key, "%.*s", (int)(n < sizeof key - 1 ? n : sizeof key - 1), name);
        ++*s;
        cJSON_AddItemToObject(object, key, mi_value(s));
    }
}
/* a value: a c-string, a tuple (an object) or a list (an array, of the values of a list of results) */
static cJSON* mi_value(const char** s) {
    cJSON* v;
    if (**s == '"') {
        char* text = mi_cstring(s);
        v = cJSON_CreateString(text);
        free(text);
    } else if (**s == '{') {
        v = cJSON_CreateObject();
        ++*s;
        mi_results(s, '}', v);
        if (**s == '}') ++*s;
    } else if (**s == '[') {
        v = cJSON_CreateArray();
        ++*s;
        while (**s && **s != ']') {
            if (**s == ',') {
                ++*s;
                continue;
            }
            if (**s != '"' && **s != '{' && **s != '[') {
                while (**s && **s != '=' && **s != ']') ++*s;
                if (**s != '=') break;
                ++*s;
            }
            cJSON_AddItemToArray(v, mi_value(s));
        }
        if (**s == ']') ++*s;
    } else {
        v = cJSON_CreateNull();
        if (**s) ++*s;
    }
    return v;
}
static const char* field(const cJSON* o, const char* name) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, name);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* "<func> (<file>:<line>)", or "<func or address> in <library>", of a frame tuple */
static void put_frame(Text* t, const cJSON* frame) {
    const char *func = field(frame, "func"), *file = field(frame, "file"), *line = field(frame, "line"),
               *addr = field(frame, "addr"), *from = field(frame, "from");
    mcp_put(t, func ? func : addr ? addr : "?");
    if (file && line) {
        mcp_put(t, " (");
        mcp_put(t, file);
        mcp_put(t, ":");
        mcp_put(t, line);
        mcp_put(t, ")");
    } else if (from) {
        mcp_put(t, " in ");
        mcp_put(t, from);
    }
}

/* a *stopped record: the game stopped, and why */
static void stopped(const char* s) {
    cJSON* o = cJSON_CreateObject();
    const char *reason, *thread;
    if (*s == ',') s++;
    mi_results(&s, 0, o);
    reason = field(o, "reason");
    thread = field(o, "thread-id");
    g_gdb.running = 0;
    g_gdb.stop.n = 0;
    if (reason && !strncmp(reason, "exited", 6)) {
        g_gdb.exited = 1;
        mcp_put(&g_gdb.stop, "the game exited");
        if (field(o, "exit-code")) {
            mcp_put(&g_gdb.stop, " with code ");
            mcp_put(&g_gdb.stop, field(o, "exit-code"));
        }
        if (field(o, "signal-name")) {
            mcp_put(&g_gdb.stop, " on ");
            mcp_put(&g_gdb.stop, field(o, "signal-name"));
        }
    } else {
        mcp_put(&g_gdb.stop, "stopped");
        if (reason) {
            mcp_put(&g_gdb.stop, " (");
            mcp_put(&g_gdb.stop, reason);
            if (field(o, "signal-name")) {
                mcp_put(&g_gdb.stop, " ");
                mcp_put(&g_gdb.stop, field(o, "signal-name"));
            }
            if (field(o, "bkptno")) {
                mcp_put(&g_gdb.stop, " ");
                mcp_put(&g_gdb.stop, field(o, "bkptno"));
            }
            mcp_put(&g_gdb.stop, ")");
        }
        if (thread) {
            mcp_put(&g_gdb.stop, " in thread ");
            mcp_put(&g_gdb.stop, thread);
        }
        if (cJSON_GetObjectItemCaseSensitive(o, "frame")) {
            mcp_put(&g_gdb.stop, " at ");
            put_frame(&g_gdb.stop, cJSON_GetObjectItemCaseSensitive(o, "frame"));
        }
    }
    cJSON_Delete(o);
}

/* one line of GDB's output: the console stream and this command's result record into r, a stop noted */
static void take(const char* line, Reply* r) {
    unsigned token = 0;
    int tokened = 0;
    while (*line >= '0' && *line <= '9') {
        token = token * 10 + (unsigned)(*line++ - '0');
        tokened = 1;
    }
    if (*line == '~' || *line == '@') {
        const char* s = line + 1;
        if (*s == '"') {
            char* text = mi_cstring(&s);
            mcp_put(&r->console, text);
            free(text);
        }
    } else if (*line == '^' && tokened && token == g_gdb.token) {
        const char* s = line + 1;
        size_t n = strcspn(s, ",");
        snprintf(r->cls, sizeof r->cls, "%.*s", (int)(n < sizeof r->cls - 1 ? n : sizeof r->cls - 1), s);
        s += n;
        if (*s == ',') s++;
        cJSON_Delete(r->result);
        r->result = cJSON_CreateObject();
        mi_results(&s, 0, r->result);
        if (!strcmp(r->cls, "running")) g_gdb.running = 1;
    } else if (!strncmp(line, "*stopped", 8))
        stopped(line + 8);
    else if (!strncmp(line, "*running", 8))
        g_gdb.running = 1;
}

static int write_all(int fd, const char* p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* An MI command, read to its result record and the prompt after it: 0 with the reply in r (which the caller frees),
   or -1 with why in err */
static int exec(const char* command, long long deadline, Reply* r, Text* err) {
    Text line = {0};
    char token[16];
    memset(r, 0, sizeof *r);
    mcp_put_n(&r->console, "", 0);
    if (!g_gdb.pid) {
        mcp_put(err, "no GDB: gdb_attach connects one");
        return -1;
    }
    snprintf(token, sizeof token, "%u", ++g_gdb.token);
    if (write_all(g_gdb.to, token, strlen(token)) || write_all(g_gdb.to, command, strlen(command)) ||
        write_all(g_gdb.to, "\n", 1)) {
        mcp_put(err, "GDB is gone");
        close_gdb();
        return -1;
    }
    for (;;) {
        int got = next_line(&line, deadline);
        if (got <= 0) {
            free(line.p);
            mcp_put(err, got ? "GDB is gone" : "GDB didn't answer in time");
            if (got < 0) close_gdb();
            reply_free(r);
            return -1;
        }
        if (!strncmp(line.p, "(gdb)", 5)) {
            if (r->cls[0]) break;
            continue;
        }
        take(line.p, r);
    }
    free(line.p);
    if (!strcmp(r->cls, "error")) {
        const char* msg = field(r->result, "msg");
        mcp_put(err, msg ? msg : "GDB refused the command");
        reply_free(r);
        return -1;
    }
    return 0;
}
static int run(const char* command, Text* err) {
    Reply r;
    if (exec(command, now_ms() + COMMAND_MS, &r, err)) return -1;
    reply_free(&r);
    return 0;
}

/* GDB's output until the game stops or the deadline passes, its console stream into console: 1 stopped, 0 still
   running, -1 GDB gone */
static int wait_stop(long long deadline, Text* console) {
    Text line = {0};
    Reply r;
    int got = 1;
    memset(&r, 0, sizeof r);
    while (g_gdb.running && (got = next_line(&line, deadline)) > 0) take(line.p, &r);
    free(line.p);
    if (r.console.p) mcp_put(console, r.console.p);
    reply_free(&r);
    if (got < 0) close_gdb();
    return got < 0 ? -1 : !g_gdb.running;
}
/* The game stopped. Not with -exec-interrupt: gdbserver hears a remote interrupt by SIGIO on its connection, which
   it arms for a TCP one only, and the stub's is stdio (nx/runtime/gdbstub.c), so GDB would wait for it forever. A
   SIGINT stops the game for gdbserver as a ^C at its terminal would, and GDB keeps it from the game */
static int interrupt(Text* console, Text* err) {
    if (!g_gdb.game || kill(g_gdb.game, SIGINT)) {
        mcp_put(err, "the game's process can't be signalled");
        return -1;
    }
    if (wait_stop(now_ms() + INTERRUPT_MS, console) != 1) {
        mcp_put(err, "the game didn't stop when interrupted");
        return -1;
    }
    return 0;
}

/* s as an MI c-string */
static void put_quoted(Text* t, const char* s) {
    mcp_put(t, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') mcp_put_n(t, "\\", 1);
        if (*s == '\n')
            mcp_put(t, "\\n");
        else
            mcp_put_n(t, s, 1);
    }
    mcp_put(t, "\"");
}

/* expr's value in thread (0: the selected one) as an integer: 0, or -1 with why in err */
static int eval(long thread, const char* expr, SnxU64* value, Text* err) {
    Text command = {0};
    Reply r;
    const char* v;
    int rc;
    mcp_put(&command, "-data-evaluate-expression ");
    if (thread > 0) {
        mcp_put(&command, "--thread ");
        mcp_put_num(&command, (unsigned long)thread);
        mcp_put(&command, " ");
    }
    put_quoted(&command, expr);
    rc = exec(command.p, now_ms() + COMMAND_MS, &r, err);
    free(command.p);
    if (rc) return -1;
    v = field(r.result, "value");
    rc = v && (isdigit((unsigned char)*v) || *v == '-') ? 0 : -1;
    if (!rc)
        *value = (SnxU64)strtoull(v, NULL, 0);
    else {
        mcp_put(err, expr);
        mcp_put(err, " is not a number: ");
        mcp_put(err, v ? v : "?");
    }
    reply_free(&r);
    return rc;
}

/* size bytes of the runtime's memory at address: 0, or -1 with why in err */
static int read_memory(SnxU64 address, size_t size, unsigned char* out, Text* err) {
    char command[96];
    const cJSON *blocks, *first;
    const char *contents, *offset;
    Reply r;
    size_t i;
    int rc = -1;
    snprintf(command, sizeof command, "-data-read-memory-bytes 0x%llx %lu", (unsigned long long)address,
             (unsigned long)size);
    if (exec(command, now_ms() + COMMAND_MS, &r, err)) return -1;
    blocks = cJSON_GetObjectItemCaseSensitive(r.result, "memory");
    first = cJSON_GetArrayItem(blocks, 0);
    contents = field(first, "contents");
    offset = field(first, "offset");
    if (contents && offset && !strtoull(offset, NULL, 0) && strlen(contents) >= 2 * size) {
        for (i = 0; i < size; i++) {
            unsigned v = 0;
            sscanf(contents + 2 * i, "%2x", &v);
            out[i] = (unsigned char)v;
        }
        rc = 0;
    } else {
        char what[96];
        snprintf(what, sizeof what, "%lu bytes at %llx can't all be read", (unsigned long)size,
                 (unsigned long long)address);
        mcp_put(err, what);
    }
    reply_free(&r);
    return rc;
}
static SnxU64 get64(const unsigned char* p) {
    SnxU64 v = 0;
    int i;
    for (i = 7; i >= 0; i--) v = v << 8 | p[i];
    return v;
}
static SnxU32 get32(const unsigned char* p) {
    return (SnxU32)p[0] | (SnxU32)p[1] << 8 | (SnxU32)p[2] << 16 | (SnxU32)p[3] << 24;
}

/* The answer: names after each guest function the recompiled code's symbols mention (A_<address> for a 32-bit game's
   blocks, F_<address> for a 64-bit game's functions), and MAX_LINES lines at most */
static char* finish(Text* t, McpNamer name, const void* ctx) {
    Text out = {0};
    const char* p = t->p ? t->p : "";
    int lines = 0;
    size_t i;
    mcp_put_n(&out, "", 0);
    for (i = 0; p[i]; i++) {
        size_t j = i + 2;
        if (p[i] == '\n' && ++lines >= MAX_LINES && p[i + 1]) {
            int more = 0;
            for (j = i + 1; p[j]; j++) more += p[j] == '\n';
            mcp_put(&out, "\n... ");
            mcp_put_num(&out, (unsigned long)more + 1);
            mcp_put(&out, " more lines\n");
            break;
        }
        /* a source path from the checkout's src/ on: shorter, and without the folders above it (the person's) */
        if (p[i] == '/' && (!i || p[i - 1] == ' ' || p[i - 1] == '(' || p[i - 1] == '=')) {
            size_t end = i + strcspn(p + i, " \t\n:),"), k;
            for (k = i; k + 5 <= end && strncmp(p + k, "/src/", 5); k++) {}
            if (k + 5 <= end && k > i) {
                i = k;
                continue; /* the next byte written is the 's' after this '/' */
            }
        }
        if (name && (p[i] == 'A' || p[i] == 'F') && p[i + 1] == '_' && isxdigit((unsigned char)p[i + 2]) &&
            (!i || (!isalnum((unsigned char)p[i - 1]) && p[i - 1] != '_'))) {
            while (isxdigit((unsigned char)p[j])) j++;
            if (!isalnum((unsigned char)p[j]) && p[j] != '_') {
                mcp_put_n(&out, p + i, j - i);
                mcp_put(&out, " [");
                name(&out, ctx, (SnxU64)strtoull(p + i + 2, NULL, 16));
                mcp_put(&out, "]");
                i = j - 1;
                continue;
            }
        }
        mcp_put_n(&out, p + i, 1);
    }
    free(t->p);
    return out.p;
}
static void put_hex(Text* t, SnxU64 v) {
    char h[24];
    snprintf(h, sizeof h, "%llx", (unsigned long long)v);
    mcp_put(t, h);
}
/* a guest address, named */
static void put_guest(Text* t, SnxU64 a, McpNamer name, const void* ctx) {
    put_hex(t, a);
    if (!name) return;
    mcp_put(t, " [");
    name(t, ctx, a);
    mcp_put(t, "]");
}

static int need_stopped(Text* err) {
    if (!g_gdb.pid) {
        mcp_put(err, "no GDB: gdb_attach connects one");
        return -1;
    }
    if (g_gdb.exited) {
        mcp_put(err, "the game exited: gdb_detach ends the session");
        return -1;
    }
    if (g_gdb.running) {
        mcp_put(err, "the game is running: gdb_command interrupt stops it");
        return -1;
    }
    if (g_gdb.k[K_SIZE] <= 0) {
        mcp_put(err, "the runtime has no debug information: GDB can't find its threads' KThread (build it "
                     "RelWithDebInfo, the default preset)");
        return -1;
    }
    return 0;
}

/* ============================================================ the tools */
char* mcp_gdb_attach(long port, Text* err) {
    static const char* const setup[] = {
        "-gdb-set mi-async on", "-gdb-set pagination off", "-gdb-set confirm off", "-gdb-set width 0",
        "-gdb-set print thread-events off", "-gdb-set debuginfod enabled off", "-gdb-set auto-load off",
        /* the game runs on this machine: its libraries are read from here, not through the remote protocol */
        "-gdb-set sysroot /",
        /* the runtime's own: GPU-owned and frozen guest memory traps, which its handler answers */
        "-interpreter-exec console \"handle SIGSEGV SIGBUS SIGPIPE nostop noprint pass\""};
    int in[2] = {-1, -1}, out[2] = {-1, -1}, i;
    char* gdb;
    char command[64];
    Text t = {0}, line = {0};
    Reply r;
    SnxU64 v;
    long long deadline;
    if (port <= 0 || port > 65535) {
        mcp_put(err, "port: the stub's, 1234 unless the game was started otherwise");
        return NULL;
    }
    if (g_gdb.pid) mcp_gdb_close();
    gdb = snx_find_tool("gdb");
    if (!gdb) {
        mcp_put(err, "gdb is not on PATH");
        return NULL;
    }
    signal(SIGPIPE, SIG_IGN); /* a write to a GDB that died fails instead of ending the server */
    if (pipe(in) || pipe(out)) {
        mcp_put(err, "no pipes for GDB");
        goto failed;
    }
    g_gdb.pid = fork();
    if (g_gdb.pid < 0) {
        g_gdb.pid = 0;
        mcp_put(err, "can't start GDB");
        goto failed;
    }
    if (!g_gdb.pid) {
        setpgid(0, 0); /* a ^C meant for the server stays the server's */
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(out[1], 2);
        close(in[0]);
        close(in[1]);
        close(out[0]);
        close(out[1]);
        execl(gdb, gdb, "--nx", "--quiet", "--interpreter=mi3", (char*)NULL);
        _exit(127);
    }
    free(gdb);
    gdb = NULL;
    close(in[0]);
    close(out[1]);
    g_gdb.to = in[1];
    g_gdb.from = out[0];
    fcntl(g_gdb.to, F_SETFD, FD_CLOEXEC);
    fcntl(g_gdb.from, F_SETFD, FD_CLOEXEC);
    for (i = 0; i < K_COUNT; i++) g_gdb.k[i] = -1;
    deadline = now_ms() + COMMAND_MS;
    do {
        if (next_line(&line, deadline) <= 0) {
            free(line.p);
            mcp_put(err, "GDB didn't start");
            mcp_gdb_close();
            return NULL;
        }
    } while (strncmp(line.p, "(gdb)", 5));
    free(line.p);
    for (i = 0; i < (int)(sizeof setup / sizeof *setup); i++)
        if (run(setup[i], err)) {
            mcp_gdb_close();
            return NULL;
        }
    snprintf(command, sizeof command, "-target-select remote localhost:%ld", port);
    if (exec(command, now_ms() + ATTACH_MS, &r, err)) {
        mcp_put(err, " (is the game running with -S or -s? The stub takes one connection per run)");
        mcp_gdb_close();
        return NULL;
    }
    reply_free(&r);
    g_gdb.running = 0;
    if (!exec("-list-thread-groups", now_ms() + COMMAND_MS, &r, err)) {
        const char* pid = field(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(r.result, "groups"), 0), "pid");
        g_gdb.game = pid ? (pid_t)strtol(pid, NULL, 10) : 0;
        reply_free(&r);
    }
    clear(err);
    mcp_put(&t, "Attached to the game at localhost:");
    mcp_put_num(&t, (unsigned long)port);
    mcp_put(&t, ". It is stopped: gdb_command continue resumes it.\n");
    if (run("-break-insert -f crash_report", err)) {
        mcp_put(&t, "No breakpoint on crash_report: ");
        mcp_put(&t, err->p ? err->p : "?");
        mcp_put(&t, "\n");
        clear(err);
    } else
        mcp_put(&t,
                "Breakpoint 1 is on crash_report: a crash stops there, with the faulting frames below the signal "
                "handler's. SIGSEGV and SIGBUS otherwise pass to the runtime, which traps guest memory with them.\n");
    for (i = 0; i < K_COUNT; i++) {
        char expr[96];
        snprintf(expr, sizeof expr, "(long)(%s)", layout[i]);
        if (eval(0, expr, &v, err)) {
            mcp_put(&t, "The runtime's threads can't be read (");
            mcp_put(&t, err->p ? err->p : "?");
            mcp_put(&t, "): gdb_threads and gdb_guest need its debug information.\n");
            clear(err);
            g_gdb.k[K_SIZE] = -1;
            break;
        }
        g_gdb.k[i] = (long)v;
    }
    return finish(&t, NULL, NULL);
failed:
    free(gdb);
    if (in[0] >= 0) close(in[0]);
    if (in[1] >= 0) close(in[1]);
    if (out[0] >= 0) close(out[0]);
    if (out[1] >= 0) close(out[1]);
    return NULL;
}

/* commands that would reach past the debugger: a shell, scripts, files written */
static int refused(const char* command) {
    static const char* const words[] = {
        "shell", "!",  "pipe",       "|",      "python", "py",   "pi",     "python-interactive",
        "guile", "gu", "guile-repl", "source", "make",   "dump", "append", "save"};
    size_t i, n;
    while (*command == ' ' || *command == '\t') command++;
    n = strcspn(command, " \t");
    if (n && (command[0] == '!' || command[0] == '|')) return 1;
    for (i = 0; i < sizeof words / sizeof *words; i++)
        if (strlen(words[i]) == n && !strncmp(command, words[i], n)) return 1;
    return 0;
}

char* mcp_gdb_command(const char* command, long wait, McpNamer name, const void* ctx, Text* err) {
    Text t = {0}, mi = {0};
    Reply r;
    const char* c = command;
    int rc;
    if (!g_gdb.pid) {
        mcp_put(err, "no GDB: gdb_attach connects one");
        return NULL;
    }
    if (refused(command)) {
        mcp_put(err, "the GDB tools don't run shells, scripts or commands that write files");
        return NULL;
    }
    while (*c == ' ') c++;
    mcp_put_n(&t, "", 0);
    if (!strcmp(c, "interrupt") || !strcmp(c, "interrupt -a")) {
        if (!g_gdb.running) {
            mcp_put(err, "the game is stopped already");
            free(t.p);
            return NULL;
        }
        if (interrupt(&t, err)) {
            free(t.p);
            return NULL;
        }
    } else {
        mcp_put(&mi, "-interpreter-exec console ");
        put_quoted(&mi, command);
        g_gdb.stop.n = 0;
        rc = exec(mi.p, now_ms() + COMMAND_MS, &r, err);
        free(mi.p);
        if (rc) {
            free(t.p);
            return NULL;
        }
        mcp_put(&t, r.console.p);
        reply_free(&r);
        if (g_gdb.running && wait > 0) {
            rc = wait_stop(now_ms() + wait * 1000, &t);
            if (rc < 0) {
                mcp_put(err, "GDB is gone");
                free(t.p);
                return NULL;
            }
            if (!rc) {
                mcp_put(&t, "\nStill running after ");
                mcp_put_num(&t, (unsigned long)wait);
                mcp_put(&t, " s: interrupted.\n");
                if (interrupt(&t, err)) {
                    free(t.p);
                    return NULL;
                }
            }
        }
    }
    if (g_gdb.running)
        mcp_put(&t, "\nThe game is running: gdb_command interrupt stops it.\n");
    else if (g_gdb.stop.n) {
        mcp_put(&t, t.n && t.p[t.n - 1] != '\n' ? "\n" : "");
        mcp_put(&t, g_gdb.stop.p);
        mcp_put(&t, "\n");
        g_gdb.stop.n = 0;
    }
    return finish(&t, name, ctx);
}

/* thread's KThread: 1 and its bytes, 0 for a thread of the runtime's own (no tls_thread), -1 with why in err */
static int kthread(long thread, SnxU64* at, unsigned char* k, Text* err) {
    size_t size = (size_t)g_gdb.k[K_SIZE] < THREAD_BYTES ? (size_t)g_gdb.k[K_SIZE] : THREAD_BYTES;
    if (eval(thread, "(unsigned long)tls_thread", at, err)) return -1;
    if (!*at) return 0;
    return read_memory(*at, size, k, err) ? -1 : 1;
}
static int is_wide(Text* err) {
    SnxU64 arm;
    return eval(0, "(unsigned long)g_arm_mod", &arm, err) ? -1 : !arm;
}
static long k_off(int member, size_t index, size_t width) {
    long off = g_gdb.k[member] + (long)(index * width);
    return off + (long)width <= g_gdb.k[K_SIZE] && off + (long)width <= THREAD_BYTES ? off : -1;
}
static SnxU64 k64(const unsigned char* k, int member, size_t index) {
    long off = k_off(member, index, 8);
    return off < 0 ? 0 : get64(k + off);
}
static SnxU32 k32(const unsigned char* k, int member, size_t index) {
    long off = k_off(member, index, 4);
    return off < 0 ? 0 : get32(k + off);
}
static void put_kname(Text* t, const unsigned char* k) {
    char name[33];
    long off = k_off(K_NAME, 0, 32);
    memcpy(name, off < 0 ? (const unsigned char*)"" : k + off, off < 0 ? 1 : 32);
    name[32] = 0;
    mcp_put(t, "'");
    mcp_put(t, name);
    mcp_put(t, "'");
}
/* where a guest thread is: in an SVC (its registers in the KThread exact), or running game code */
static void put_where(Text* t, const unsigned char* k) {
    SnxU32 state = k32(k, K_STATE, 0);
    if (state == 0)
        mcp_put(t, "created, not started");
    else if (state == 2)
        mcp_put(t, "exited");
    else if (k32(k, K_IN_SVC, 0))
        mcp_put(t, "in an SVC (its registers are exact)");
    else
        mcp_put(t, "running game code (the recompiled code keeps registers on the host between calls into the "
                   "runtime: these may be stale)");
}

/* where a host thread is: its innermost frame with a source file and that frame's caller (the innermost is mostly a
   system call in a library without symbols), else the innermost */
static void put_host(Text* t, const char* id, const cJSON* top) {
    char command[64];
    const cJSON *stack, *f, *found = NULL;
    Text ignored = {0};
    Reply r;
    snprintf(command, sizeof command, "-stack-list-frames --thread %s 0 7", id);
    if (exec(command, now_ms() + COMMAND_MS, &r, &ignored)) {
        free(ignored.p);
        put_frame(t, top);
        return;
    }
    stack = cJSON_GetObjectItemCaseSensitive(r.result, "stack");
    cJSON_ArrayForEach(f, stack) {
        if (!field(f, "file")) continue;
        if (found) {
            mcp_put(t, " < ");
            put_frame(t, f);
            break;
        }
        found = f;
        put_frame(t, f);
    }
    if (!found) put_frame(t, top);
    reply_free(&r);
}

char* mcp_gdb_threads(McpNamer name, const void* ctx, Text* err) {
    unsigned char k[THREAD_BYTES];
    Text t = {0};
    Reply r;
    const cJSON *threads, *th;
    int wide, guests = 0, hosts = 0;
    if (need_stopped(err)) return NULL;
    wide = is_wide(err);
    if (wide < 0 || exec("-thread-info", now_ms() + COMMAND_MS, &r, err)) return NULL;
    threads = cJSON_GetObjectItemCaseSensitive(r.result, "threads");
    mcp_put_n(&t, "", 0);
    cJSON_ArrayForEach(th, threads) {
        const char *id = field(th, "id"), *target = field(th, "target-id"), *tname = field(th, "name");
        SnxU64 at;
        int got;
        if (!id) continue;
        got = kthread(strtol(id, NULL, 10), &at, k, err);
        if (got < 0) {
            clear(err);
            got = 0;
        }
        mcp_put(&t, id);
        mcp_put(&t, " ");
        mcp_put(&t, target ? target : "?");
        if (tname) {
            mcp_put(&t, " '");
            mcp_put(&t, tname);
            mcp_put(&t, "'");
        }
        if (got) {
            SnxU64 pc = wide ? k64(k, K_PC, 0) : k32(k, K_R, 15), lr = wide ? k64(k, K_X, 30) : k32(k, K_R, 14);
            guests++;
            mcp_put(&t, ": guest thread ");
            mcp_put_num(&t, (unsigned long)k64(k, K_ID, 0));
            mcp_put(&t, " ");
            put_kname(&t, k);
            mcp_put(&t, ", ");
            put_where(&t, k);
            mcp_put(&t, "\n    pc=");
            put_guest(&t, pc, name, ctx);
            mcp_put(&t, " lr=");
            put_guest(&t, lr, name, ctx);
            mcp_put(&t, "\n    host at ");
        } else {
            hosts++;
            mcp_put(&t, ": the runtime's, at ");
        }
        put_host(&t, id, cJSON_GetObjectItemCaseSensitive(th, "frame"));
        mcp_put(&t, "\n");
    }
    reply_free(&r);
    mcp_put_num(&t, (unsigned long)guests);
    mcp_put(&t, " guest threads, ");
    mcp_put_num(&t, (unsigned long)hosts);
    mcp_put(&t, " of the runtime's. gdb_guest <id> shows one's registers and guest backtrace.\n");
    return finish(&t, name, ctx);
}

char* mcp_gdb_guest(long thread, int frames, McpNamer name, const void* ctx, Text* err) {
    unsigned char k[THREAD_BYTES], record[16];
    Text t = {0};
    SnxU64 at, mem, fp, pc, lr;
    int wide, got, i, n = 0;
    if (need_stopped(err)) return NULL;
    wide = is_wide(err);
    if (wide < 0) return NULL;
    if (thread <= 0) {
        SnxU64 current;
        if (eval(0, "$_thread", &current, err)) return NULL;
        thread = (long)current;
    }
    got = kthread(thread, &at, k, err);
    if (got < 0) return NULL;
    if (!got) {
        mcp_put(err, "not a guest thread: one of the runtime's own (gdb_threads lists which are guest threads)");
        return NULL;
    }
    if (eval(0, "(unsigned long)g_mem", &mem, err)) return NULL;
    mcp_put(&t, "guest thread ");
    mcp_put_num(&t, (unsigned long)k64(k, K_ID, 0));
    mcp_put(&t, " ");
    put_kname(&t, k);
    mcp_put(&t, " (GDB thread ");
    mcp_put_num(&t, (unsigned long)thread);
    mcp_put(&t, "), ");
    put_where(&t, k);
    mcp_put(&t, "\n");
    for (i = 0; i < (wide ? 31 : 16); i++) {
        char reg[48];
        if (wide)
            snprintf(reg, sizeof reg, "x%-2d=%016llx", i, (unsigned long long)k64(k, K_X, (size_t)i));
        else
            snprintf(reg, sizeof reg, "r%-2d=%08x", i, (unsigned)k32(k, K_R, (size_t)i));
        mcp_put(&t, reg);
        mcp_put(&t, i % 4 == 3 ? "\n" : " ");
    }
    if (wide) {
        char reg[96];
        snprintf(reg, sizeof reg, "\nsp=%llx nzcv=%c%c%c%c tpidrro=%llx\n", (unsigned long long)k64(k, K_X, 31),
                 k32(k, K_N, 0) ? 'N' : 'n', k32(k, K_N, 1) ? 'Z' : 'z', k32(k, K_N, 2) ? 'C' : 'c',
                 k32(k, K_N, 3) ? 'V' : 'v', (unsigned long long)k64(k, K_TPIDRRO, 0));
        mcp_put(&t, reg);
        pc = k64(k, K_PC, 0);
        lr = k64(k, K_X, 30);
        fp = k64(k, K_X, 29);
    } else {
        char reg[48];
        SnxU32 cpsr = k32(k, K_CPSR, 0);
        snprintf(reg, sizeof reg, "cpsr=%08x (%s)\n", (unsigned)cpsr, cpsr & 0x20 ? "Thumb" : "A32");
        mcp_put(&t, reg);
        pc = k32(k, K_R, 15);
        lr = k32(k, K_R, 14);
        fp = k32(k, K_R, cpsr & 0x20 ? 7 : 11);
    }
    /* the frame records fp points at, the caller's fp and then the return address, as hle/kernel/debug.c walks them */
    mcp_put(&t, "pc ");
    put_guest(&t, pc, name, ctx);
    mcp_put(&t, "\nlr ");
    put_guest(&t, lr, name, ctx);
    mcp_put(&t, "\nframes from ");
    mcp_put(&t, wide ? "x29" : "the frame pointer");
    mcp_put(&t, ":\n");
    if (frames <= 0 || frames > MAX_FRAMES) frames = MAX_FRAMES;
    while (n < frames && fp && !(fp & (wide ? 7 : 3))) {
        SnxU64 next;
        if (read_memory(mem + fp, wide ? 16 : 8, record, err)) {
            mcp_put(&t, "  (");
            mcp_put(&t, err->p);
            mcp_put(&t, ")\n");
            clear(err);
            break;
        }
        next = wide ? get64(record) : get32(record);
        lr = wide ? get64(record + 8) : get32(record + 4);
        if (!lr) break;
        mcp_put(&t, "  #");
        mcp_put_num(&t, (unsigned long)++n);
        mcp_put(&t, " ");
        put_guest(&t, lr, name, ctx);
        mcp_put(&t, "\n");
        if (next <= fp) break; /* frames go up the stack */
        fp = next;
    }
    if (!n) mcp_put(&t, "  (none)\n");
    return finish(&t, NULL, NULL);
}

char* mcp_gdb_memory(SnxU64 address, long size, Text* err) {
    unsigned char bytes[MAX_MEMORY];
    Text t = {0};
    SnxU64 mem;
    long i, j;
    if (need_stopped(err)) return NULL;
    if (size <= 0) size = 256;
    if (size > MAX_MEMORY) size = MAX_MEMORY;
    if (eval(0, "(unsigned long)g_mem", &mem, err)) return NULL;
    if (!mem) {
        mcp_put(err, "guest memory isn't mapped yet");
        return NULL;
    }
    if (read_memory(mem + address, (size_t)size, bytes, err)) return NULL;
    for (i = 0; i < size; i += 16) {
        char line[96];
        int n = snprintf(line, sizeof line, "%010llx ", (unsigned long long)(address + (SnxU64)i));
        for (j = i; j < i + 16; j++)
            n += snprintf(line + n, sizeof line - (size_t)n, j < size ? " %02x" : "   ", bytes[j < size ? j : 0]);
        n += snprintf(line + n, sizeof line - (size_t)n, "  ");
        for (j = i; j < i + 16 && j < size; j++) line[n++] = bytes[j] >= 0x20 && bytes[j] < 0x7f ? (char)bytes[j] : '.';
        line[n++] = '\n';
        line[n] = 0;
        mcp_put(&t, line);
    }
    return finish(&t, NULL, NULL);
}

char* mcp_gdb_detach(Text* err) {
    Text t = {0};
    if (!g_gdb.pid) {
        mcp_put(err, "no GDB: gdb_attach connects one");
        return NULL;
    }
    mcp_gdb_close();
    mcp_put(&t, "Detached: the game runs on without the debugger. The stub takes one connection per run, so attaching "
                "again needs a new run.\n");
    return t.p;
}

void mcp_gdb_close(void) {
    Text ignored = {0};
    if (!g_gdb.pid) return;
    if (!g_gdb.exited) {
        if (g_gdb.running) interrupt(&ignored, &ignored);
        if (g_gdb.pid) run("-target-detach", &ignored);
    }
    if (g_gdb.pid) write_all(g_gdb.to, "-gdb-exit\n", 10);
    free(ignored.p);
    close_gdb();
}
#endif
