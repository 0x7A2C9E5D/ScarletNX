/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "pad_screen.h"
#include <stdio.h>
#include <string.h>

/* the controls' boxes, as NieR sets them: the left side's on the left, top to bottom, then the right side's */
static const int ORDER[SNX_PAD_CONTROLS] = {
    SNX_PAD_LT, SNX_PAD_LB, SNX_PAD_UP,    SNX_PAD_DOWN, SNX_PAD_LEFT, SNX_PAD_RIGHT, SNX_PAD_LS, SNX_PAD_BACK,
    SNX_PAD_RT, SNX_PAD_RB, SNX_PAD_NORTH, SNX_PAD_WEST, SNX_PAD_EAST, SNX_PAD_SOUTH, SNX_PAD_RS, SNX_PAD_START};
#define SIDE (SNX_PAD_CONTROLS / 2)

void snx_pad_screen_start(SnxPadScreen* s, int presets, const char* const* names, const int* modes, int in_use) {
    int i;
    memset(s, 0, sizeof *s);
    s->presets = presets < SNX_PAD_PRESETS ? presets : SNX_PAD_PRESETS;
    for (i = 0; i < s->presets; ++i) {
        s->name[i] = names[i];
        s->mode[i] = modes[i];
    }
    s->in_use = in_use;
    s->focus = in_use >= 0 && in_use < s->presets ? in_use : 0;
    snx_pad_map_unset(&s->own);
}
/* the pad's labels, NULL for none known: an Xbox pad's */
static const int* labels_of(const SnxPadScreen* s) {
    int c;
    for (c = 0; c < 4; ++c)
        if (s->labels[c] != SNX_PAD_LABEL_NONE) return s->labels;
    return NULL;
}
void snx_pad_screen_map(const SnxPadScreen* s, SnxPadMap* map) {
    int mode = s->in_use >= 0 && s->in_use < s->presets ? s->mode[s->in_use] : SNX_PAD_BY_LABEL;
    SnxPadMap labeled;
    snx_pad_map_labels(&labeled, labels_of(s));
    if (mode == SNX_PAD_BY_POSITION)
        snx_pad_map_position(map);
    else if (mode == SNX_PAD_CUSTOM) {
        *map = s->own;
        snx_pad_map_fill(map, &labeled);
    } else /* by label, and each game's .ini's own is shown as its default */
        *map = labeled;
}
/* map is the custom one from now on, and Custom the preset in use */
static int own(SnxPadScreen* s, const SnxPadMap* map) {
    int i;
    for (i = 0; i < s->presets && s->mode[i] != SNX_PAD_CUSTOM; ++i) {}
    if (i == s->presets) return SNX_PAD_DID_NOTHING;
    s->own = *map;
    s->in_use = i;
    return SNX_PAD_DID_MAP;
}
static int at_of(int control) {
    int i;
    for (i = 0; i < SNX_PAD_CONTROLS && ORDER[i] != control; ++i) {}
    return i;
}
int snx_pad_screen_key(SnxPadScreen* s, int key) {
    int control = s->focus - SNX_PAD_PRESETS;
    if (s->listening) {
        if (key == SNX_PAD_KEY_BACK) s->listening = 0;
        return SNX_PAD_DID_NOTHING;
    }
    if (control < 0) {
        if (key == SNX_PAD_KEY_UP || key == SNX_PAD_KEY_DOWN)
            s->focus = (s->focus + (key == SNX_PAD_KEY_UP ? s->presets - 1 : 1)) % s->presets;
        else if (key == SNX_PAD_KEY_RIGHT)
            s->focus = SNX_PAD_PRESETS + ORDER[0];
        else if (key == SNX_PAD_KEY_SELECT && s->in_use != s->focus) {
            s->in_use = s->focus;
            return SNX_PAD_DID_PRESET;
        } else if (key == SNX_PAD_KEY_BACK)
            return SNX_PAD_DID_LEAVE;
        return SNX_PAD_DID_NOTHING;
    }
    if (key == SNX_PAD_KEY_UP || key == SNX_PAD_KEY_DOWN) {
        int at = (at_of(control) + (key == SNX_PAD_KEY_UP ? SNX_PAD_CONTROLS - 1 : 1)) % SNX_PAD_CONTROLS;
        s->focus = SNX_PAD_PRESETS + ORDER[at];
    } else if (key == SNX_PAD_KEY_LEFT || key == SNX_PAD_KEY_RIGHT) { /* None, then A to Down, round */
        SnxPadMap map;
        snx_pad_screen_map(s, &map);
        map.bit[control] = (signed char)((map.bit[control] + 1 + (key == SNX_PAD_KEY_LEFT ? SNX_PAD_BUTTONS : 1)) %
                                             (SNX_PAD_BUTTONS + 1) -
                                         1);
        return own(s, &map);
    } else if (key == SNX_PAD_KEY_SELECT)
        s->listening = s->pad[0] != 0; /* a press needs a pad */
    else if (key == SNX_PAD_KEY_BACK)
        s->focus = s->in_use >= 0 && s->in_use < s->presets ? s->in_use : 0;
    return SNX_PAD_DID_NOTHING;
}
int snx_pad_screen_press(SnxPadScreen* s, int control) {
    int focused = s->focus - SNX_PAD_PRESETS;
    SnxPadMap map;
    signed char bit;
    if (!s->listening || focused < 0 || control < 0 || control >= SNX_PAD_CONTROLS) return SNX_PAD_DID_NOTHING;
    s->listening = 0;
    if (control == focused) return SNX_PAD_DID_NOTHING;
    snx_pad_screen_map(s, &map);
    bit = map.bit[control];
    map.bit[control] = map.bit[focused];
    map.bit[focused] = bit;
    s->focus = SNX_PAD_PRESETS + control; /* where the button went */
    return own(s, &map);
}
int snx_pad_screen_tap(SnxPadScreen* s, int id) {
    if (id < 0 || s->listening) return SNX_PAD_DID_NOTHING;
    if (id < SNX_PAD_PRESETS) {
        if (id >= s->presets) return SNX_PAD_DID_NOTHING;
        s->focus = id;
        return snx_pad_screen_key(s, SNX_PAD_KEY_SELECT);
    }
    if (id >= SNX_PAD_PRESETS + SNX_PAD_CONTROLS) return SNX_PAD_DID_NOTHING;
    if (s->focus == id) return snx_pad_screen_key(s, SNX_PAD_KEY_SELECT);
    s->focus = id;
    return SNX_PAD_DID_NOTHING;
}

/* ---- the layout ---- */
static SnxPadShape* add(SnxPadDrawing* d, int kind, int style, int id, float x, float y, float w, float h) {
    SnxPadShape* p;
    if (d->n == SNX_PAD_SHAPES) return NULL;
    p = &d->shape[d->n++];
    p->kind = (unsigned char)kind;
    p->style = (unsigned char)style;
    p->id = (short)id;
    p->x = x;
    p->y = y;
    p->w = w;
    p->h = h;
    p->text = NULL;
    return p;
}
/* text from x, y, cut to chars glyphs */
static void text(SnxPadDrawing* d, int style, float x, float y, float cw, float ch, const char* s, int chars) {
    int n = (int)strlen(s), room = (int)sizeof d->text - d->used - 1;
    SnxPadShape* p;
    if (n > chars) n = chars;
    if (n > room) n = room;
    if (n <= 0 || !(p = add(d, SNX_PAD_SHAPE_TEXT, style, -1, x, y, (float)n * cw, ch))) return;
    memcpy(d->text + d->used, s, (size_t)n);
    d->text[d->used + n] = 0;
    p->text = d->text + d->used;
    d->used += n + 1;
}
/* a box with its text a glyph in from its left, in the middle of its height */
static void box(SnxPadDrawing* d, int style, int id, float x, float y, float w, float h, float cw, float ch,
                const char* s) {
    if (!add(d, SNX_PAD_SHAPE_BOX, style, id, x, y, w, h)) return;
    text(d, style, x + cw, y + (h - ch) / 2, cw, ch, s, (int)(w / cw) - 2);
}
static int style_of(const SnxPadScreen* s, int control) {
    if (s->focus == SNX_PAD_PRESETS + control) return SNX_PAD_STYLE_FOCUS;
    return s->held >> control & 1 ? SNX_PAD_STYLE_LIT : SNX_PAD_STYLE_PLAIN;
}
/* a face button's glyph: its letter, or the symbol's in the font */
static const char* glyph(const SnxPadScreen* s, int control) {
    static const char* const SYMBOLS[] = {"x", "o", "\xFE", "\x1E"};
    int label = labels_of(s) ? s->labels[control] : SNX_PAD_LABEL_NONE;
    if (label >= SNX_PAD_LABEL_CROSS && label <= SNX_PAD_LABEL_TRIANGLE) return SYMBOLS[label - SNX_PAD_LABEL_CROSS];
    return snx_pad_control_name(control, labels_of(s));
}
/* a part of the pad, and its name in it when it fits */
static void part(SnxPadDrawing* d, const SnxPadScreen* s, int kind, int control, float x, float y, float w, float h,
                 float cw, float ch, const char* name) {
    int n = name ? (int)strlen(name) : 0;
    add(d, kind, style_of(s, control), SNX_PAD_PRESETS + control, x, y, w, h);
    if (n && (float)n * cw <= w && ch <= h)
        text(d, style_of(s, control), x + (w - (float)n * cw) / 2, y + (h - ch) / 2, cw, ch, name, n);
}
/* the pad in w x h at x, y, its height 0.62 of its width: the parts' places as fractions of the width, from the
   Xbox pad's and the PlayStation one's, none over another */
static void pad(SnxPadDrawing* d, const SnxPadScreen* s, float x, float y, float w, float cw, float ch) {
    float h = w * 0.62f, stick = 0.09f * w, face = 0.045f * w, apart = 0.095f * w, arm = 0.075f * w, thick = 0.055f * w;
    float lsx = s->symmetric ? 0.38f : 0.24f, lsy = s->symmetric ? 0.80f : 0.45f;
    float dx = (s->symmetric ? 0.24f : 0.38f) * w, dy = (s->symmetric ? 0.45f : 0.80f) * h;
    float fx = 0.76f * w, fy = 0.45f * h;
    int c;
    part(d, s, SNX_PAD_SHAPE_BAR, SNX_PAD_LT, x + 0.10f * w, y, 0.20f * w, 0.12f * h, cw, ch, "LT");
    part(d, s, SNX_PAD_SHAPE_BAR, SNX_PAD_RT, x + 0.70f * w, y, 0.20f * w, 0.12f * h, cw, ch, "RT");
    part(d, s, SNX_PAD_SHAPE_BAR, SNX_PAD_LB, x + 0.10f * w, y + 0.14f * h, 0.20f * w, 0.08f * h, cw, ch, "LB");
    part(d, s, SNX_PAD_SHAPE_BAR, SNX_PAD_RB, x + 0.70f * w, y + 0.14f * h, 0.20f * w, 0.08f * h, cw, ch, "RB");
    part(d, s, SNX_PAD_SHAPE_BAR, SNX_PAD_BACK, x + 0.41f * w, y + 0.37f * h, 0.06f * w, 0.06f * h, cw, ch, NULL);
    part(d, s, SNX_PAD_SHAPE_BAR, SNX_PAD_START, x + 0.53f * w, y + 0.37f * h, 0.06f * w, 0.06f * h, cw, ch, NULL);
    part(d, s, SNX_PAD_SHAPE_DISC, SNX_PAD_LS, x + lsx * w - stick, y + lsy * h - stick, 2 * stick, 2 * stick, cw, ch,
         NULL);
    part(d, s, SNX_PAD_SHAPE_DISC, SNX_PAD_RS, x + 0.62f * w - stick, y + 0.80f * h - stick, 2 * stick, 2 * stick, cw,
         ch, NULL);
    /* the D-pad: an arm each way round a middle that is none of them */
    add(d, SNX_PAD_SHAPE_BAR, SNX_PAD_STYLE_PLAIN, -1, x + dx - thick / 2, y + dy - thick / 2, thick, thick);
    part(d, s, SNX_PAD_SHAPE_BAR, SNX_PAD_UP, x + dx - thick / 2, y + dy - thick / 2 - arm, thick, arm, cw, ch, NULL);
    part(d, s, SNX_PAD_SHAPE_BAR, SNX_PAD_DOWN, x + dx - thick / 2, y + dy + thick / 2, thick, arm, cw, ch, NULL);
    part(d, s, SNX_PAD_SHAPE_BAR, SNX_PAD_LEFT, x + dx - thick / 2 - arm, y + dy - thick / 2, arm, thick, cw, ch, NULL);
    part(d, s, SNX_PAD_SHAPE_BAR, SNX_PAD_RIGHT, x + dx + thick / 2, y + dy - thick / 2, arm, thick, cw, ch, NULL);
    for (c = SNX_PAD_SOUTH; c <= SNX_PAD_NORTH; ++c) {
        float ox = c == SNX_PAD_WEST ? -apart : c == SNX_PAD_EAST ? apart : 0;
        float oy = c == SNX_PAD_NORTH ? -apart : c == SNX_PAD_SOUTH ? apart : 0;
        part(d, s, SNX_PAD_SHAPE_DISC, c, x + fx + ox - face, y + fy + oy - face, 2 * face, 2 * face, cw, ch,
             glyph(s, c));
    }
}
/* what the focus does, in the bar */
static void say(const SnxPadScreen* s, char* out, size_t size, const char** keys) {
    static const char* const MODES[] = {"A B X Y where the console's are", "A B X Y as the pad's labels say",
                                        "Your own: Right, then pick a control"};
    int control = s->focus - SNX_PAD_PRESETS;
    if (control < 0) {
        int mode = s->mode[s->focus];
        snprintf(out, size, "%s", mode < 0 ? "As each game's .ini says" : MODES[mode]);
        *keys = "Enter Use  \x1A Map  Esc Back";
    } else {
        SnxPadMap map;
        snx_pad_screen_map(s, &map);
        if (s->listening)
            snprintf(out, size, "Press a button on the pad for %s", snx_pad_button_name(map.bit[control]));
        else
            snprintf(out, size, "%s sends %s", snx_pad_control_name(control, labels_of(s)),
                     snx_pad_button_name(map.bit[control]));
        *keys = s->listening ? "Esc Stop"
                : s->pad[0]  ? "\x1B\x1A Change  Enter Press  Esc Presets"
                             : "\x1B\x1A Change  Esc Presets";
    }
}
void snx_pad_screen_lay(const SnxPadScreen* s, float x, float y, float w, float h, float cw, float ch,
                        SnxPadDrawing* d) {
    enum { MARGIN = 1, GAP = 2, PRESETS = 16, BOX = 16, PAD_MIN = 18 };
    int wide = w >= cw * (2 * MARGIN + PRESETS + BOX * 2 + GAP * 3 + PAD_MIN), i, c, per_row = 1, rows = 1;
    float left = x + MARGIN * cw, right = x + w - MARGIN * cw, top = y + ch / 2, bottom = y + h - 2.25f * ch;
    float bw = BOX * cw, step, bh, rows_x[2], rows_y, region_x, region_y, region_w, region_h, size;
    char line[96], bar[160];
    const char* keys;
    SnxPadMap map;
    d->n = d->used = 0;
    snx_pad_screen_map(s, &map);
    /* the bar of what the focus does, across the bottom; the keys at its right when they fit */
    say(s, bar, sizeof bar, &keys);
    if (add(d, SNX_PAD_SHAPE_BOX, SNX_PAD_STYLE_PLAIN, -1, left, y + h - 1.75f * ch, right - left, 1.5f * ch)) {
        int room = (int)((right - left) / cw) - 2, n = (int)strlen(keys);
        float ty = y + h - 1.5f * ch;
        if ((int)strlen(bar) + 2 + n > room) n = 0;
        text(d, SNX_PAD_STYLE_PLAIN, left + cw, ty, cw, ch, bar, room - (n ? n + 2 : 0));
        if (n) text(d, SNX_PAD_STYLE_SOFT, right - (float)(n + 1) * cw, ty, cw, ch, keys, n);
    }
    /* the presets: a column of boxes, or rows of them on a narrow screen, as many a row as their names fit in */
    if (!wide) {
        int longest = 0;
        for (i = 0; i < s->presets; ++i)
            if ((int)strlen(s->name[i]) > longest) longest = (int)strlen(s->name[i]);
        per_row = s->presets > 0 ? s->presets : 1;
        while (per_row > 1 && (right - left - (float)(per_row - 1) * cw) / (float)per_row < (float)(longest + 4) * cw)
            --per_row;
        rows = (s->presets + per_row - 1) / per_row;
    }
    for (i = 0; i < s->presets; ++i) {
        int style = s->focus == i ? SNX_PAD_STYLE_FOCUS : SNX_PAD_STYLE_PLAIN;
        float pw = wide ? PRESETS * cw : (right - left - (float)(per_row - 1) * cw) / (float)per_row;
        snprintf(line, sizeof line, "%s %s", s->in_use == i ? "\xFE" : " ", s->name[i]);
        box(d, style, i, wide ? left : left + (float)(i % per_row) * (pw + cw),
            wide ? top + (float)i * 2 * ch : top + (float)(i / per_row) * 2 * ch, pw, 1.5f * ch, cw, ch, line);
    }
    /* the controls' boxes: two columns of eight, either side of the pad or under it */
    if (wide) {
        rows_x[0] = left + (PRESETS + GAP) * cw;
        rows_x[1] = right - bw;
        rows_y = top;
    } else {
        bw = (right - left - GAP * cw) / 2;
        rows_x[0] = left;
        rows_x[1] = right - bw;
        rows_y = top + (float)rows * 2 * ch;
    }
    step = (bottom - rows_y) / SIDE;
    if (!wide) step = step / 2 > 1.75f * ch ? 1.75f * ch : step / 2; /* the pad takes the rest above them */
    if (step > 2 * ch) step = 2 * ch;
    if (step < ch + 2) step = ch + 2;
    bh = step >= 1.75f * ch ? 1.5f * ch : step - 2;
    if (!wide) rows_y = bottom - step * SIDE;
    for (i = 0; i < SNX_PAD_CONTROLS; ++i) {
        float by = rows_y + (float)(i % SIDE) * step;
        c = ORDER[i];
        if (by < (wide ? top : top + (float)rows * 2 * ch) || by + bh > bottom) continue; /* no room: left out */
        snprintf(line, sizeof line, "%s : %s", snx_pad_control_name(c, labels_of(s)), snx_pad_button_name(map.bit[c]));
        box(d, style_of(s, c), SNX_PAD_PRESETS + c, rows_x[i / SIDE], by, bw, bh, cw, ch, line);
    }
    /* the pad between the columns, or over them, its name above it */
    region_x = wide ? rows_x[0] + bw + GAP * cw : left;
    region_w = wide ? rows_x[1] - GAP * cw - region_x : right - left;
    region_y = wide ? top : top + (float)rows * 2 * ch;
    region_h = (wide ? bottom : rows_y - ch / 2) - region_y - 1.5f * ch;
    size = region_w < region_h / 0.62f ? region_w : region_h / 0.62f;
    if (size >= PAD_MIN * cw / 2 && region_h >= ch) {
        int n;
        snprintf(line, sizeof line, "%s", s->pad[0] ? s->pad : "No controller");
        n = (int)strlen(line) < (int)(region_w / cw) ? (int)strlen(line) : (int)(region_w / cw);
        text(d, SNX_PAD_STYLE_SOFT, region_x + (region_w - (float)n * cw) / 2, region_y, cw, ch, line, n);
        pad(d, s, region_x + (region_w - size) / 2, region_y + 1.5f * ch + (region_h - size * 0.62f) / 2, size, cw, ch);
    }
}
int snx_pad_screen_hit(const SnxPadDrawing* d, float x, float y) {
    int i;
    for (i = d->n - 1; i >= 0; --i) {
        const SnxPadShape* p = &d->shape[i];
        if (p->id >= 0 && x >= p->x && y >= p->y && x < p->x + p->w && y < p->y + p->h) return p->id;
    }
    return -1;
}
