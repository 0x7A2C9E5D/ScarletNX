/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The menu's Settings (src/gui/settings.c): the runtime's list read into the form's rows, a setting's pick and the
   options a run gets. With a runtime's path, its own --settings list too */
#include "common.h"
#include "editor.h"
#include "settings.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* as scarletnx --settings prints them, and a line of something else, a Windows line end and no last one */
static const char list[] = "Display\tdocked\t0\tint\t0\t1\t1 = TV mode\n"
                           "Display\tfullscreen\t0\tint\t0\t1\t1 = start in fullscreen\r\n"
                           "Display\tlanguage\t1\tint\t-2.14748e+09\t2.14748e+09\tGame language: 0 Japanese\n"
                           "Graphics\tresolution\t1\tfloat\t0.25\t4\tInternal 3D resolution\n"
                           "Graphics\tanisotropy\t0\tint\t0\t16\tTexture filtering at an angle\n"
                           "Graphics\tpasses\t0\tint\t0\t17\tOne value too many to go round\n"
                           "settings: not a setting\n"
                           "Input\ttouch\t1\tint\t0\t2\tThe game's touch screen\n"
                           "Input\tcontrols\t0\tint\t0\t1\tThe on-screen controls\n"
                           "Input\tcontrols_a\t\ttext\t0\t0\tA control's place\n"
                           "Debug\tlog\t1\tint\t0\t2\tLog detail";

static const GuiThemeKey* key(const GuiSettings* s, const char* name) {
    int i;
    for (i = 0; i < s->count; ++i)
        if (!strcmp(s->keys[i].key, name)) return &s->keys[i];
    return NULL;
}
static int words(const GuiThemeKey* k) {
    int n = 0;
    while (k->words[n]) ++n;
    return n;
}

int main(int argc, char** argv) {
    GuiSettings s = {0};
    GuiEditor form;
    const char* options[64];
    int fullscreen, touch, gcc, stub, prepared, launch;
    assert(!gui_settings_read(&s, list));

    /* the launcher's two, then a row a setting in the list's order, the other line left out */
    assert(s.count == GUI_SETTINGS_OWN + 10 && !strcmp(s.keys[0].key, "libgccjit") &&
           !strcmp(s.keys[1].key, "gdbstub"));
    assert(!strcmp(s.keys[0].section, "Launch") && words(&s.keys[0]) == 1 + SNX_GUI_HAS_GCCJIT);
    assert(!strcmp(s.keys[0].about, SNX_GUI_HAS_GCCJIT ? "libgccjit will be used instead of llvm"
                                                       : "Program was not compiled with libgccjit"));
    assert(!strcmp(s.keys[1].about, "Enables gdbstub on port 1234") && words(&s.keys[1]) == 2);
    assert(!strcmp(s.keys[2].section, "Display") && !strcmp(s.keys[2].key, "docked"));
    assert(!strcmp(s.keys[2].about, "1 = TV mode") && !strcmp(key(&s, "fullscreen")->about, "1 = start in fullscreen"));
    assert(!strcmp(s.keys[s.count - 1].key, "log") && !strcmp(s.keys[s.count - 1].about, "Log detail"));
    /* a whole number of few values goes round .ini and them; the others are typed */
    assert(key(&s, "touch")->type == GUI_KEY_WORD && words(key(&s, "touch")) == 4);
    assert(!strcmp(key(&s, "touch")->words[0], ".ini") && !strcmp(key(&s, "touch")->words[3], "2"));
    assert(words(key(&s, "anisotropy")) == 1 + GUI_SETTING_WORDS);
    assert(!strcmp(key(&s, "anisotropy")->words[GUI_SETTING_WORDS], "16"));
    assert(key(&s, "language")->type == GUI_KEY_TEXT && key(&s, "resolution")->type == GUI_KEY_TEXT);
    assert(key(&s, "passes")->type == GUI_KEY_TEXT);
    assert(key(&s, "controls_a")->type == GUI_KEY_TEXT);

    /* a pick: a word of its row or a text; nothing picked is the .ini's */
    assert(!gui_settings_value(&s, "touch") && !gui_settings_value(&s, "language") && !gui_settings_value(&s, "x"));
    assert(gui_settings_set(&s, "touch", "2") && !strcmp(gui_settings_value(&s, "touch"), "2"));
    assert(!gui_settings_set(&s, "touch", "3") && !strcmp(gui_settings_value(&s, "touch"), "2"));
    assert(gui_settings_set(&s, "language", "3") && !strcmp(gui_settings_value(&s, "language"), "3"));
    assert(gui_settings_set(&s, "controls_a", "0.5 0.5 100 1") && gui_settings_set(&s, "language", ""));
    assert(!gui_settings_value(&s, "language") && !gui_settings_set(&s, "nothing", "1"));
    assert(gui_settings_options(&s, 1, 0, options) == 2 && !strcmp(options[0], "--touch=2"));
    assert(!strcmp(options[1], "--controls_a=0.5 0.5 100 1"));
    assert(gui_settings_set(&s, "touch", ".ini") && gui_settings_set(&s, "controls_a", ""));

    /* the form edits them: Left and Right go round a row's words, .ini among them */
    gui_editor_form(&form, s.keys, s.count, s.values, NULL);
    form.row = (int)(key(&s, "fullscreen") - s.keys);
    gui_editor_key(&form, GUI_RIGHT);
    assert(!strcmp(gui_settings_value(&s, "fullscreen"), "0"));
    gui_editor_key(&form, GUI_RIGHT);
    gui_editor_key(&form, GUI_RIGHT);
    assert(!gui_settings_value(&s, "fullscreen"));
    gui_editor_key(&form, GUI_LEFT);
    assert(!strcmp(gui_settings_value(&s, "fullscreen"), "1"));
    gui_editor_stop(&form);

    /* a run's options: --key=value for each pick in the list's order; the launcher's own with launch, the compiler
       only for what is not a game from games/ already */
    for (fullscreen = 0; fullscreen < 3; ++fullscreen)
        for (touch = 0; touch < 4; ++touch)
            for (gcc = 0; gcc < 2; ++gcc)
                for (stub = 0; stub < 2; ++stub)
                    for (prepared = 0; prepared < 2; ++prepared)
                        for (launch = 0; launch < 2; ++launch) {
                            static const char* const picks[] = {".ini", "0", "1", "2"};
                            size_t n, i = 0;
                            char want[32];
                            assert(gui_settings_set(&s, "fullscreen", picks[fullscreen]));
                            assert(gui_settings_set(&s, "touch", picks[touch]));
                            assert(gui_settings_set(&s, "libgccjit", gcc ? "on" : "off") ==
                                   (!gcc || SNX_GUI_HAS_GCCJIT));
                            assert(gui_settings_set(&s, "gdbstub", stub ? "on" : "off"));
                            n = gui_settings_options(&s, launch, prepared, options);
                            assert(n <= (size_t)s.count + 1);
                            if (fullscreen) {
                                snprintf(want, sizeof want, "--fullscreen=%s", picks[fullscreen]);
                                assert(i < n && !strcmp(options[i++], want));
                            }
                            if (touch) {
                                snprintf(want, sizeof want, "--touch=%s", picks[touch]);
                                assert(i < n && !strcmp(options[i++], want));
                            }
                            if (launch && gcc && SNX_GUI_HAS_GCCJIT && !prepared)
                                assert(i < n && !strcmp(options[i++], "--backend=gcc"));
                            if (launch && stub) {
                                assert(i < n && !strcmp(options[i++], "-S"));
                                assert(i < n && !strcmp(options[i++], "-s"));
                            }
                            assert(i == n);
                        }
    gui_settings_free(&s);

    /* kept in a file of the game's sections and keys: what changed from where the rows started, read back over a
       new start; a pick back at its start leaves it */
    {
        char *dir = snx_temp_dir(NULL), *path = snx_path_join(dir, "config/settings.ini"), *text;
        size_t size;
        assert(!gui_settings_read(&s, list) && gui_settings_set(&s, "fullscreen", "0"));
        gui_settings_mark(&s); /* the launcher's own choice, fullscreen 0, is where it starts */
        assert(gui_settings_set(&s, "gdbstub", "on") && gui_settings_set(&s, "touch", "2"));
        assert(gui_settings_set(&s, "language", "3") && gui_settings_set(&s, "fullscreen", ".ini"));
        assert(gui_settings_set(&s, "log", "2") && gui_settings_set(&s, "log", ".ini"));
        assert(!gui_settings_save(&s, path));
        text = (char*)snx_read_file(path, &size);
        assert(text &&
               strstr(text, "\n[Launch]\ngdbstub=on\n[Display]\nfullscreen=.ini\nlanguage=3\n[Input]\ntouch=2\n"));
        assert(!strstr(text, "log=") && !strstr(text, "docked") && !strstr(text, "libgccjit"));
        free(text);
        gui_settings_free(&s);
        assert(!gui_settings_read(&s, list) && gui_settings_set(&s, "fullscreen", "0"));
        gui_settings_mark(&s);
        gui_settings_load(&s, path);
        assert(!strcmp(gui_settings_value(&s, "gdbstub"), "on") && !strcmp(gui_settings_value(&s, "language"), "3"));
        assert(!gui_settings_value(&s, "fullscreen") && !strcmp(gui_settings_value(&s, "touch"), "2"));
        assert(!gui_settings_value(&s, "log") && gui_settings_options(&s, 1, 0, options) == 4);
        /* a row back to where it started, then every one */
        gui_settings_reset(&s, (int)(key(&s, "language") - s.keys));
        assert(!gui_settings_value(&s, "language") && !strcmp(gui_settings_value(&s, "touch"), "2"));
        gui_settings_reset(&s, -1);
        assert(!strcmp(gui_settings_value(&s, "fullscreen"), "0") && !gui_settings_value(&s, "touch"));
        assert(!gui_settings_value(&s, "gdbstub") && gui_settings_options(&s, 1, 0, options) == 1);
        gui_settings_load(&s, path);                        /* and the file's picks again */
        gui_settings_load(&s, "/nonexistent/settings.ini"); /* none: nothing changes */
        assert(!strcmp(gui_settings_value(&s, "language"), "3"));
        {
            char *plain = snx_path_join(dir, "a file"), *under = snx_path_join(plain, "settings.ini");
            assert(!snx_write_file(plain, "", 0) && gui_settings_save(&s, under)); /* a folder it cannot make */
            free(under);
            free(plain);
        }
        gui_settings_free(&s);
        assert(!snx_remove_tree(dir));
        free(path);
        free(dir);
    }
    assert(!gui_settings_read(&s, "") && s.count == GUI_SETTINGS_OWN); /* no runtime: the launcher's alone */
    assert(gui_settings_options(&s, 1, 0, options) == 0 && !gui_settings_set(&s, "touch", "1"));
    gui_settings_free(&s);

    /* the runtime's own list: every line a row, each setting the menu used to have one of its items for */
    if (argc > 1) {
        const char* run[] = {argv[1], "--settings", NULL};
        char* output = NULL;
        const char* c;
        int lines = 0;
        assert(!snx_run(run, NULL, NULL, &output) && output);
        for (c = output; *c; ++c) lines += *c == '\n';
        assert(!gui_settings_read(&s, output) && s.count == GUI_SETTINGS_OWN + lines);
        assert(!strcmp(s.keys[GUI_SETTINGS_OWN].section, "Display") && !strcmp(s.keys[s.count - 1].section, "Debug"));
        assert(words(key(&s, "fullscreen")) == 3 && !key(&s, "touch") && words(key(&s, "controls")) == 3);
        assert(gui_settings_set(&s, "fullscreen", "0") && !gui_settings_set(&s, "touch", "1")); /* the game's menu's */
        assert(gui_settings_set(&s, "controls", "0") && key(&s, "resolution")->type == GUI_KEY_TEXT);
        gui_settings_free(&s);
        free(output);
    }
    puts("GUI settings: the runtime's list, picks and a run's options passed");
    return 0;
}
