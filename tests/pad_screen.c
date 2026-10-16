/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The button map screen (src/common/utilities/pad_screen.c): what the keys, a press on the pad and a tap do to it,
   and its layout, wide and narrow, every part inside the area and none over another */
#include "pad_screen.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { A, B, X, Y };
static const char* const NAMES[] = {"Each game's", "By position", "By label", "Custom"};
static const int MODES[] = {-1, SNX_PAD_BY_POSITION, SNX_PAD_BY_LABEL, SNX_PAD_CUSTOM};

/* whether two shapes overlap: touching edges (to rounding) do not; a text over the box or part it is written in does
   not count */
static int over(const SnxPadShape* a, const SnxPadShape* b) {
    const float e = 0.01f;
    return a->x < b->x + b->w - e && b->x < a->x + a->w - e && a->y < b->y + b->h - e && b->y < a->y + a->h - e;
}
static int inside(const SnxPadShape* in, const SnxPadShape* out) {
    return in->x >= out->x && in->y >= out->y && in->x + in->w <= out->x + out->w && in->y + in->h <= out->y + out->h;
}
/* every shape in the area, none over another but a text in its own box or part */
static void tidy(const SnxPadDrawing* d, float x, float y, float w, float h) {
    int i, j;
    assert(d->n > 0);
    for (i = 0; i < d->n; ++i) {
        const SnxPadShape* p = &d->shape[i];
        assert(p->x >= x && p->y >= y && p->x + p->w <= x + w + 0.01f && p->y + p->h <= y + h + 0.01f);
        for (j = 0; j < i; ++j) {
            const SnxPadShape* q = &d->shape[j];
            int text_in =
                (p->kind == SNX_PAD_SHAPE_TEXT && inside(p, q)) || (q->kind == SNX_PAD_SHAPE_TEXT && inside(q, p));
            if (over(p, q) && !text_in) {
                fprintf(stderr, "shape %d (kind %d id %d) over shape %d (kind %d id %d)\n", i, p->kind, p->id, j,
                        q->kind, q->id);
                assert(0);
            }
        }
    }
}
static int has_text(const SnxPadDrawing* d, const char* s) {
    int i;
    for (i = 0; i < d->n; ++i)
        if (d->shape[i].kind == SNX_PAD_SHAPE_TEXT && strstr(d->shape[i].text, s)) return 1;
    return 0;
}

int main(void) {
    SnxPadScreen s;
    SnxPadDrawing d;
    SnxPadMap map;
    static const int sizes[][2] = {{1280, 800}, {2560, 1440}, {800, 480}, {640, 400}, {480, 800}, {360, 640}};
    size_t k;

    /* the presets, the one in use focused; Up/Down go round them; Select uses the focused one */
    snx_pad_screen_start(&s, 4, NAMES, MODES, 2);
    assert(s.focus == 2 && s.in_use == 2);
    assert(snx_pad_screen_key(&s, SNX_PAD_KEY_DOWN) == SNX_PAD_DID_NOTHING && s.focus == 3);
    assert(snx_pad_screen_key(&s, SNX_PAD_KEY_DOWN) == SNX_PAD_DID_NOTHING && s.focus == 0);
    assert(snx_pad_screen_key(&s, SNX_PAD_KEY_UP) == SNX_PAD_DID_NOTHING && s.focus == 3);
    assert(snx_pad_screen_key(&s, SNX_PAD_KEY_UP) == SNX_PAD_DID_NOTHING && s.focus == 2);
    assert(snx_pad_screen_key(&s, SNX_PAD_KEY_SELECT) == SNX_PAD_DID_NOTHING); /* in use already */
    s.focus = 1;
    assert(snx_pad_screen_key(&s, SNX_PAD_KEY_SELECT) == SNX_PAD_DID_PRESET && s.in_use == 1);
    snx_pad_screen_map(&s, &map);
    assert(map.bit[SNX_PAD_SOUTH] == B && map.bit[SNX_PAD_EAST] == A); /* by position */
    s.focus = 2;
    assert(snx_pad_screen_key(&s, SNX_PAD_KEY_SELECT) == SNX_PAD_DID_PRESET && s.in_use == 2);
    snx_pad_screen_map(&s, &map);
    assert(map.bit[SNX_PAD_SOUTH] == A); /* by an Xbox pad's labels: none known */

    /* Right goes to the controls; Left/Right change the focused one's button, which makes Custom the one in use */
    assert(snx_pad_screen_key(&s, SNX_PAD_KEY_RIGHT) == SNX_PAD_DID_NOTHING && s.focus == SNX_PAD_PRESETS + SNX_PAD_LT);
    s.focus = SNX_PAD_PRESETS + SNX_PAD_SOUTH;
    assert(snx_pad_screen_key(&s, SNX_PAD_KEY_RIGHT) == SNX_PAD_DID_MAP && s.in_use == 3);
    snx_pad_screen_map(&s, &map);
    assert(map.bit[SNX_PAD_SOUTH] == B && s.own.bit[SNX_PAD_SOUTH] == B && map.bit[SNX_PAD_EAST] == B);
    assert(snx_pad_screen_key(&s, SNX_PAD_KEY_LEFT) == SNX_PAD_DID_MAP);
    assert(snx_pad_screen_key(&s, SNX_PAD_KEY_LEFT) == SNX_PAD_DID_MAP);
    snx_pad_screen_map(&s, &map);
    assert(map.bit[SNX_PAD_SOUTH] == SNX_PAD_NONE); /* None comes before A, round */
    /* Select waits for a press, only with a pad; the press swaps the two controls' buttons, the focus following */
    assert(snx_pad_screen_key(&s, SNX_PAD_KEY_SELECT) == SNX_PAD_DID_NOTHING && !s.listening);
    snprintf(s.pad, sizeof s.pad, "Acer Gamepad");
    assert(snx_pad_screen_key(&s, SNX_PAD_KEY_SELECT) == SNX_PAD_DID_NOTHING && s.listening);
    assert(snx_pad_screen_key(&s, SNX_PAD_KEY_DOWN) == SNX_PAD_DID_NOTHING &&
           s.focus == SNX_PAD_PRESETS + SNX_PAD_SOUTH);
    assert(snx_pad_screen_press(&s, SNX_PAD_EAST) == SNX_PAD_DID_MAP && !s.listening);
    snx_pad_screen_map(&s, &map);
    assert(map.bit[SNX_PAD_SOUTH] == B && map.bit[SNX_PAD_EAST] == SNX_PAD_NONE &&
           s.focus == SNX_PAD_PRESETS + SNX_PAD_EAST);
    assert(snx_pad_screen_press(&s, SNX_PAD_SOUTH) == SNX_PAD_DID_NOTHING); /* not waiting */
    /* Back stops waiting, then goes to the presets, then leaves */
    snx_pad_screen_key(&s, SNX_PAD_KEY_SELECT);
    assert(s.listening && snx_pad_screen_key(&s, SNX_PAD_KEY_BACK) == SNX_PAD_DID_NOTHING && !s.listening);
    assert(snx_pad_screen_key(&s, SNX_PAD_KEY_BACK) == SNX_PAD_DID_NOTHING && s.focus == 3);
    assert(snx_pad_screen_key(&s, SNX_PAD_KEY_BACK) == SNX_PAD_DID_LEAVE);

    /* a tap: a preset is used; a control focused, then tapped again waited for */
    assert(snx_pad_screen_tap(&s, 1) == SNX_PAD_DID_PRESET && s.in_use == 1);
    assert(snx_pad_screen_tap(&s, SNX_PAD_PRESETS + SNX_PAD_NORTH) == SNX_PAD_DID_NOTHING && !s.listening);
    assert(snx_pad_screen_tap(&s, SNX_PAD_PRESETS + SNX_PAD_NORTH) == SNX_PAD_DID_NOTHING && s.listening);
    assert(snx_pad_screen_tap(&s, 0) == SNX_PAD_DID_NOTHING && s.listening); /* waiting: taps do nothing */
    s.listening = 0;
    assert(snx_pad_screen_tap(&s, -1) == SNX_PAD_DID_NOTHING && snx_pad_screen_tap(&s, 99) == SNX_PAD_DID_NOTHING);

    /* the layout: wide, the presets, the controls' two columns and the pad between them; narrow, stacked; small, the
       boxes alone; each size every part inside the area and none over another; a tap finds what is under it */
    s.labels[0] = SNX_PAD_LABEL_CROSS; /* a PlayStation pad: its symbols, its sticks side by side */
    s.labels[1] = SNX_PAD_LABEL_CIRCLE;
    s.labels[2] = SNX_PAD_LABEL_SQUARE;
    s.labels[3] = SNX_PAD_LABEL_TRIANGLE;
    for (k = 0; k < sizeof sizes / sizeof sizes[0]; ++k) {
        int scale, sym;
        for (sym = 0; sym < 2; ++sym)
            for (scale = 1; scale <= 2; ++scale) {
                float w = (float)sizes[k][0], h = (float)sizes[k][1], cw = 8.0f * (float)scale,
                      ch = 16.0f * (float)scale;
                int i, found = 0;
                s.symmetric = sym;
                s.focus = SNX_PAD_PRESETS + SNX_PAD_SOUTH;
                snx_pad_screen_lay(&s, 0, 0, w, h, cw, ch, &d);
                tidy(&d, 0, 0, w, h);
                assert(has_text(&d, "Cross sends") && has_text(&d, "By position"));
                for (i = 0; i < d.n; ++i)
                    if (d.shape[i].id == SNX_PAD_PRESETS + SNX_PAD_SOUTH && d.shape[i].kind == SNX_PAD_SHAPE_BOX) {
                        found = 1;
                        assert(d.shape[i].style == SNX_PAD_STYLE_FOCUS);
                        assert(snx_pad_screen_hit(&d, d.shape[i].x + 1, d.shape[i].y + 1) ==
                               SNX_PAD_PRESETS + SNX_PAD_SOUTH);
                    }
                assert(found); /* the focused control's box shows at every size */
            }
    }
    /* a held control lit on the pad, and the pad named above it */
    s.held = 1u << SNX_PAD_RT;
    s.focus = 0;
    snx_pad_screen_lay(&s, 0, 0, 1280, 800, 16, 32, &d);
    assert(has_text(&d, "Acer Gamepad"));
    {
        int i, lit = 0;
        for (i = 0; i < d.n; ++i)
            lit |= d.shape[i].id == SNX_PAD_PRESETS + SNX_PAD_RT && d.shape[i].style == SNX_PAD_STYLE_LIT;
        assert(lit);
    }
    puts("pad_screen passed");
    return 0;
}
