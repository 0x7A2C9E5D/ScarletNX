/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the screen tests of the menu (gui_sdl.c, gui_setup.c) read off the real renderer, after including gui/main.c:
   text in a colour, glyph by glyph in the menu's font, and the mouse or a finger at a pixel of the render.
   SNX_SCREEN_TEST names the test in what a check that fails says */
#ifndef SNX_TEST_GUI_SCREEN_H
#define SNX_TEST_GUI_SCREEN_H
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/* the 16 rows of the menu font's glyph c: CP437's, then the Latin letters past it, then the wide letters' halves
   (gui_menu_glyph) */
static inline const uint8_t* font_rows(int c) {
    return c < 256                     ? gui_vga_font + c * 16
           : c < 256 + gui_latin_count ? gui_latin_font + (c - 256) * 16
                                       : gui_wide_font + (c - 256 - gui_latin_count) * 16;
}
/* n glyphs in color anywhere on the render: their ink pixels in color, the others not (a box's frame may cross the
   font's last row, which is empty); 1 and where they start, else 0 */
static inline int find_glyphs(App* app, const int* text, int n, SDL_Color color, int size, int* at_x, int* at_y) {
    SDL_Surface *read = SDL_RenderReadPixels(app->renderer, NULL), *image;
    int s = app->scale * size, first = -1, fx = 0, fy = 0, found = 0, x, y;
    assert(read);
    image = SDL_ConvertSurface(read, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(read);
    assert(image);
#define SNX_AT(px, py) ((const Uint8*)image->pixels + (py) * image->pitch + (px) * 4)
#define SNX_IS(c)      ((c)[0] == color.r && (c)[1] == color.g && (c)[2] == color.b)
    for (int i = 0; i < n && first < 0; ++i) /* the first ink, to look for */
        for (int gy = 0; gy < 15 && first < 0; ++gy)
            for (int gx = 0; gx < 8 && first < 0; ++gx)
                if (font_rows(text[i])[gy] & (0x80 >> gx)) {
                    first = i;
                    fx = (i * 8 + gx) * s;
                    fy = gy * s;
                }
    assert(first >= 0);
    for (y = 0; !found && y + 15 * s <= image->h; ++y)
        for (x = 0; !found && x + n * 8 * s <= image->w; ++x) {
            if (!SNX_IS(SNX_AT(x + fx, y + fy))) continue;
            found = 1;
            for (int i = 0; found && i < n; ++i)
                for (int gy = 0; found && gy < 15; ++gy)
                    for (int gx = 0; found && gx < 8; ++gx) {
                        int ink = (font_rows(text[i])[gy] & (0x80 >> gx)) != 0;
                        found = ink == SNX_IS(SNX_AT(x + (i * 8 + gx) * s, y + gy * s));
                    }
            if (found) {
                *at_x = x;
                *at_y = y;
            }
        }
#undef SNX_IS
#undef SNX_AT
    SDL_DestroySurface(image);
    return found;
}
/* text in color, glyph by glyph in the font's own bytes (CP437) */
static inline int find_text(App* app, const char* text, SDL_Color color, int size, int* at_x, int* at_y) {
    int glyphs[256], n = 0;
    while (text[n]) {
        assert(n < 256);
        glyphs[n] = (unsigned char)text[n];
        ++n;
    }
    return find_glyphs(app, glyphs, n, color, size, at_x, at_y);
}
/* UTF-8 text in color, a glyph each letter as the menu draws it (gui_menu_glyph; a wide one's two halves), size times
   the text's letters */
static inline int find_letters(App* app, const char* text, SDL_Color color, int size, int* at_x, int* at_y) {
    int glyphs[256], n = 0;
    while (*text) {
        int c = gui_menu_glyph(gui_utf8_step(&text));
        assert(n + 2 <= 256);
        glyphs[n++] = c;
        if (gui_glyph_cols(c) == 2) glyphs[n++] = c + 1;
    }
    return find_glyphs(app, glyphs, n, color, size, at_x, at_y);
}
/* the text drawn, or not, in color, its letters size times the text's: a check that fails names its line */
#define shows(app, text, color)           shows_at(__LINE__, app, text, color, 1, 1)
#define hides(app, text, color)           shows_at(__LINE__, app, text, color, 1, 0)
#define shows_big(app, text, color, size) shows_at(__LINE__, app, text, color, size, 1)
static inline void shows_at(int line, App* app, const char* text, SDL_Color color, int size, int drawn) {
    int x, y;
    if (find_text(app, text, color, size, &x, &y) == drawn) return;
    if (drawn)
        fprintf(stderr, "%s:%d: \"%s\" is not drawn in %02x%02x%02x\n", SNX_SCREEN_TEST, line, text, color.r, color.g,
                color.b);
    else
        fprintf(stderr, "%s:%d: \"%s\" is drawn in %02x%02x%02x at %d,%d\n", SNX_SCREEN_TEST, line, text, color.r,
                color.g, color.b, x, y);
    if (getenv("SNX_GUI_FAILED_SHOT")) { /* what was drawn, to look at */
        SDL_Surface* image = SDL_RenderReadPixels(app->renderer, NULL);
        if (image) SDL_SaveBMP(image, getenv("SNX_GUI_FAILED_SHOT"));
        SDL_DestroySurface(image);
    }
    assert(!"the text drawn");
}
/* the same in the foot's letters, a size under the rest's (foot_scale) */
#define shows_foot(app, text, color) foot_shows_at(__LINE__, app, text, color, 1)
#define hides_foot(app, text, color) foot_shows_at(__LINE__, app, text, color, 0)
static inline void foot_shows_at(int line, App* app, const char* text, SDL_Color color, int drawn) {
    int scale = app->scale;
    app->scale = foot_scale(app);
    shows_at(line, app, text, color, 1, drawn);
    app->scale = scale;
}
/* the mouse at pixel x, y of the render, moved there or clicking */
static inline void mouse_px(App* app, int x, int y, bool click) {
    SDL_Event e = {0};
    float wx, wy;
    layout(app);
    assert(SDL_RenderCoordinatesToWindow(app->renderer, (float)x + 0.5f, (float)y + 0.5f, &wx, &wy));
    e.type = click ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_MOTION;
    e.button.button = SDL_BUTTON_LEFT;
    if (click) {
        e.button.x = wx;
        e.button.y = wy;
    } else {
        e.motion.x = wx;
        e.motion.y = wy;
    }
    event(app, &e);
}
/* a finger tapping pixel x, y of the render */
static inline void tap_px(App* app, int x, int y) {
    SDL_Event e = {0};
    float wx, wy;
    int w, h;
    layout(app);
    SDL_GetWindowSize(app->window, &w, &h);
    assert(SDL_RenderCoordinatesToWindow(app->renderer, (float)x + 0.5f, (float)y + 0.5f, &wx, &wy));
    e.tfinger.fingerID = 9;
    e.tfinger.x = wx / (float)w;
    e.tfinger.y = wy / (float)h;
    e.type = SDL_EVENT_FINGER_DOWN;
    event(app, &e);
    e.type = SDL_EVENT_FINGER_UP;
    event(app, &e);
}
static inline void point(App* app, GuiRect b, bool click) { mouse_px(app, b.x + b.w / 2, b.y + b.h / 2, click); }
/* choice i's box in the dialog placed (ui.txt's scene Choices), as drawn: its row past the cursor's margin; none while it
   is scrolled out */
static inline GuiRect choice_box(const App* app, const GuiChoices* c, int i) {
    size_t first = c->lists[UI_CHOICES_ROWS].first;
    if (i < 0 || (size_t)i < first || (size_t)i >= first + ui_choices_shown(c->rects, app->scale, UI_CHOICES_ROWS))
        return gui_rect(0, 0, 0, 0);
    return ui_row_box(app, ui_choices_row(c->rects, app->scale, UI_CHOICES_ROWS, (size_t)i - first));
}
#endif
