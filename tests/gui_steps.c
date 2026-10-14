/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A run's output as a loading screen's steps. */
#include "steps.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void feed(GuiSteps* s, const char* text, uint64_t now) { gui_steps_feed(s, text, strlen(text), now); }
static const GuiStep* at(const GuiSteps* s, int i) { return &s->step[i]; }

int main(void) {
    GuiSteps s;
    gui_steps_reset(&s);
    assert(gui_steps_percent(&s) == -1);
    feed(&s, "Unpacking Game.nsp\nBuilding hactool\r\nExtracting the base ", 10);
    assert(s.count == 2 && !strcmp(at(&s, 0)->label, "UNPACK") && !strcmp(at(&s, 0)->detail, "Game.nsp"));
    assert(at(&s, 0)->state == GUI_STEP_DONE && at(&s, 1)->state == GUI_STEP_ACTIVE && at(&s, 1)->shown_ms == 10);
    feed(&s, "game\nan unknown line\nRecompiling Game (1 modules)\nFunctions: 9  instructions: 99\n", 20);
    assert(s.count == 4 && !strcmp(at(&s, 2)->label, "EXTRACT") && !strcmp(at(&s, 2)->detail, "the base game"));
    assert(!strcmp(at(&s, 3)->label, "RECOMPILE") && !strcmp(at(&s, 3)->detail, "Game"));
    assert(gui_steps_percent(&s) == 0);
    feed(&s, "  compiled block 1/4: code_000 (1.0 s)\n", 30);
    assert(at(&s, 3)->value == 1 && at(&s, 3)->total == 4 && gui_steps_percent(&s) == 25);
    feed(&s, "  compiled block 3/4: code_002 (1.0 s)\nLLVM module: 1.1 MB\nBuilt /games/Game\n", 40);
    assert(s.count == 5 && at(&s, 3)->state == GUI_STEP_DONE && !strcmp(at(&s, 4)->label, "BUILT"));
    assert(at(&s, 4)->state == GUI_STEP_DONE && gui_steps_percent(&s) == 100);
    feed(&s, "ScarletNX - Game\nvulkan: AMD Radeon RX 6700 XT (RADV NAVI22), Vulkan 1.4.354, driver 1\n", 50);
    feed(&s, "vulkan: pipeline cache loaded (662 KB)\nvulkan: Other GPU, Vulkan 1.3\n", 60);
    assert(s.count == 7 && !strcmp(at(&s, 5)->label, "LAUNCH") && !strcmp(at(&s, 5)->detail, "Game"));
    assert(!strcmp(at(&s, 6)->label, "GPU") && !strcmp(at(&s, 6)->detail, "AMD Radeon RX 6700 XT"));
    assert(at(&s, 6)->state == GUI_STEP_ACTIVE);
    gui_steps_finish(&s, true);
    assert(at(&s, 6)->state == GUI_STEP_DONE);

    /* a failed process fails the step it was in, even one already done, and a line cut short still counts */
    gui_steps_reset(&s);
    feed(&s, "Recompiling Broken (2 modules)\n  compiled block 1/2: x\nScarletNX - Bro", 5);
    gui_steps_finish(&s, false);
    assert(s.count == 2 && at(&s, 0)->state == GUI_STEP_DONE && at(&s, 1)->state == GUI_STEP_FAILED);
    assert(!strcmp(at(&s, 1)->detail, "Bro") && gui_steps_percent(&s) == 100);
    gui_steps_reset(&s);
    feed(&s, "Built /games/Game\n", 5);
    gui_steps_finish(&s, false);
    assert(at(&s, 0)->state == GUI_STEP_FAILED);

    /* a native launch, no recompile: 100% from the banner; the oldest steps go past the maximum */
    gui_steps_reset(&s);
    feed(&s, "ScarletNX - Native\n", 1);
    assert(gui_steps_percent(&s) == 100);
    for (int i = 0; i < GUI_STEPS_MAX + 3; ++i) {
        char line[64];
        snprintf(line, sizeof line, "Unpacking part%d\n", i);
        feed(&s, line, (uint64_t)i);
    }
    assert(s.count == GUI_STEPS_MAX && !strcmp(at(&s, GUI_STEPS_MAX - 1)->detail, "part26"));
    assert(!strcmp(at(&s, 0)->detail, "part3"));
    puts("GUI steps: driver and runner lines, progress, finish passed");
    return 0;
}
