/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What tools/uigen generates from tests/ui_test.txt, built and run: each scene's layout at two UI scales, the rows its
   lists show, its drawing (the calls main.c's ui_draw_row and ui_draw_label would get, with the attributes the code
   and the hidden rows give), its hit test, the actions a hit, a hover and a hotkey call, and how a list follows its
   focus. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL_keycode.h>
#include "gui/ui.h"

/* the app the callbacks get: what they were called with */
struct App {
    int calls[8], hits[8];
    size_t rows[8];
    bool keys_missing; /* Load Switch blinks in the failure colour */
    size_t active;     /* LoadList's row in the active colour, + 1; 0: none */
    int width;         /* the width Games' get_row_info was given last */
};
enum { LOAD_SWITCH, LOAD_3DS, SETTINGS, GAMES, OK, BACK, DATA };
/* the row attributes are a bit each, none shared */
typedef char ui_attr_bits[(UI_ATTR_FAILURE | UI_ATTR_BLINK | UI_ATTR_MORE_UP | UI_ATTR_MORE_DOWN | UI_ATTR_ACTIVE |
                           UI_ATTR_LABEL | UI_ATTR_OPEN | UI_ATTR_TITLE) == 255
                              ? 1
                              : -1];

/* what was drawn, in order */
typedef struct {
    GuiRect r;
    char text[64];
    const void* image;
    unsigned color, attr;
    int focused, label;
} Drawn;
static struct {
    Drawn drawn[32];
    int ndrawn;
    char image; /* where the last game's image is: what the drawing passes on */
} g_gui_ui;

void ui_draw_row(App* app, GuiRect r, const char* text, const void* image, unsigned color, unsigned attr,
                 bool focused) {
    Drawn* d = &g_gui_ui.drawn[g_gui_ui.ndrawn++];
    (void)app;
    d->r = r;
    snprintf(d->text, sizeof d->text, "%s", text);
    d->image = image;
    d->color = color;
    d->attr = attr;
    d->focused = focused;
    d->label = 0;
}
const char* ui_tr(const char* english) { return english; }
void ui_draw_label(App* app, GuiRect r, const char* text, unsigned color) {
    ui_draw_row(app, r, text, NULL, color, 0, false);
    g_gui_ui.drawn[g_gui_ui.ndrawn - 1].label = 1;
}

static void called(App* app, int which, uint32_t hit, size_t row) {
    app->calls[which]++;
    app->hits[which] = (int)hit;
    app->rows[which] = row;
}
void ui_main_load_list_load_switch_action_cb(App* app, uint32_t hit) { called(app, LOAD_SWITCH, hit, 0); }
unsigned ui_main_load_list_attr_cb(App* app, size_t row) { return app->active == row + 1 ? UI_ATTR_ACTIVE : 0; }
unsigned ui_main_load_list_load_switch_attr_cb(App* app) {
    return app->keys_missing ? UI_ATTR_FAILURE | UI_ATTR_BLINK : 0;
}
void ui_main_load_list_load_3ds_cia_action_cb(App* app, uint32_t hit) { called(app, LOAD_3DS, hit, 0); }
#ifdef SDL_PLATFORM_ANDROID
void ui_main_load_list_open_data_folder_action_cb(App* app, uint32_t hit) { called(app, DATA, hit, 0); }
#endif
void ui_main_load_list_settings_action_cb(App* app, uint32_t hit) { called(app, SETTINGS, hit, 0); }
/* four games: a title, the second in the failure colour, the third with no label and open (the focus's look, no
   cursor), the last with an image */
bool ui_main_games_get_row_info(App* app, size_t row, int width, UiRowInfo* info) {
    static const char* const names[] = {"Game 0", "Game 1", NULL, "Game 3"};
    app->width = width;
    if (row >= 4) return false;
    info->label = names[row];
    if (row == 3) info->image = &g_gui_ui.image;
    info->attr = row == 0 ? UI_ATTR_TITLE : row == 1 ? UI_ATTR_FAILURE : row == 2 ? UI_ATTR_OPEN : 0;
    return true;
}
void ui_main_games_row_action_cb(App* app, size_t row, uint32_t hit) { called(app, GAMES, hit, row); }
void ui_main_status_update_cb(App* app, char* text, size_t size) {
    (void)app;
    snprintf(text, size, "ready");
}
void ui_row_ok_action_cb(App* app, uint32_t hit) { called(app, OK, hit, 0); }
void ui_row_back_action_cb(App* app, uint32_t hit) { called(app, BACK, hit, 0); }
void ui_touch_big_a_action_cb(App* app, uint32_t hit) { called(app, OK, hit, 0); }
void ui_touch_big_b_action_cb(App* app, uint32_t hit) { called(app, OK, hit, 0); }
void ui_touch_big_c_action_cb(App* app, uint32_t hit) { called(app, OK, hit, 0); }
unsigned ui_row_back_attr_cb(App* app) {
    (void)app;
    return UI_ATTR_FAILURE;
}

static int same(GuiRect r, int x, int y, int w, int h) { return r.x == x && r.y == y && r.w == w && r.h == h; }
static void is(GuiRect r, int x, int y, int w, int h) {
    if (!same(r, x, y, w, h)) fprintf(stderr, "%d,%d %dx%d, not %d,%d %dx%d\n", r.x, r.y, r.w, r.h, x, y, w, h);
    assert(same(r, x, y, w, h));
}

/* padding 4px, gap 10px, a column: the label's row, two lists sharing what is left, the status a fifth of the inside */
static void test_layout(void) {
    GuiRect r[UI_MAIN_ITEMS], o[UI_ROW_ITEMS];
    ui_main_layout(gui_rect(10, 20, 400, 300), 1, r);
    is(r[UI_MAIN_PICK_ONE], 14, 24, 392, 17);
    is(r[UI_MAIN_LOAD_LIST], 14, 51, 392, 93);
    is(r[UI_MAIN_GAMES], 14, 154, 392, 93);
    is(r[UI_MAIN_STATUS], 14, 257, 392, 58);
    is(r[UI_MAIN_LOAD_LIST_LOAD_SWITCH], 0, 0, 0, 0);    /* a row: its list's */
    assert(ui_main_shown(r, 1, UI_MAIN_LOAD_LIST) == 3); /* five fit, max_rows 3 */
    assert(ui_main_shown(r, 1, UI_MAIN_GAMES) == 5);
    assert(ui_main_shown(r, 1, UI_MAIN_STATUS) == 0);
    /* at 2x the pixels double, the fractions stay: fewer rows fit */
    ui_main_layout(gui_rect(10, 20, 400, 300), 2, r);
    is(r[UI_MAIN_PICK_ONE], 18, 28, 384, 34);
    is(r[UI_MAIN_LOAD_LIST], 18, 82, 384, 66);
    assert(ui_main_shown(r, 2, UI_MAIN_LOAD_LIST) == 1);
    /* a row, centred both ways: OK 60 by 20, Back a quarter wide and the whole height */
    ui_row_layout(gui_rect(0, 0, 400, 100), 1, o);
    is(o[UI_ROW_OK], 120, 40, 60, 20);
    is(o[UI_ROW_BACK], 180, 0, 100, 100);
    /* a platform's own row metrics: Android's rows 30px high, the desktop's at most 2, the plain ones elsewhere */
    {
        GuiRect t[UI_TOUCH_ITEMS];
        ui_touch_layout(gui_rect(0, 0, 100, 100), 1, t);
#ifdef SDL_PLATFORM_ANDROID
        is(ui_touch_row(t, 1, UI_TOUCH_BIG, 1), 0, 32, 100, 30);
        assert(ui_touch_shown(t, 1, UI_TOUCH_BIG) == 3); /* 100 / 32: 3 */
#else
        is(ui_touch_row(t, 1, UI_TOUCH_BIG, 1), 0, 19, 100, 17);
        assert(ui_touch_shown(t, 1, UI_TOUCH_BIG) == 2); /* 5 fit, max_rows 2 */
#endif
    }
    puts("PASS the layouts at 1x and 2x, the rows that fit, a platform's own metrics");
}

static void test_draw(void) {
    GuiRect r[UI_MAIN_ITEMS];
    UiList lists[UI_MAIN_ITEMS] = {{0}};
    App app = {0};
    const Drawn* d = g_gui_ui.drawn;
    lists[UI_MAIN_LOAD_LIST].focus = 1;
    lists[UI_MAIN_GAMES].focus = 3;
    ui_main_layout(gui_rect(10, 20, 400, 300), 1, r);
    g_gui_ui.ndrawn = 0;
    ui_main_draw(&app, r, 1, lists);
    /* the label, three of LoadList's four rows (max_rows), Games's four (the fifth is past its last), the status */
    assert(g_gui_ui.ndrawn == 1 + 3 + 4 + 1);
    assert(d[0].label && !strcmp(d[0].text, "Pick one") && d[0].color == UI_COLOR_SOFT);
    is(d[0].r, 14, 24, 392, 17);
    assert(!strcmp(d[1].text, "Load Switch") && !d[1].focused && !d[1].attr); /* its attr_cb: keys in use */
    is(d[1].r, 14, 51, 200, 17);                                              /* max_row_width */
    assert(!strcmp(d[2].text, "Load 3DS CIA") && d[2].focused);
    is(d[2].r, 14, 70, 200, 17);
    is(ui_main_row(r, 1, UI_MAIN_LOAD_LIST, 1), 14, 70, 200, 17);       /* as drawn */
    is(ui_main_row(r, 1, UI_MAIN_GAMES, 3), 14, 154 + 3 * 19, 300, 17); /* max_row_width */
    is(ui_main_row(r, 1, UI_MAIN_STATUS, 0), 0, 0, 0, 0);               /* not a list */
#ifdef SDL_PLATFORM_ANDROID
    assert(!strcmp(d[3].text, "Open data folder"));
#else
    assert(!strcmp(d[3].text, "Settings") && d[3].color == UI_COLOR_FOCUS);
#endif
    assert(d[2].attr == 0 && d[3].attr == UI_ATTR_MORE_DOWN); /* the last row shown, rows below it */
    assert(!strcmp(d[4].text, "Game 0") && d[4].attr == UI_ATTR_TITLE);
    assert(!strcmp(d[5].text, "Game 1") && d[5].attr == UI_ATTR_FAILURE);
    assert(!strcmp(d[6].text, "(null)") && d[6].attr == UI_ATTR_OPEN && !d[6].focused);
    assert(!strcmp(d[7].text, "Game 3") && d[7].focused);
    assert(d[7].image == &g_gui_ui.image && !d[4].image && !d[5].image && !d[6].image); /* a row's own image */
    assert(!d[0].image && !d[3].image); /* a static list's rows have none */
    is(d[7].r, 14, 154 + 3 * 19, 300, 17);
    assert(app.width == 300); /* each row asked with its own width, max_row_width */
    assert(d[8].label && !strcmp(d[8].text, "ready"));
    is(d[8].r, 14, 257, 392, 58);
    /* scrolled: the rows from the first, an arrow up on it; the arrow down only while rows are left below */
    lists[UI_MAIN_LOAD_LIST].first = 1;
    lists[UI_MAIN_GAMES].first = 1;
    g_gui_ui.ndrawn = 0;
    ui_main_draw(&app, r, 1, lists);
    assert(!strcmp(d[1].text, "Load 3DS CIA") && d[1].focused && d[1].attr == UI_ATTR_MORE_UP);
#ifdef SDL_PLATFORM_ANDROID
    assert(!strcmp(d[3].text, "Settings") && d[3].attr == UI_ATTR_MORE_DOWN);
#else
    assert(!strcmp(d[3].text, "Decor") && d[3].attr == 0);
#endif
    assert(!strcmp(d[4].text, "Game 1") && d[4].attr == (UI_ATTR_FAILURE | UI_ATTR_MORE_UP));
    assert(!strcmp(d[6].text, "Game 3") && d[6].attr == 0);
    /* the code's attributes: Load Switch with no keys; a scene's button */
    app.keys_missing = true;
    lists[UI_MAIN_LOAD_LIST].first = 0;
    g_gui_ui.ndrawn = 0;
    ui_main_draw(&app, r, 1, lists);
    assert(!strcmp(d[1].text, "Load Switch") && d[1].attr == (UI_ATTR_FAILURE | UI_ATTR_BLINK));
    app.active = 2; /* the list's: any row's, by its index, with the row's own */
    g_gui_ui.ndrawn = 0;
    ui_main_draw(&app, r, 1, lists);
    assert(d[1].attr == (UI_ATTR_FAILURE | UI_ATTR_BLINK) && d[2].attr == UI_ATTR_ACTIVE);
    app.active = 1;
    g_gui_ui.ndrawn = 0;
    ui_main_draw(&app, r, 1, lists);
    assert(d[1].attr == (UI_ATTR_FAILURE | UI_ATTR_BLINK | UI_ATTR_ACTIVE) && d[2].attr == 0);
    {
        GuiRect o[UI_ROW_ITEMS];
        ui_row_layout(gui_rect(0, 0, 400, 100), 1, o);
        g_gui_ui.ndrawn = 0;
        ui_row_draw(&app, o, 1, NULL);
        assert(g_gui_ui.ndrawn == 2 && !strcmp(d[1].text, "Back") && d[1].attr == UI_ATTR_FAILURE && !d[0].attr);
    }
    /* at 2x Games shows one row: the arrow down, from get_row_info's next row */
    lists[UI_MAIN_GAMES].first = 0;
    ui_main_layout(gui_rect(10, 20, 400, 300), 2, r);
    assert(ui_main_shown(r, 2, UI_MAIN_GAMES) == 1);
    g_gui_ui.ndrawn = 0;
    ui_main_draw(&app, r, 2, lists);
    assert(!strcmp(d[2].text, "Game 0") && d[2].attr == (UI_ATTR_TITLE | UI_ATTR_MORE_DOWN) && app.width == 384 &&
           d[2].r.w == 384);
    lists[UI_MAIN_GAMES].first = 3; /* its last row: none below */
    g_gui_ui.ndrawn = 0;
    ui_main_draw(&app, r, 2, lists);
    assert(!strcmp(d[2].text, "Game 3") && d[2].attr == UI_ATTR_MORE_UP);
    puts("PASS the drawing: labels, rows from items and from get_row_info, focus, colours, scroll, attributes");
}

static void test_hit(void) {
    GuiRect r[UI_MAIN_ITEMS];
    UiList lists[UI_MAIN_ITEMS] = {{0}};
    App app = {0};
    int item;
    size_t row;
    ui_main_layout(gui_rect(10, 20, 400, 300), 1, r);
    /* a row of LoadList: its button, and the bits of the event */
    assert(ui_main_hit(&app, r, 1, lists, 20, 75, UI_HIT_CLICK, &item, &row) == (UI_HIT_HOVER | UI_HIT_CLICK));
    assert(item == UI_MAIN_LOAD_LIST_LOAD_3DS_CIA && row == 1);
    ui_main_act(&app, item, row, UI_HIT_CLICK);
    assert(app.calls[LOAD_3DS] == 1 && app.hits[LOAD_3DS] == UI_HIT_CLICK);
    /* past max_row_width: no row there, in a list with items or without */
    assert(!ui_main_hit(&app, r, 1, lists, 300, 60, 0, &item, &row) && item == -1);
    assert(!ui_main_hit(&app, r, 1, lists, 350, 157, 0, &item, &row) && item == -1);
    /* a row of Games, scrolled one down: its row from the list's first */
    lists[UI_MAIN_GAMES].first = 1;
    assert(ui_main_hit(&app, r, 1, lists, 20, 154 + 19 + 3, UI_HIT_RELEASE, &item, &row) ==
           (UI_HIT_HOVER | UI_HIT_RELEASE));
    assert(item == UI_MAIN_GAMES && row == 2);
    ui_main_act(&app, item, row, UI_HIT_RELEASE);
    assert(app.calls[GAMES] == 1 && app.rows[GAMES] == 2 && app.hits[GAMES] == UI_HIT_RELEASE);
    /* a row past get_row_info's last is not there; each asked with its width */
    app.width = 0;
    assert(!ui_main_hit(&app, r, 1, lists, 20, 154 + 19 * 3 + 3, 0, &item, &row));
    assert(app.width == 300);
    /* the label is hit, and acts on nothing */
    assert(ui_main_hit(&app, r, 1, lists, 20, 260, 0, &item, &row) == UI_HIT_HOVER && item == UI_MAIN_STATUS);
    ui_main_act(&app, item, row, UI_HIT_CLICK);
    assert(app.calls[LOAD_3DS] == 1 && app.calls[GAMES] == 1);
    /* a hover takes the same path: the callback gets UI_HIT_HOVER alone */
    assert(ui_main_hit(&app, r, 1, lists, 20, 75, 0, &item, &row) == UI_HIT_HOVER);
    ui_main_act(&app, item, row, UI_HIT_HOVER);
    assert(app.calls[LOAD_3DS] == 2 && app.hits[LOAD_3DS] == UI_HIT_HOVER);
    puts("PASS the hit test's items, rows and bits, and the actions a hit and a hover call");
}

static void test_follow_and_keys(void) {
    GuiRect r[UI_MAIN_ITEMS];
    UiList list;
    App app = {0};
    ui_main_layout(gui_rect(10, 20, 400, 300), 1, r);
    /* LoadList keeps its focus in the middle (center, the default), as far as its rows go */
    list.first = 0;
    list.focus = 3;
    ui_main_follow(r, 1, UI_MAIN_LOAD_LIST, 4, &list);
    assert(list.first == 1);
    list.focus = 1;
    ui_main_follow(r, 1, UI_MAIN_LOAD_LIST, 4, &list);
    assert(list.first == 0);
    /* Games (edge) moves as little as it takes */
    list.first = 0;
    list.focus = 7;
    ui_main_follow(r, 1, UI_MAIN_GAMES, 10, &list);
    assert(list.first == 3);
    list.focus = 2;
    ui_main_follow(r, 1, UI_MAIN_GAMES, 10, &list);
    assert(list.first == 2);
    list.focus = 4;
    ui_main_follow(r, 1, UI_MAIN_GAMES, 10, &list);
    assert(list.first == 2);
    /* the hotkeys, each scene its own */
    assert(ui_main_key(&app, SDLK_F4, UI_MOD_ALT) && app.calls[LOAD_3DS] == 1 && app.hits[LOAD_3DS] == UI_HIT_CLICK);
    assert(!ui_main_key(&app, SDLK_F4, 0) && !ui_main_key(&app, 'o', 0) && app.calls[LOAD_3DS] == 1);
    assert(ui_row_key(&app, 'o', 0) && app.calls[OK] == 1);
    /* the stub of a decorative row does nothing */
    ui_main_act(&app, UI_MAIN_LOAD_LIST_DECOR, 0, UI_HIT_CLICK);
    puts("PASS how lists follow their focus, and the hotkeys");
}

int main(void) {
    test_layout();
    test_draw();
    test_hit();
    test_follow_and_keys();
    return 0;
}
