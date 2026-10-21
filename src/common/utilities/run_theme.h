/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The launcher's look for a loading screen, as the runtime draws one of its own (the shaders it prepares before the
   game starts, nx/runtime/platform/overlay.c): the [run] part of the theme the launcher's Settings > App picked
   (src/gui/theme.c fills it, docs/GUI.md describes the keys) */
#ifndef SNX_RUN_THEME_H
#define SNX_RUN_THEME_H
#include <stdint.h>

typedef struct {
    uint32_t page, ink, soft, dim, failure; /* 0xRRGGBB */
    char name[64];                          /* the theme's, as its [theme] name says */
    char title[64];                         /* [head] loading: the loading screen's title word */
    int steps;                              /* layout = steps: labelled steps; console: the lines as printed */
    int texture, bar;                       /* faint lines over the page; the progress bar and its percentage */
    int progress;                           /* a step's progress: 0 amount, 1 percent, 2 time left (GuiProgress) */
} SnxRunTheme;
#endif
