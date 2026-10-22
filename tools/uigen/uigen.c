/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* uigen: generates the launcher's scenes from src/gui/ui.txt. gui/ui.h has the items' types, each scene's items by
   name, its functions and the prototype of every callback the file names; gui/ui.c has each scene's items as a table
   in .rodata, its layout as one flat function of the area it is given and the UI scale, its drawing, its hit test, the
   actions a hit or a hotkey calls, and the stubs of the items marked @. A scene's rectangles are fixed by the file and
   computed here, at build time: each edge is a fraction of the area plus pixels times the scale. An item's text is an
   offset into its scene's array of characters, never a pointer: a table of pointers goes to .data.rel.ro in a PIE or a
   shared library. The callbacks and the drawing are main.c's and are linked: one the file names and nothing defines
   fails the link. It is strict C99 and depends on nothing, so a cross build can build it for the machine it runs on.

   Usage: uigen --out DIR FILE
   It writes DIR/gui/ui.h and DIR/gui/ui.c, each only rewritten when it changes. An error names the file and the line
   it is on.

   The format, a line at a time, indented with tabs (a tab a level; spaces are refused), # starting a comment:

       [scene]Main {                     a scene: its items laid out as a flexbox lays out its children
       	padding = 8px
       	[list]LoadList {                  a list: a row each of its items, or without any, its rows from callbacks
       		max_rows = 8
       		[button]"Load 3DS CIA" {}         a button, named by its English text: ui_main_load_list_load_3ds_cia
       		@[button]"Decor" {}               @: the generator defines its callbacks, which do nothing
       	}
       	[label]Status {}                  a label: its "Text", or a Name and its text from a callback
       }

   Each type takes its own properties, word = value, and refuses any other. Lengths are Npx (pixels at a UI scale of
   1x, multiplied by the scale) or N% (of the parent's inside, 0 to 100).
     scene   direction (column or row: the main axis), gap (px between its items), padding (px around them), align
             (stretch, start, center or end: across), justify (start, center or end: along, when nothing grows)
     list    width, height, grow (its share of the free length along its scene's axis; 1 when it has no length),
             max_rows (1 to 64, 8 by default: it shows the rows that fit, those at most), row_height (17px by
             default), row_gap (2px by default, under each row), scroll (center, the default, or edge: where the
             row in focus stays as the list moves), min_row_width and max_row_width (a row's width kept between
             them), attr, color
     button  width, height, grow (17px along, the whole inside across, by default), attr, color, key, for-platform
     label   as a button, without attr and key
   An item of a list is a row: it takes attr, color, key and for-platform alone, and shows its "Text". attr = callback:
   the code gives the button or row its UI_ATTR_* bits as it is drawn (failure, blink, active); on a list with items,
   any row's by its index. for-platform (android or desktop) is a row's alone: a scene's items keep their places on
   every platform, though an item's width and height, and a list's row_height, row_gap and max_rows, may take a
   platform's own value (android.row_height = 40px; desktop. too), the scene laid out for each platform. color is a theme colour by its theme.ini name. key is an ASCII character or f1 to f12 after ctrl-
   or alt- (alt-f4), a hotkey of its scene (Enter is the click's).
   The callbacks, main.c's: a button's ui_<symbol>_action_cb(App*, uint32_t hit); a named label's
   ui_<symbol>_update_cb(App*, char* text, size_t size); a list without items' ui_<symbol>_get_row_info(App*, size_t
   row, UiRowInfo*) (false past its last row: the row and those after it are not shown; its label is used before
   the next call, so a buffer of the code's does) and
   ui_<symbol>_row_action_cb(App*, size_t row, uint32_t hit); with attr = callback, unsigned ui_<symbol>_attr_cb(App*),
   a list's with size_t row after it.
   A hover calls the same action as a click: hit has UI_HIT_HOVER, and UI_HIT_CLICK or UI_HIT_RELEASE when one made
   it, and the callback tests for the event it acts on. The drawing, main.c's too: ui_draw_row, which draws a theme's
   cursor and the arrows of hidden rows (UI_ATTR_MORE_UP, UI_ATTR_MORE_DOWN) inside the row, and ui_draw_label. */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ITEMS 1024
#define MAX_DEPTH 16

enum { T_SCENE, T_LIST, T_BUTTON, T_LABEL, T_TYPES };
static const char* const type_words[T_TYPES] = {"scene", "list", "button", "label"};
static const char* const type_enums[T_TYPES] = {"UI_SCENE", "UI_LIST", "UI_BUTTON", "UI_LABEL"};
/* what each type holds, a bit per type */
static const unsigned holds[T_TYPES] = {1u << T_LIST | 1u << T_BUTTON | 1u << T_LABEL, 1u << T_BUTTON | 1u << T_LABEL,
                                        0, 0};

/* the properties, by the kind of value each takes */
enum {
    P_WIDTH, /* lengths: Npx or N% */
    P_HEIGHT,
    P_MIN_ROW_WIDTH,
    P_MAX_ROW_WIDTH,
    P_GAP, /* pixels: Npx */
    P_PADDING,
    P_ROW_HEIGHT,
    P_ROW_GAP,
    P_GROW, /* whole numbers */
    P_MAX_ROWS,
    P_DIRECTION, /* words */
    P_ALIGN,
    P_JUSTIFY,
    P_SCROLL,
    P_ATTR,
    P_COLOR,
    P_KEY,
    P_PLATFORM,
    P_COUNT
};
#define LENGTHS_END (P_MAX_ROW_WIDTH + 1)
#define PIXELS_END  (P_ROW_GAP + 1)
#define NUMBERS_END (P_MAX_ROWS + 1)
#define WORDS_END   (P_ATTR + 1)
static const char* const property_words[P_COUNT] = {
    "width",      "height",  "min_row_width", "max_row_width", "gap",       "padding",
    "row_height", "row_gap", "grow",          "max_rows",      "direction", "align",
    "justify",    "scroll",  "attr",          "color",         "key",       "for-platform"};
/* the whole numbers' bounds */
static const int number_lo[2] = {0, 1}, number_hi[2] = {100, 64};
/* the words' values, a list each, their index the value */
static const char* const directions[] = {"column", "row", NULL};
static const char* const aligns[] = {"stretch", "start", "center", "end", NULL};
static const char* const justifies[] = {"start", "center", "end", NULL};
static const char* const scrolls[] = {"center", "edge", NULL};
static const char* const attrs[] = {"callback", NULL};
static const char* const* const word_values[5] = {directions, aligns, justifies, scrolls, attrs};
enum { COLUMN, ROW };
enum { STRETCH, A_START, A_CENTER, A_END };
enum { J_START, J_CENTER, J_END };
enum { SCROLL_CENTER, SCROLL_EDGE };
#define BIT(p) (1u << (p))
#define BOX    (BIT(P_WIDTH) | BIT(P_HEIGHT) | BIT(P_GROW))
/* the properties each type takes; a row of a list takes ROW_TAKES of its type's alone */
static const unsigned properties_of[T_TYPES] = {
    BIT(P_DIRECTION) | BIT(P_ALIGN) | BIT(P_JUSTIFY) | BIT(P_GAP) | BIT(P_PADDING),
    BOX | BIT(P_MAX_ROWS) | BIT(P_ROW_HEIGHT) | BIT(P_ROW_GAP) | BIT(P_SCROLL) | BIT(P_MIN_ROW_WIDTH) |
        BIT(P_MAX_ROW_WIDTH) | BIT(P_ATTR) | BIT(P_COLOR),
    BOX | BIT(P_ATTR) | BIT(P_COLOR) | BIT(P_KEY) | BIT(P_PLATFORM), BOX | BIT(P_COLOR) | BIT(P_PLATFORM)};
#define ROW_TAKES  (BIT(P_ATTR) | BIT(P_COLOR) | BIT(P_KEY) | BIT(P_PLATFORM))
/* the defaults, in pixels at 1x and rows: a row as main.c's lists have it (a cell of text and a pixel), 2px under
   each (Liz's) */
#define ROW_HEIGHT 17
#define ROW_GAP    2
#define MAX_ROWS   8

/* the theme's colours (theme.h's GuiTheme), by the names theme.ini gives them */
static const char* const colors[] = {"page", "ink", "soft", "focus", "active", "failure", "dim", "warning", "added"};
#define COLORS (int)(sizeof colors / sizeof colors[0])
enum { PLATFORM_ANY, PLATFORM_ANDROID, PLATFORM_DESKTOP };

enum { UNIT_UNSET, UNIT_PX, UNIT_PERCENT };
enum { MOD_CTRL = 1, MOD_ALT = 2 };

/* a length along one of the area's axes: a * the area's length + c pixels at 1x (times the scale) */
typedef struct {
    double a, c;
} Lin;

typedef struct {
    int type, parent, scene, line, stub, depth;
    char *name, *text, *symbol, *upper; /* upper: its enum's name */
    int value[P_COUNT], unit[P_COUNT];  /* a length's unit, else UNIT_UNSET */
    unsigned set;                       /* the properties given, a bit each */
    int color, platform;                /* color: an index in colors + 1, 0 for none */
    int key, fkey, mods;                /* key: an ASCII character; fkey: 1 to 12 for F1 to F12 */
    int key_line;
    int plat_value[3][P_COUNT]; /* a platform's own values (android.height), by PLATFORM_* */
    int plat_unit[3][P_COUNT];
    unsigned plat_set[3];
    int children, offset;
    Lin x[3], y[3], w[3], h[3]; /* its rectangle in its scene's area, a scene's own item, by PLATFORM_* */
} Item;

static struct {
    Item items[MAX_ITEMS];
    int nitems;
    const char* spec_path;
    const char* out_path; /* the file being written, for #line */
    char* obuf;
    size_t olen, ocap;
    int olines; /* the output's newlines so far */
    int away;   /* the lines written last came from ui.txt's (#line) */
} g_uigen;

static void fail(int line, const char* fmt, ...) {
    va_list ap;
    if (line)
        fprintf(stderr, "%s:%d: ", g_uigen.spec_path, line);
    else
        fprintf(stderr, "uigen: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void* alloc(size_t n) {
    void* p = malloc(n ? n : 1);
    if (!p) fail(0, "out of memory");
    return p;
}
static char* dup_n(const char* s, size_t n) {
    char* d = (char*)alloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}
static char* dup(const char* s) { return dup_n(s, strlen(s)); }
static char* cat3(const char* a, const char* b, const char* c) {
    size_t na = strlen(a), nb = strlen(b), nc = strlen(c);
    char* d = (char*)alloc(na + nb + nc + 1);
    memcpy(d, a, na);
    memcpy(d + na, b, nb);
    memcpy(d + na + nb, c, nc + 1);
    return d;
}

/* ============================================================ output */

static void outs_n(const char* s, size_t n) {
    size_t i;
    if (g_uigen.olen + n + 1 > g_uigen.ocap) {
        g_uigen.ocap = (g_uigen.olen + n + 1) * 2;
        g_uigen.obuf = (char*)realloc(g_uigen.obuf, g_uigen.ocap);
        if (!g_uigen.obuf) fail(0, "out of memory");
    }
    memcpy(g_uigen.obuf + g_uigen.olen, s, n);
    g_uigen.olen += n;
    g_uigen.obuf[g_uigen.olen] = 0;
    for (i = 0; i < n; i++) g_uigen.olines += s[i] == '\n';
}
static void outs(const char* s) { outs_n(s, strlen(s)); }
static void out(const char* fmt, ...) {
    va_list ap;
    char small[512], *text = small;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n < 0) fail(0, "cannot format %s", fmt);
    if ((size_t)n >= sizeof small) {
        text = (char*)alloc((size_t)n + 1);
        va_start(ap, fmt);
        vsnprintf(text, (size_t)n + 1, fmt, ap);
        va_end(ap);
    }
    outs_n(text, (size_t)n);
    if (text != small) free(text);
}
/* s as a C string literal */
static void out_string(const char* s) {
    outs("\"");
    for (; *s; s++)
        if (*s == '"' || *s == '\\') {
            char c[3] = {'\\', *s, 0};
            outs(c);
        } else
            outs_n(s, 1);
    outs("\"");
}
/* the next line came from that line of ui.txt, which a compiler's message then names; out_here, before lines of the
   generator's own, goes back to the generated file's */
static void out_from(int line) {
    out("#line %d ", line);
    out_string(g_uigen.spec_path);
    outs("\n");
    g_uigen.away = 1;
}
static void out_here(void) {
    if (!g_uigen.away) return;
    out("#line %d ", g_uigen.olines + 2);
    out_string(g_uigen.out_path);
    outs("\n");
    g_uigen.away = 0;
}
/* an item's lines kept on the platform it is for */
static void out_platform(const Item* it) {
    if (it->platform != PLATFORM_ANY) out_here();
    if (it->platform == PLATFORM_ANDROID) outs("#ifdef SDL_PLATFORM_ANDROID\n");
    if (it->platform == PLATFORM_DESKTOP) outs("#if !defined(SDL_PLATFORM_ANDROID) && !defined(SDL_PLATFORM_IOS)\n");
}
static void out_platform_end(const Item* it) {
    if (it->platform == PLATFORM_ANY) return;
    out_here();
    outs("#endif\n");
}

/* written only when it changes, so what includes it recompiles only then (Ninja's restat) */
static void flush(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) {
        char* old = (char*)alloc(g_uigen.olen + 1);
        size_t got = fread(old, 1, g_uigen.olen + 1, f);
        int same = got == g_uigen.olen && !memcmp(old, g_uigen.obuf, g_uigen.olen);
        free(old);
        fclose(f);
        if (same) {
            g_uigen.olen = 0;
            g_uigen.olines = 0;
            g_uigen.away = 0;
            return;
        }
    }
    f = fopen(path, "wb");
    if (!f || fwrite(g_uigen.obuf, 1, g_uigen.olen, f) != g_uigen.olen || fclose(f)) fail(0, "cannot write %s", path);
    g_uigen.olen = 0;
    g_uigen.olines = 0;
    g_uigen.away = 0;
}

static char* read_spec(void) {
    FILE* f = fopen(g_uigen.spec_path, "rb");
    char* text;
    long n = 0;
    if (!f || fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET))
        fail(0, "cannot read %s", g_uigen.spec_path);
    text = (char*)alloc((size_t)n + 1);
    if (fread(text, 1, (size_t)n, f) != (size_t)n) fail(0, "cannot read %s", g_uigen.spec_path);
    text[n] = 0;
    fclose(f);
    return text;
}

/* ============================================================ names */

/* a name or an English text in snake_case: letters and digits lowercased, every other run an underscore, none at
   either end; a camel-case word split where its case changes (LoadList: load_list) */
static char* snake(const char* name, int line) {
    size_t n = strlen(name), i, o = 0;
    char* s = (char*)alloc(2 * n + 2);
    int gap = 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!isalnum(c)) {
            gap = 1;
            continue;
        }
        if (o && isupper(c) && i > 0 &&
            (islower((unsigned char)name[i - 1]) ||
             (isupper((unsigned char)name[i - 1]) && i + 1 < n && islower((unsigned char)name[i + 1]))))
            gap = 1;
        if (gap && o) s[o++] = '_';
        gap = 0;
        s[o++] = (char)tolower(c);
    }
    s[o] = 0;
    if (!o) fail(line, "\"%s\" has no letter or digit to name its symbol by", name);
    return s;
}
static char* upper(const char* s) {
    char* u = dup(s);
    char* p;
    for (p = u; *p; p++) *p = (char)toupper((unsigned char)*p);
    return u;
}

/* ============================================================ ui.txt */

/* the property's word, or -1 */
static int property_of(const char* word) {
    int p;
    for (p = 0; p < P_COUNT; p++)
        if (!strcmp(property_words[p], word)) return p;
    return -1;
}
/* the words a set of properties has, for an error */
static char* words_of(unsigned taken) {
    char* list = dup("");
    int p;
    for (p = 0; p < P_COUNT; p++)
        if (taken & BIT(p)) {
            char* grown = cat3(list, *list ? ", " : "", property_words[p]);
            free(list);
            list = grown;
        }
    if (!*list) {
        free(list);
        list = dup("none");
    }
    return list;
}
static int is_row(const Item* it) { return it->parent >= 0 && g_uigen.items[it->parent].type == T_LIST; }

/* 200px or 50% (lengths), 6px (pixels) */
static void parse_length(Item* it, int p, const char* value, int line) {
    char* end;
    long n = strtol(value, &end, 10);
    if (end == value || !isdigit((unsigned char)*value))
        fail(line, "%s = %s: a length is a whole number of px%s", property_words[p], value,
             p < LENGTHS_END ? ", or a % of its parent" : "");
    if (!strcmp(end, "px")) {
        if (n > 10000) fail(line, "%s = %s: px from 0 to 10000", property_words[p], value);
        it->unit[p] = UNIT_PX;
    } else if (!strcmp(end, "%") && p < LENGTHS_END) {
        if (n > 100) fail(line, "%s = %s: a %% of its parent is 0 to 100", property_words[p], value);
        it->unit[p] = UNIT_PERCENT;
    } else
        fail(line, "%s = %s: %s", property_words[p], value,
             p < LENGTHS_END ? "a length ends in px or %" : "pixels end in px");
    it->value[p] = (int)n;
}
/* ascii, f1 to f12, after ctrl- and alt- */
static void parse_key(Item* it, const char* value, int line) {
    const char* k = value;
    for (;;) {
        int mod = !strncmp(k, "ctrl-", 5) ? MOD_CTRL : !strncmp(k, "alt-", 4) ? MOD_ALT : 0;
        if (!mod || !k[mod == MOD_CTRL ? 5 : 4]) break; /* a key of its own after the modifier, else the key is "-" */
        if (it->mods & mod) fail(line, "key = %s: %s twice", value, mod == MOD_CTRL ? "ctrl" : "alt");
        it->mods |= mod;
        k += mod == MOD_CTRL ? 5 : 4;
    }
    if ((k[0] == 'f' || k[0] == 'F') && isdigit((unsigned char)k[1])) {
        char* end;
        long f = strtol(k + 1, &end, 10);
        if (*end || f < 1 || f > 12) fail(line, "key = %s: the function keys are f1 to f12", value);
        it->fkey = (int)f;
    } else if (k[0] > ' ' && k[0] < 127 && !k[1])
        it->key = tolower((unsigned char)k[0]); /* the key, not its shifted letter */
    else
        fail(line,
             "key = %s: one ASCII character or f1 to f12, after ctrl- or alt- (Enter is the click's, not a hotkey)",
             value);
    it->key_line = line;
}
static void set_property(Item* it, char* word, char* value, int line);
/* a platform's own width, height, or list's row metric, android.row_height = 40px: checked as the plain one is, its
   layout and rows take it there */
static void set_platform_property(Item* it, int on, char* word, char* value, int line) {
    static const char* const platforms[] = {"", "android", "desktop"};
    unsigned own =
        BIT(P_WIDTH) | BIT(P_HEIGHT) | (it->type == T_LIST ? BIT(P_ROW_HEIGHT) | BIT(P_ROW_GAP) | BIT(P_MAX_ROWS) : 0);
    int p = property_of(word);
    Item checked = {0};
    if (it->type == T_SCENE || p < 0 || !(own & BIT(p)))
        fail(line, "%s.%s: a platform's own value is a width, a height, or a list's row_height, row_gap or max_rows",
             platforms[on], word);
    if (it->plat_set[on] & BIT(p))
        fail(line, "%s.%s is set twice for one %s", platforms[on], word, type_words[it->type]);
    checked.type = it->type;
    checked.parent = it->parent;
    set_property(&checked, word, value, line);
    it->plat_value[on][p] = checked.value[p];
    it->plat_unit[on][p] = checked.unit[p];
    it->plat_set[on] |= BIT(p);
}
static void set_property(Item* it, char* word, char* value, int line) {
    int p, i;
    unsigned taken;
    if (!strncmp(word, "android.", 8) || !strncmp(word, "desktop.", 8)) {
        set_platform_property(it, word[0] == 'a' ? PLATFORM_ANDROID : PLATFORM_DESKTOP, word + 8, value, line);
        return;
    }
    p = property_of(word);
    taken = is_row(it) ? properties_of[it->type] & ROW_TAKES : properties_of[it->type];
    if (p < 0 || !(taken & BIT(p))) {
        char* list = words_of(taken);
        fail(line, "%s is not a property of a %s%s: it takes %s", word, is_row(it) ? "row's " : "",
             type_words[it->type], list);
    }
    if (it->set & BIT(p)) fail(line, "%s is set twice for one %s", word, type_words[it->type]);
    if (!*value) fail(line, "%s = without its value", word);
    it->set |= BIT(p);
    if (p < PIXELS_END)
        parse_length(it, p, value, line);
    else if (p < NUMBERS_END) {
        char* end;
        long n = strtol(value, &end, 10);
        int lo = number_lo[p - PIXELS_END], hi = number_hi[p - PIXELS_END];
        if (end == value || *end || n < lo || n > hi)
            fail(line, "%s = %s: a whole number from %d to %d", word, value, lo, hi);
        it->value[p] = (int)n;
    } else if (p < WORDS_END) {
        const char* const* words = word_values[p - NUMBERS_END];
        char list[64] = "";
        for (i = 0; words[i] && strcmp(words[i], value); i++) continue;
        if (!words[i]) {
            for (i = 0; words[i]; i++) {
                strcat(list, i ? (words[i + 1] ? ", " : " or ") : "");
                strcat(list, words[i]);
            }
            fail(line, "%s = %s: it is %s", word, value, list);
        }
        it->value[p] = i;
    } else if (p == P_COLOR) {
        for (i = 0; i < COLORS && strcmp(colors[i], value); i++) continue;
        if (i == COLORS)
            fail(line,
                 "color = %s: the theme's colours are page, ink, soft, focus, active, failure, dim, warning, added",
                 value);
        it->color = i + 1;
    } else if (p == P_PLATFORM) {
        if (!strcmp(value, "android"))
            it->platform = PLATFORM_ANDROID;
        else if (!strcmp(value, "desktop"))
            it->platform = PLATFORM_DESKTOP;
        else
            fail(line, "for-platform = %s: the platforms are android and desktop", value);
    } else
        parse_key(it, value, line);
}

/* a quoted text at *s, its escapes \" and \\ taken; *s moves past its closing quote */
static char* quoted(char** s, int line) {
    char *p = *s + 1, *text = (char*)alloc(strlen(*s)), *o = text;
    for (; *p && *p != '"'; p++) {
        if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) p++;
        *o++ = *p;
    }
    if (*p != '"') fail(line, "a text without its closing \"");
    *o = 0;
    if (!*text) fail(line, "an empty text: an item shows its text, or a label has a name instead");
    *s = p + 1;
    return text;
}
static int item_with_symbol(const char* symbol) {
    int i;
    for (i = 0; i < g_uigen.nitems; i++)
        if (!strcmp(g_uigen.items[i].symbol, symbol)) return i;
    return -1;
}

/* @[type]Name { or @[type]"Text" {, maybe closed on its line ({}); its parent the block it is in */
static int parse_opener(char* p, int parent, int depth, int line, int* closed) {
    Item* it;
    char *close = NULL, *rest;
    int type;
    if (g_uigen.nitems == MAX_ITEMS) fail(line, "more than %d items", MAX_ITEMS);
    it = &g_uigen.items[g_uigen.nitems];
    memset(it, 0, sizeof *it);
    it->line = line;
    it->parent = parent;
    it->depth = depth;
    if (*p == '@') {
        it->stub = 1;
        p++;
    }
    if (*p != '[' || !(close = strchr(p, ']'))) fail(line, "an item opens [type]Name { or [type]\"Text\" {");
    *close = 0;
    for (type = 0; type < T_TYPES && strcmp(type_words[type], p + 1); type++) continue;
    if (type == T_TYPES) fail(line, "[%s] is not a type: scene, list, button, label", p + 1);
    it->type = type;
    if (parent < 0 && type != T_SCENE) fail(line, "a %s outside a scene: the file holds scenes", type_words[type]);
    if (parent >= 0 && !(holds[g_uigen.items[parent].type] & (1u << type)))
        fail(line, "a %s cannot hold a %s", type_words[g_uigen.items[parent].type], type_words[type]);
    rest = close + 1;
    if (*rest == '"')
        it->text = quoted(&rest, line);
    else {
        char* e = rest;
        while (isalnum((unsigned char)*e) || *e == '_') e++;
        if (e == rest || !isalpha((unsigned char)*rest)) fail(line, "[%s] needs a Name or a \"Text\" after it", p + 1);
        it->name = dup_n(rest, (size_t)(e - rest));
        rest = e;
    }
    while (*rest == ' ' || *rest == '\t') rest++;
    if (!strcmp(rest, "{}"))
        *closed = 1;
    else if (!strcmp(rest, "{"))
        *closed = 0;
    else
        fail(line, "after the item, { or {}, not \"%s\"", rest);
    if ((type == T_SCENE || type == T_LIST) && !it->name) fail(line, "a %s has a Name, not a text", type_words[type]);
    if (type == T_BUTTON && !it->text)
        fail(line, "a button has its \"Text\": its symbol is the text's, and a label is what shows a changing one");
    if (type == T_LABEL && it->name && is_row(it))
        fail(line, "a row of a list shows its \"Text\": a list whose rows change has no items, its rows come from "
                   "its get_row_info");
    if (it->stub && (type == T_SCENE || (type == T_LABEL && it->text)))
        fail(line, "@ stubs an item's callbacks, and this %s has none", type_words[type]);
    {
        char* own = snake(it->name ? it->name : it->text, line);
        int other;
        if (parent >= 0 && g_uigen.items[parent].type == T_SCENE &&
            (!strcmp(own, "items") || !strcmp(own, "texts") || !strcmp(own, "key") || !strcmp(own, "layout") ||
             !strcmp(own, "shown") || !strcmp(own, "draw") || !strcmp(own, "hit") || !strcmp(own, "act")))
            fail(line, "%s_%s is the scene's own table or function: name it otherwise", g_uigen.items[parent].symbol,
                 own);
        it->symbol = parent < 0 ? cat3("ui", "_", own) : cat3(g_uigen.items[parent].symbol, "_", own);
        free(own);
        if ((other = item_with_symbol(it->symbol)) >= 0)
            fail(line,
                 "%s is the symbol of line %d's item too: their names or texts differ only in what is not a "
                 "letter or a digit",
                 it->symbol, g_uigen.items[other].line);
    }
    it->upper = upper(it->symbol);
    it->scene = parent < 0 ? g_uigen.nitems : g_uigen.items[parent].scene;
    if (parent >= 0) g_uigen.items[parent].children++;
    return g_uigen.nitems++;
}

static void read_ui(void) {
    char* text = read_spec();
    char *line, *next;
    int number = 0, depth = 0, stack[MAX_DEPTH];
    for (line = text; line && *line; line = next) {
        char *p, *e;
        int tabs = 0, quote = 0;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        number++;
        while (line[tabs] == '\t') tabs++;
        p = line + tabs;
        for (e = p; *e; e++) { /* a # outside a text starts a comment */
            if (*e == '"' && (e == p || e[-1] != '\\')) quote = !quote;
            if (*e == '#' && !quote) {
                *e = 0;
                break;
            }
        }
        e = p + strlen(p);
        while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
        if (!*p) continue;
        if (*p == ' ') fail(number, "indented with spaces: ui.txt indents with tabs");
        if (!strcmp(p, "}")) {
            if (!depth) fail(number, "} with no block open");
            if (tabs != depth - 1)
                fail(number, "} at %d tab(s): the block it closes opens at %d", tabs,
                     g_uigen.items[stack[depth - 1]].depth);
            depth--;
        } else if (*p == '@' || *p == '[') {
            int closed, at;
            if (tabs != depth) fail(number, "an item at %d tab(s): inside its parent, %d", tabs, depth);
            at = parse_opener(p, depth ? stack[depth - 1] : -1, depth, number, &closed);
            if (!closed) {
                if (depth == MAX_DEPTH) fail(number, "more than %d blocks inside one another", MAX_DEPTH);
                stack[depth++] = at;
            }
        } else {
            char* eq = strchr(p, '=');
            char *word, *value;
            if (!depth) fail(number, "%s outside any item", p);
            if (tabs != depth) fail(number, "a property at %d tab(s): its item's are at %d", tabs, depth);
            if (!eq) fail(number, "%s: a property is word = value", p);
            *eq = 0;
            word = p;
            value = eq + 1;
            for (e = word + strlen(word); e > word && (e[-1] == ' ' || e[-1] == '\t'); e--) e[-1] = 0;
            while (*value == ' ' || *value == '\t') value++;
            set_property(&g_uigen.items[stack[depth - 1]], word, value, number);
        }
    }
    if (depth)
        fail(g_uigen.items[stack[depth - 1]].line, "[%s]%s is never closed",
             type_words[g_uigen.items[stack[depth - 1]].type],
             g_uigen.items[stack[depth - 1]].name ? g_uigen.items[stack[depth - 1]].name
                                                  : g_uigen.items[stack[depth - 1]].text);
    free(text);
}

/* what the items must have together: a hotkey once in its scene and platform; a list's rows its items or a callback's;
   a row's width kept between bounds that leave room */
static void check_items(void) {
    int i, j;
    for (i = 0; i < g_uigen.nitems; i++) {
        const Item* a = &g_uigen.items[i];
        if (a->stub && a->type == T_LIST && a->children)
            fail(a->line, "@ stubs a list's rows, and this one has items of its own");
        if (a->type == T_LIST && !a->children && (a->set & BIT(P_ATTR)))
            fail(a->line, "attr = callback on a list without items: its get_row_info gives each row's attr");
        if (a->platform != PLATFORM_ANY && !is_row(a))
            fail(a->line, "for-platform is a row's: an item of a scene keeps its place on every platform");
        if ((a->set & BIT(P_MIN_ROW_WIDTH)) && (a->set & BIT(P_MAX_ROW_WIDTH)) &&
            a->unit[P_MIN_ROW_WIDTH] == a->unit[P_MAX_ROW_WIDTH] &&
            a->value[P_MIN_ROW_WIDTH] > a->value[P_MAX_ROW_WIDTH])
            fail(a->line, "min_row_width %d is over max_row_width %d", a->value[P_MIN_ROW_WIDTH],
                 a->value[P_MAX_ROW_WIDTH]);
        if (a->key || a->fkey)
            for (j = 0; j < i; j++) {
                const Item* b = &g_uigen.items[j];
                int both = a->platform == PLATFORM_ANY || b->platform == PLATFORM_ANY || a->platform == b->platform;
                if (b->scene == a->scene && both && b->key == a->key && b->fkey == a->fkey && b->mods == a->mods)
                    fail(a->key_line, "its key is line %d's too, in the same scene", b->key_line);
            }
    }
}

/* ============================================================ the layout: a flexbox of each scene's items */

static Lin lin(double a, double c) {
    Lin l;
    l.a = a;
    l.c = c;
    return l;
}
static Lin lin_add(Lin x, Lin y) { return lin(x.a + y.a, x.c + y.c); }
static Lin lin_sub(Lin x, Lin y) { return lin(x.a - y.a, x.c - y.c); }
static Lin lin_times(Lin x, double k) { return lin(x.a * k, x.c * k); }

/* an item's length on an axis on platform on: its own there, else the property set (px, or a % of the parent's
   inside), else none (0 in *has) */
static Lin length_of(const Item* it, int p, Lin inside, int* has, int on) {
    int value = it->value[p], unit = it->unit[p];
    *has = (it->set & BIT(p)) != 0;
    if (on != PLATFORM_ANY && (it->plat_set[on] & BIT(p))) {
        *has = 1;
        value = it->plat_value[on][p];
        unit = it->plat_unit[on][p];
    }
    if (!*has) return lin(0, 0);
    if (unit == UNIT_PX) return lin(0, value);
    return lin_times(inside, value / 100.0);
}
static void layout_scene(int scene, int on) {
    Item* s = &g_uigen.items[scene];
    int dir = s->value[P_DIRECTION], align = s->value[P_ALIGN], justify = s->value[P_JUSTIFY];
    double gap = s->value[P_GAP], pad = s->value[P_PADDING];
    int main_p = dir == COLUMN ? P_HEIGHT : P_WIDTH, cross_p = dir == COLUMN ? P_WIDTH : P_HEIGHT, i, n = 0;
    double grows = 0;
    Lin inside = lin(1, -2 * pad), fixed = lin(0, 0), free_len, at, size[MAX_ITEMS];
    int grow[MAX_ITEMS];
    for (i = scene + 1; i < g_uigen.nitems && g_uigen.items[i].scene == scene; i++) {
        Item* it = &g_uigen.items[i];
        int has;
        if (it->parent != scene) continue;
        size[i] = length_of(it, main_p, inside, &has, on);
        grow[i] = (it->set & BIT(P_GROW)) ? it->value[P_GROW] : !has && it->type == T_LIST;
        if (!has && !grow[i]) size[i] = lin(0, ROW_HEIGHT); /* a button's or a label's row */
        if (grow[i] && has)
            fail(it->line, "%s %s and grow %d: one of them, a length or a share of what is left",
                 property_words[main_p], it->unit[main_p] == UNIT_PX ? "in px" : "in %", grow[i]);
        if (!grow[i]) fixed = lin_add(fixed, size[i]);
        grows += grow[i];
        n++;
    }
    free_len = lin_sub(lin_sub(inside, fixed), lin(0, gap * (n > 1 ? n - 1 : 0)));
    at = lin(0, pad);
    if (!grows && justify == J_CENTER) at = lin_add(at, lin_times(free_len, 0.5));
    if (!grows && justify == J_END) at = lin_add(at, free_len);
    for (i = scene + 1; i < g_uigen.nitems && g_uigen.items[i].scene == scene; i++) {
        Item* it = &g_uigen.items[i];
        Lin cross, offset;
        int has;
        if (it->parent != scene) continue;
        if (grow[i]) size[i] = lin_times(free_len, grow[i] / grows);
        cross = length_of(it, cross_p, inside, &has, on);
        if (!has) cross = inside; /* stretched, and the whole inside whatever the align when it has no length */
        offset = lin(0, pad);
        if (align == A_CENTER) offset = lin_add(offset, lin_times(lin_sub(inside, cross), 0.5));
        if (align == A_END) offset = lin_add(offset, lin_sub(inside, cross));
        if (dir == COLUMN) {
            it->y[on] = at;
            it->h[on] = size[i];
            it->x[on] = offset;
            it->w[on] = cross;
        } else {
            it->x[on] = at;
            it->w[on] = size[i];
            it->y[on] = offset;
            it->h[on] = cross;
        }
        at = lin_add(lin_add(at, size[i]), lin(0, gap));
    }
}

/* ============================================================ the generated files */

static void head(void) {
    outs(
        "/* Generated by tools/uigen from src/gui/ui.txt (tools/uigen/uigen.c's head holds the format): do not edit */\n");
}
/* an item's callbacks: a button's action, a label's text when it has a name, a list's rows when it has no item */
static int has_action(const Item* it) { return it->type == T_BUTTON; }
static int has_update(const Item* it) { return it->type == T_LABEL && it->name; }
static int has_rows(const Item* it) { return it->type == T_LIST && !it->children; }
static int has_attr(const Item* it) { return (it->set & BIT(P_ATTR)) != 0; }

static void out_prototypes(const Item* it, int stub) {
    const char* end = stub ? " {" : ";\n";
    if (has_action(it)) out("void %s_action_cb(App* app, uint32_t hit)%s", it->symbol, end);
    if (stub && has_action(it)) outs("\n    (void)app;\n    (void)hit;\n}\n");
    if (has_update(it)) out("void %s_update_cb(App* app, char* text, size_t size)%s", it->symbol, end);
    if (stub && has_update(it)) outs("\n    (void)app;\n    if (size) text[0] = 0;\n}\n");
    if (has_attr(it))
        out("unsigned %s_attr_cb(App* app%s)%s", it->symbol, it->type == T_LIST ? ", size_t row" : "", end);
    if (stub && has_attr(it)) outs("\n    (void)app;\n    return 0;\n}\n");
    if (has_rows(it)) {
        out("bool %s_get_row_info(App* app, size_t row, UiRowInfo* info)%s", it->symbol, end);
        if (stub) outs("\n    (void)app;\n    (void)row;\n    (void)info;\n    return false;\n}\n");
        out("void %s_row_action_cb(App* app, size_t row, uint32_t hit)%s", it->symbol, end);
        if (stub) outs("\n    (void)app;\n    (void)row;\n    (void)hit;\n}\n");
    }
}

static void write_h(const char* path) {
    int i, j;
    g_uigen.out_path = path;
    head();
    outs(
        "#ifndef SNX_GUI_UI_H\n#define SNX_GUI_UI_H\n#include <stdbool.h>\n#include <stddef.h>\n#include <stdint.h>\n");
    outs("#include <SDL3/SDL_platform_defines.h>\n#include \"gui/place.h\" /* GuiRect */\n");
    outs("typedef struct App App; /* main.c's */\n");
    outs("typedef enum { UI_SCENE, UI_LIST, UI_BUTTON, UI_LABEL } UiType;\n");
    outs("/* a colour of the theme's, by its theme.ini name; none: the one its type draws in */\ntypedef enum {\n"
         "    UI_COLOR_NONE");
    for (i = 0; i < COLORS; i++) {
        char* u = upper(colors[i]);
        out(",\n    UI_COLOR_%s", u);
        free(u);
    }
    outs("\n} UiColor;\n");
    outs("#define UI_NO_TEXT 0xffff /* a named label's update_cb gives it */\n");
    outs("/* an item of a scene: its parent's index (-1: the scene's own), its text's offset in the scene's texts */\n"
         "typedef struct {\n    uint8_t type, color; /* UiType, UiColor */\n    int16_t parent;\n    uint16_t text;\n"
         "} UiItem;\n");
    outs("/* a list's state, the caller's: the row it shows first and the row in focus */\ntypedef struct {\n"
         "    size_t first, focus;\n} UiList;\n");
    outs(
        "/* a row a list without items asks get_row_info for: its text (NULL: \"(null)\", as printf's %s prints it) and\n"
        "   how it is drawn */\ntypedef struct {\n    const char* label;\n    unsigned attr;\n} UiRowInfo;\n");
    outs(
        "/* how a row is drawn, a bit each: in the failure colour, every other blink, an arrow up (rows above it are\n"
        "   hidden) or down (rows below) at its right, in the active colour (the one activated last), a label between\n"
        "   rows (no box) */\nenum { UI_ATTR_FAILURE = 1, UI_ATTR_BLINK = 2, UI_ATTR_MORE_UP = 4, UI_ATTR_MORE_DOWN = 8, "
        "UI_ATTR_ACTIVE = 16, UI_ATTR_LABEL = 32 };\n");
    outs("enum { UI_MOD_CTRL = 1, UI_MOD_ALT = 2 };\n");
    outs("/* what the hit test says of a point, a bit each: on an item, pressed on it, released on it */\nenum { "
         "UI_HIT_HOVER = 1, UI_HIT_CLICK = 2, UI_HIT_RELEASE = 4 };\n");
    outs(
        "/* the drawing, main.c's: a row (a list's, or a button) and a label, in the theme's look, their text as given; a\n"
        "   text of ui.txt as shown (translated) */\n"
        "void ui_draw_row(App* app, GuiRect r, const char* text, unsigned color, unsigned attr, bool focused);\n"
        "void ui_draw_label(App* app, GuiRect r, const char* text, unsigned color);\n"
        "const char* ui_tr(const char* english);\n");
    for (i = 0; i < g_uigen.nitems; i++) {
        const Item* s = &g_uigen.items[i];
        if (s->type != T_SCENE) continue;
        out("/* scene %s (ui.txt:%d): its items in ui.txt's order */\nenum {\n", s->name, s->line);
        for (j = i + 1; j < g_uigen.nitems && g_uigen.items[j].scene == i; j++) {
            out_platform(&g_uigen.items[j]);
            out("    %s,\n", g_uigen.items[j].upper);
            out_platform_end(&g_uigen.items[j]);
        }
        out("    %s_ITEMS\n};\n", s->upper);
        out("extern const char %s_texts[];\nextern const UiItem %s_items[%s_ITEMS];\n", s->symbol, s->symbol, s->upper);
        out("/* its items' rectangles in the area at, at a UI scale of scale (a row of a list gets none: its list's\n"
            "   rows are drawn and hit in the list's) */\nvoid %s_layout(GuiRect at, int scale, GuiRect* rects);\n",
            s->symbol);
        out("/* how many rows its list item shows: those that fit, max_rows at most */\nsize_t %s_shown(const GuiRect* "
            "rects, int scale, int item);\n",
            s->symbol);
        out("/* the rectangle of the row its list item shows i-th (from its first), as drawn and hit */\nGuiRect "
            "%s_row(const GuiRect* rects, int scale, int item, size_t i);\n",
            s->symbol);
        out("/* every item drawn, each list from its state (lists: one per item, a list's at its index) */\nvoid "
            "%s_draw(App* app, const GuiRect* rects, int scale, const UiList* lists);\n",
            s->symbol);
        out("/* the item at x, y and its row (a list's), the bits of what event (UI_HIT_CLICK, UI_HIT_RELEASE or 0) "
            "makes\n   of it; 0 and *item -1 off every item */\nuint32_t %s_hit(App* app, const GuiRect* rects, int "
            "scale, const UiList* lists, int x, int y, uint32_t event,\n    int* item, size_t* row);\n",
            s->symbol);
        out("/* list item's first row set so its focus shows, as its scroll says, of count rows */\nvoid "
            "%s_follow(const GuiRect* rects, int scale, int item, size_t count, UiList* list);\n",
            s->symbol);
        out("/* what a hit on an item does: its button's action_cb, its list's row_action_cb */\nvoid %s_act(App* app, "
            "int item, size_t row, uint32_t hit);\n",
            s->symbol);
        out("/* a hotkey of the scene pressed (key: an SDL_Keycode; mods: UI_MOD_*): its button's action_cb called, "
            "true; false when it is none */\nbool %s_key(App* app, int key, unsigned mods);\n",
            s->symbol);
    }
    outs("/* the callbacks: main.c's, those of the items marked @ ui.c's */\n");
    for (i = 0; i < g_uigen.nitems; i++) {
        const Item* it = &g_uigen.items[i];
        if (!has_action(it) && !has_update(it) && !has_rows(it) && !has_attr(it)) continue;
        out_platform(it);
        out_prototypes(it, 0);
        out_platform_end(it);
    }
    outs("#endif\n");
    flush(path);
}

/* a, a fraction, and c, pixels, in 16.16 fixed point, as UI_AT takes them */
static long fixed(double v) {
    double f = v * 65536.0;
    return (long)(f < 0 ? f - 0.5 : f + 0.5);
}
static void out_at(const char* origin, const char* length, Lin l) {
    out("%s%sUI_AT(%ldL, %s, %ldL, s)", origin, *origin ? " + " : "", fixed(l.a), length, fixed(l.c));
}
static void out_key(const Item* it) {
    if (it->fkey)
        out("SDLK_F%d", it->fkey);
    else if (it->key == '\'' || it->key == '\\')
        out("'\\%c'", it->key);
    else
        out("'%c'", it->key);
}
/* a row bound's pixels: a list's min_row_width or max_row_width, 0 for none */
static void out_bound(const Item* list, int p, const char* rect) {
    if (!(list->set & BIT(p)))
        outs("0");
    else if (list->unit[p] == UNIT_PX)
        out("%d * s", list->value[p]);
    else
        out("%s.w * %d / 100", rect, list->value[p]);
}
/* a list's row metric (row_height, row_gap, max_rows) on a platform: its own there, else the list's, else the default */
static int metric(const Item* list, int on, int p) {
    if (on != PLATFORM_ANY && (list->plat_set[on] & BIT(p))) return list->plat_value[on][p];
    if (list->set & BIT(p)) return list->value[p];
    return p == P_ROW_HEIGHT ? ROW_HEIGHT : p == P_ROW_GAP ? ROW_GAP : MAX_ROWS;
}
static int metric_varies(const Item* list, int p) {
    return ((list->plat_set[PLATFORM_ANDROID] | list->plat_set[PLATFORM_DESKTOP]) & BIT(p)) != 0;
}
static const char* metric_name(int p) {
    return p == P_ROW_HEIGHT ? "ROW_HEIGHT" : p == P_ROW_GAP ? "ROW_GAP" : "MAX_ROWS";
}
/* a metric as the code uses it: its number, or the macro out_metrics defines where a platform sets its own */
static void out_metric(const Item* list, int p) {
    if (metric_varies(list, p))
        out("%s__%s", list->upper, metric_name(p));
    else
        out("%d", metric(list, PLATFORM_ANY, p));
}
/* the macros of a scene's lists' metrics that a platform sets: android's, desktop's (out_platform's), else iOS's */
static void out_metrics(int scene) {
    static const int metrics[] = {P_ROW_HEIGHT, P_ROW_GAP, P_MAX_ROWS};
    int i, k;
    for (i = scene + 1; i < g_uigen.nitems && g_uigen.items[i].scene == scene; i++) {
        const Item* l = &g_uigen.items[i];
        for (k = 0; k < 3; k++) {
            int p = metrics[k];
            if (!metric_varies(l, p)) continue;
            out("#ifdef SDL_PLATFORM_ANDROID\n#define %s__%s %d\n", l->upper, metric_name(p),
                metric(l, PLATFORM_ANDROID, p));
            out("#elif !defined(SDL_PLATFORM_IOS)\n#define %s__%s %d\n", l->upper, metric_name(p),
                metric(l, PLATFORM_DESKTOP, p));
            out("#else\n#define %s__%s %d\n#endif\n", l->upper, metric_name(p), metric(l, PLATFORM_ANY, p));
        }
    }
}
/* whether a scene's layout differs on a platform, one setting its own lengths */
static int layout_varies(int scene) {
    int i, on;
    for (i = scene + 1; i < g_uigen.nitems && g_uigen.items[i].scene == scene; i++) {
        const Item* it = &g_uigen.items[i];
        for (on = PLATFORM_ANDROID; on <= PLATFORM_DESKTOP; on++)
            if (fixed(it->x[on].a) != fixed(it->x[0].a) || fixed(it->x[on].c) != fixed(it->x[0].c) ||
                fixed(it->y[on].a) != fixed(it->y[0].a) || fixed(it->y[on].c) != fixed(it->y[0].c) ||
                fixed(it->w[on].a) != fixed(it->w[0].a) || fixed(it->w[on].c) != fixed(it->w[0].c) ||
                fixed(it->h[on].a) != fixed(it->h[0].a) || fixed(it->h[on].c) != fixed(it->h[0].c))
                return 1;
    }
    return 0;
}
/* a scene's rectangles on platform on */
static void out_layout(int scene, int on) {
    int i;
    for (i = scene + 1; i < g_uigen.nitems && g_uigen.items[i].scene == scene; i++) {
        const Item* it = &g_uigen.items[i];
        if (it->parent != scene) {
            out_platform(it);
            out("    r[%s] = gui_rect(0, 0, 0, 0);\n", it->upper);
            out_platform_end(it);
            continue;
        }
        out_from(it->line);
        out("    r[%s] = gui_rect(", it->upper);
        out_at("at.x", "at.w", it->x[on]);
        outs(", ");
        out_at("at.y", "at.h", it->y[on]);
        outs(", ");
        out_at("", "at.w", it->w[on]);
        outs(", ");
        out_at("", "at.h", it->h[on]);
        outs(");\n");
    }
}
/* row i of list item l (its rows' metrics as ui.txt sets them), in its rectangle r[l] */
static void out_row(const Item* list, int l, const char* i) {
    char rect[64];
    snprintf(rect, sizeof rect, "r[%s]", g_uigen.items[l].upper);
    out("ui_row(%s, %s, ", rect, i);
    out_metric(list, P_ROW_HEIGHT);
    outs(", ");
    out_metric(list, P_ROW_GAP);
    outs(", ");
    out_bound(list, P_MIN_ROW_WIDTH, rect);
    outs(", ");
    out_bound(list, P_MAX_ROW_WIDTH, rect);
    outs(", s)");
}
static void write_scene(int scene) {
    const Item* s = &g_uigen.items[scene];
    int i, j, offset = 0, keys = 0, attrs = 0;
    /* the texts, one array; the items, a row each; each static list's rows, as item indices */
    out("const char %s_texts[] =", s->symbol);
    for (i = scene + 1; i < g_uigen.nitems && g_uigen.items[i].scene == scene; i++)
        if (g_uigen.items[i].text) {
            g_uigen.items[i].offset = offset;
            outs("\n    ");
            out_string(g_uigen.items[i].text);
            outs(" \"\\0\"");
            offset += (int)strlen(g_uigen.items[i].text) + 1;
        }
    if (!offset) outs(" \"\"");
    outs(";\n");
    if (offset > 0xfffe)
        fail(s->line, "scene %s's texts take %d bytes, more than an offset of 65534 reaches", s->name, offset);
    out("const UiItem %s_items[%s_ITEMS] = {\n", s->symbol, s->upper);
    for (i = scene + 1; i < g_uigen.nitems && g_uigen.items[i].scene == scene; i++) {
        const Item* it = &g_uigen.items[i];
        char* color = upper(it->color ? colors[it->color - 1] : "none");
        out_platform(it);
        out_from(it->line);
        out("    {%s, UI_COLOR_%s, %s, ", type_enums[it->type], color,
            it->parent == scene ? "-1" : g_uigen.items[it->parent].upper);
        if (it->text)
            out("%d},\n", it->offset);
        else
            outs("UI_NO_TEXT},\n");
        out_platform_end(it);
        free(color);
    }
    out_here();
    outs("};\n");
    for (i = scene + 1; i < g_uigen.nitems && g_uigen.items[i].scene == scene; i++) {
        const Item* l = &g_uigen.items[i];
        if (l->type != T_LIST || !l->children) continue;
        out("/* %s's rows */\nstatic const int16_t %s_rows[] = {\n", l->name, l->symbol);
        for (j = i + 1; j < g_uigen.nitems && g_uigen.items[j].scene == scene; j++)
            if (g_uigen.items[j].parent == i) {
                out_platform(&g_uigen.items[j]);
                out("    %s,\n", g_uigen.items[j].upper);
                out_platform_end(&g_uigen.items[j]);
            }
        out_here();
        outs("};\n");
    }
    out_metrics(scene);
    /* the layout: each item of the scene at its fractions of the area and its pixels times the scale; a platform's
       own where one sets its own lengths */
    out("void %s_layout(GuiRect at, int scale, GuiRect* r) {\n    int s = scale;\n", s->symbol);
    if (!layout_varies(scene))
        out_layout(scene, PLATFORM_ANY);
    else {
        outs("#ifdef SDL_PLATFORM_ANDROID\n");
        out_layout(scene, PLATFORM_ANDROID);
        outs("#elif !defined(SDL_PLATFORM_IOS)\n");
        out_layout(scene, PLATFORM_DESKTOP);
        outs("#else\n");
        out_layout(scene, PLATFORM_ANY);
        outs("#endif\n");
    }
    out_here();
    outs("}\n");
    /* the rows each list shows */
    out("size_t %s_shown(const GuiRect* r, int scale, int item) {\n    (void)r; /* a scene without lists */\n"
        "    (void)scale;\n    switch (item) {\n",
        s->symbol);
    for (i = scene + 1; i < g_uigen.nitems && g_uigen.items[i].scene == scene; i++) {
        const Item* l = &g_uigen.items[i];
        if (l->type != T_LIST) continue;
        out_from(l->line);
        out("        case %s: return ui_shown(r[%s].h, ", l->upper, l->upper);
        out_metric(l, P_ROW_HEIGHT);
        outs(", ");
        out_metric(l, P_ROW_GAP);
        outs(", ");
        out_metric(l, P_MAX_ROWS);
        outs(", scale);\n");
    }
    out_here();
    outs("        default: return 0;\n    }\n}\n");
    /* a list's row i, as drawn */
    out("GuiRect %s_row(const GuiRect* r, int scale, int item, size_t i) {\n    int s = scale;\n    (void)r; /* a scene "
        "without lists */\n    (void)s;\n    (void)i;\n    switch (item) {\n",
        s->symbol);
    for (i = scene + 1; i < g_uigen.nitems && g_uigen.items[i].scene == scene; i++) {
        const Item* l = &g_uigen.items[i];
        if (l->type != T_LIST) continue;
        out_from(l->line);
        out("        case %s: return ", l->upper);
        out_row(l, i, "i");
        outs(";\n");
    }
    out_here();
    outs("        default: return gui_rect(0, 0, 0, 0);\n    }\n}\n");
    /* each list's first row, from its focus */
    out("void %s_follow(const GuiRect* r, int scale, int item, size_t count, UiList* list) {\n"
        "    (void)r; /* a scene without lists */\n    (void)scale;\n    (void)count;\n    (void)list;\n"
        "    switch (item) {\n",
        s->symbol);
    for (i = scene + 1; i < g_uigen.nitems && g_uigen.items[i].scene == scene; i++) {
        const Item* l = &g_uigen.items[i];
        if (l->type != T_LIST) continue;
        out_from(l->line);
        out("        case %s: ui_follow(list, count, %s_shown(r, scale, item), %d); break;\n", l->upper, s->symbol,
            (l->set & BIT(P_SCROLL)) ? l->value[P_SCROLL] == SCROLL_CENTER : 1);
    }
    out_here();
    outs("        default: break;\n    }\n}\n");
    /* the attributes the code gives the rows of its static lists, by item */
    for (i = scene + 1; i < g_uigen.nitems && g_uigen.items[i].scene == scene; i++)
        attrs += has_attr(&g_uigen.items[i]) && g_uigen.items[i].parent != scene;
    if (attrs) {
        out("static unsigned %s_attr(App* app, int item) {\n    (void)app;\n    switch (item) {\n", s->symbol);
        for (i = scene + 1; i < g_uigen.nitems && g_uigen.items[i].scene == scene; i++) {
            const Item* it = &g_uigen.items[i];
            if (!has_attr(it) || it->parent == scene) continue;
            out_platform(it);
            out_from(it->line);
            out("        case %s: return %s_attr_cb(app);\n", it->upper, it->symbol);
            out_platform_end(it);
        }
        out_here();
        outs("        default: return 0;\n    }\n}\n");
    }
    /* the drawing: each item, in ui.txt's order; the first row shown says the rows hidden above it, the last those
       below */
    out("void %s_draw(App* app, const GuiRect* r, int scale, const UiList* lists) {\n    int s = scale;\n"
        "    char text[512];\n    UiRowInfo info;\n    size_t i, n;\n    (void)text;\n    (void)info;\n    (void)i;\n"
        "    (void)n;\n    (void)s;\n    (void)lists;\n    (void)app;\n    (void)r;\n",
        s->symbol);
    for (i = scene + 1; i < g_uigen.nitems && g_uigen.items[i].scene == scene; i++) {
        const Item* it = &g_uigen.items[i];
        if (it->parent != scene) continue;
        out_from(it->line);
        if (it->type == T_LIST && it->children) {
            out("    n = %s_shown(r, scale, %s);\n", s->symbol, it->upper);
            out("    for (i = 0; i < n && lists[%s].first + i < sizeof %s_rows / sizeof *%s_rows; i++) {\n", it->upper,
                it->symbol, it->symbol);
            out("        size_t row = lists[%s].first + i;\n        int item = %s_rows[row];\n", it->upper, it->symbol);
            outs("        unsigned attr = ");
            if (attrs) out("%s_attr(app, item)%s", s->symbol, has_attr(it) ? " | " : "");
            if (has_attr(it)) out("%s_attr_cb(app, row)", it->symbol);
            outs(attrs || has_attr(it) ? ";\n" : "0;\n");
            outs("        if (i == 0 && row > 0) attr |= UI_ATTR_MORE_UP;\n");
            out("        if (i + 1 == n && row + 1 < sizeof %s_rows / sizeof *%s_rows) attr |= UI_ATTR_MORE_DOWN;\n",
                it->symbol, it->symbol);
            out("        ui_draw_row(app, ");
            out_row(it, i, "i");
            out(", ui_tr(%s_texts + %s_items[item].text), %s_items[item].color ? %s_items[item].color : "
                "%s_items[%s].color, attr,\n                    row == lists[%s].focus);\n    }\n",
                s->symbol, s->symbol, s->symbol, s->symbol, s->symbol, it->upper, it->upper);
        } else if (it->type == T_LIST) {
            out("    n = %s_shown(r, scale, %s);\n", s->symbol, it->upper);
            out("    for (i = 0; i < n; i++) {\n        size_t row = lists[%s].first + i;\n        UiRowInfo next;\n",
                it->upper);
            /* the row after it asked first: the row's own label may be a buffer of the code's the next call writes */
            out("        bool more = i + 1 == n && %s_get_row_info(app, row + 1, &next);\n", it->symbol);
            out("        info.label = NULL;\n        info.attr = 0;\n        if (!%s_get_row_info(app, row, &info)) "
                "break;\n",
                it->symbol);
            outs("        if (i == 0 && row > 0) info.attr |= UI_ATTR_MORE_UP;\n");
            outs("        if (more) info.attr |= UI_ATTR_MORE_DOWN;\n");
            outs("        ui_draw_row(app, ");
            out_row(it, i, "i");
            out(", info.label ? info.label : \"(null)\", %s_items[%s].color, info.attr,\n                    row == "
                "lists[%s].focus);\n    }\n",
                s->symbol, it->upper, it->upper);
        } else if (it->type == T_BUTTON) {
            out("    ui_draw_row(app, r[%s], ui_tr(%s_texts + %d), %s_items[%s].color, ", it->upper, s->symbol,
                it->offset, s->symbol, it->upper);
            if (has_attr(it))
                out("%s_attr_cb(app), false);\n", it->symbol);
            else
                outs("0, false);\n");
        } else if (it->text)
            out("    ui_draw_label(app, r[%s], ui_tr(%s_texts + %d), %s_items[%s].color);\n", it->upper, s->symbol,
                it->offset, s->symbol, it->upper);
        else
            out("    %s_update_cb(app, text, sizeof text);\n    ui_draw_label(app, r[%s], text, %s_items[%s].color);\n",
                it->symbol, it->upper, s->symbol, it->upper);
    }
    out_here();
    outs("}\n");
    /* the hit test: the rows of each list, then each button and label, as they are drawn */
    out("uint32_t %s_hit(App* app, const GuiRect* r, int scale, const UiList* lists, int x, int y, uint32_t event, "
        "int* item,\n    size_t* row) {\n    int s = scale;\n    UiRowInfo info;\n    size_t i, n;\n    (void)app;\n"
        "    (void)info;\n    (void)i;\n    (void)n;\n    (void)s;\n    (void)lists;\n",
        s->symbol);
    for (i = scene + 1; i < g_uigen.nitems && g_uigen.items[i].scene == scene; i++) {
        const Item* it = &g_uigen.items[i];
        if (it->parent != scene) continue;
        out_from(it->line);
        if (it->type == T_LIST) {
            out("    n = %s_shown(r, scale, %s);\n    for (i = 0; i < n; i++) {\n        *row = lists[%s].first + i;\n",
                s->symbol, it->upper, it->upper);
            if (it->children)
                out("        if (*row >= sizeof %s_rows / sizeof *%s_rows) break;\n", it->symbol, it->symbol);
            else
                out("        if (!%s_get_row_info(app, *row, &info)) break;\n", it->symbol);
            outs("        if (ui_in(");
            out_row(it, i, "i");
            if (it->children) /* the row's own item, which a hit acts on */
                out(", x, y)) {\n            *item = %s_rows[*row];\n", it->symbol);
            else
                out(", x, y)) {\n            *item = %s;\n", it->upper);
            outs("            return UI_HIT_HOVER | event;\n        }\n    }\n");
        } else
            out("    if (ui_in(r[%s], x, y)) {\n        *item = %s;\n        *row = 0;\n        return UI_HIT_HOVER | "
                "event;\n    }\n",
                it->upper, it->upper);
    }
    out_here();
    outs("    *item = -1;\n    *row = 0;\n    return 0;\n}\n");
    /* what a hit does */
    out("void %s_act(App* app, int item, size_t row, uint32_t hit) {\n    (void)app;\n    (void)row;\n    (void)hit;\n"
        "    switch (item) {\n",
        s->symbol);
    for (i = scene + 1; i < g_uigen.nitems && g_uigen.items[i].scene == scene; i++) {
        const Item* it = &g_uigen.items[i];
        if (!has_action(it) && !has_rows(it)) continue;
        out_platform(it);
        out_from(it->line);
        if (has_action(it))
            out("        case %s: %s_action_cb(app, hit); break;\n", it->upper, it->symbol);
        else
            out("        case %s: %s_row_action_cb(app, row, hit); break;\n", it->upper, it->symbol);
        out_platform_end(it);
    }
    out_here();
    outs("        default: break;\n    }\n}\n");
    /* the hotkeys: one case a key, its modifiers within it */
    out("bool %s_key(App* app, int key, unsigned mods) {\n", s->symbol);
    for (i = scene + 1; i < g_uigen.nitems && g_uigen.items[i].scene == scene; i++)
        keys += g_uigen.items[i].key || g_uigen.items[i].fkey;
    outs("    (void)app; /* a hotkey of another platform alone leaves them unused */\n    (void)mods;\n");
    if (!keys)
        outs("    (void)key;\n    return false;\n}\n");
    else {
        outs("    switch (key) {\n");
        for (i = scene + 1; i < g_uigen.nitems && g_uigen.items[i].scene == scene; i++) {
            const Item* it = &g_uigen.items[i];
            int first = 1;
            if (!it->key && !it->fkey) continue;
            for (j = scene + 1; j < i && first; j++)
                first = !(g_uigen.items[j].key == it->key && g_uigen.items[j].fkey == it->fkey &&
                          (g_uigen.items[j].key || g_uigen.items[j].fkey));
            if (!first) continue;
            outs("        case ");
            out_key(it);
            outs(":\n");
            for (j = i; j < g_uigen.nitems && g_uigen.items[j].scene == scene; j++) {
                const Item* k = &g_uigen.items[j];
                if (k->key != it->key || k->fkey != it->fkey) continue;
                out_platform(k);
                out_from(k->key_line);
                out("            if (mods == %s) {\n", k->mods == (MOD_CTRL | MOD_ALT) ? "(UI_MOD_CTRL | UI_MOD_ALT)"
                                                       : k->mods == MOD_CTRL           ? "UI_MOD_CTRL"
                                                       : k->mods == MOD_ALT            ? "UI_MOD_ALT"
                                                                                       : "0");
                out("                %s_action_cb(app, UI_HIT_CLICK);\n                return true;\n            }\n",
                    k->symbol);
                out_platform_end(k);
            }
            out_here();
            outs("            break;\n");
        }
        outs("        default: break;\n    }\n    return false;\n}\n");
    }
}

static void write_c(const char* path) {
    int i;
    g_uigen.out_path = path;
    head();
    outs("#include \"gui/ui.h\"\n#include <SDL3/SDL_keycode.h>\n");
    outs("/* a: a fraction of length, c: pixels at 1x, both in 16.16 fixed point; s the UI scale */\n"
         "#define UI_AT(a, length, c, s) ((int)(((int64_t)(a) * (length) + (int64_t)(c) * (s)) / 65536))\n");
    outs("/* whether x, y is in r */\nstatic bool ui_in(GuiRect r, int x, int y) { return x >= r.x && x < r.x + r.w && "
         "y >= r.y && y < r.y + r.h; }\n");
    outs("/* row i of a list at r: height and gap in pixels at 1x, its width kept between min and max (0: none) */\n"
         "static GuiRect ui_row(GuiRect r, size_t i, int height, int gap, int min, int max, int s) {\n"
         "    int w = r.w;\n    if (max && w > max) w = max;\n    if (min && w < min) w = min;\n"
         "    return gui_rect(r.x, r.y + (int)i * (height + gap) * s, w, height * s);\n}\n");
    outs(
        "/* first set so the focus is among the shown rows of count: in their middle (center) while rows are left to "
        "move\n   past, else as little moved as it takes */\nstatic void ui_follow(UiList* l, size_t count, size_t shown, "
        "int center) {\n    size_t last = count > shown ? count - shown : 0;\n    if (!shown) return;\n"
        "    if (center)\n        l->first = l->focus > (shown - 1) / 2 ? l->focus - (shown - 1) / 2 : 0;\n"
        "    else if (l->focus < l->first)\n        l->first = l->focus;\n    else if (l->focus >= l->first + shown)\n"
        "        l->first = l->focus - shown + 1;\n    if (l->first > last) l->first = last;\n}\n");
    outs("/* the rows that fit in a list height pixels high, max_rows at most */\n"
         "static size_t ui_shown(int height, int row, int gap, int max_rows, int s) {\n"
         "    int fit = height > 0 ? (height + gap * s) / ((row + gap) * s) : 0;\n"
         "    return (size_t)(fit < max_rows ? fit : max_rows);\n}\n");
    for (i = 0; i < g_uigen.nitems; i++)
        if (g_uigen.items[i].type == T_SCENE) write_scene(i);
    outs("/* the callbacks of the items marked @: decorative, they do nothing */\n");
    for (i = 0; i < g_uigen.nitems; i++) {
        const Item* it = &g_uigen.items[i];
        if (!it->stub) continue;
        out_platform(it);
        out_from(it->line);
        out_prototypes(it, 1);
        out_here();
        out_platform_end(it);
    }
    flush(path);
}

int main(int argc, char** argv) {
    const char* dir = NULL;
    char *h, *c;
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc)
            dir = argv[++i];
        else if (argv[i][0] == '-' && argv[i][1] == '-')
            fail(0, "%s is not an option: --out DIR, then ui.txt", argv[i]);
        else if (g_uigen.spec_path)
            fail(0, "%s after %s: one ui.txt holds every scene", argv[i], g_uigen.spec_path);
        else
            g_uigen.spec_path = argv[i];
    }
    if (!dir || !g_uigen.spec_path) fail(0, "usage: uigen --out DIR FILE");
    read_ui();
    check_items();
    for (i = 0; i < g_uigen.nitems && g_uigen.items[i].type != T_SCENE; i++) continue;
    if (i == g_uigen.nitems) fail(0, "no scene in %s", g_uigen.spec_path);
    for (i = 0; i < g_uigen.nitems; i++)
        if (g_uigen.items[i].type == T_SCENE) {
            layout_scene(i, PLATFORM_ANY);
            layout_scene(i, PLATFORM_ANDROID);
            layout_scene(i, PLATFORM_DESKTOP);
        }
    h = cat3(dir, "/", "gui/ui.h");
    c = cat3(dir, "/", "gui/ui.c");
    write_h(h);
    write_c(c);
    free(h);
    free(c);
    return 0;
}
