/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What a gamepad's controls send (src/common/utilities/pad_map.c): by position, by the pad's labels, or the player's
   own map as gamepad_map writes it, read back word by word, a bad word naming itself and leaving the map */
#include "common/utilities/pad_map.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { A, B, X, Y, L3, R3, L, R, ZL, ZR, PLUS, MINUS, LEFT, UP, RIGHT, DOWN };

int main(void) {
    static const int xbox[4] = {SNX_PAD_LABEL_A, SNX_PAD_LABEL_B, SNX_PAD_LABEL_X, SNX_PAD_LABEL_Y};
    static const int sony[4] = {SNX_PAD_LABEL_CROSS, SNX_PAD_LABEL_CIRCLE, SNX_PAD_LABEL_SQUARE,
                                SNX_PAD_LABEL_TRIANGLE};
    static const int console[4] = {SNX_PAD_LABEL_B, SNX_PAD_LABEL_A, SNX_PAD_LABEL_Y, SNX_PAD_LABEL_X};
    SnxPadMap map, own, labeled;
    char text[SNX_PAD_MAP_TEXT], error[64];
    int c;

    /* by position, as the console's controller has them: A right, B bottom, X top, Y left; the rest by their place */
    snx_pad_map_position(&map);
    assert(map.bit[SNX_PAD_SOUTH] == B && map.bit[SNX_PAD_EAST] == A && map.bit[SNX_PAD_NORTH] == X &&
           map.bit[SNX_PAD_WEST] == Y);
    assert(map.bit[SNX_PAD_LT] == ZL && map.bit[SNX_PAD_RB] == R && map.bit[SNX_PAD_START] == PLUS &&
           map.bit[SNX_PAD_BACK] == MINUS && map.bit[SNX_PAD_UP] == UP && map.bit[SNX_PAD_LS] == L3);
    /* by labels: an Xbox pad's A (at the bottom) sends A; a PlayStation pad's symbols as the letters at their places;
       a console layout's as on the console; no labels (NULL) as an Xbox pad's */
    snx_pad_map_labels(&map, xbox);
    assert(map.bit[SNX_PAD_SOUTH] == A && map.bit[SNX_PAD_EAST] == B && map.bit[SNX_PAD_WEST] == X &&
           map.bit[SNX_PAD_NORTH] == Y && map.bit[SNX_PAD_LT] == ZL);
    snx_pad_map_labels(&labeled, NULL);
    assert(!memcmp(&labeled, &map, sizeof map));
    {
        const int unknown[4] = {SNX_PAD_LABEL_NONE, SNX_PAD_LABEL_NONE, SNX_PAD_LABEL_NONE, SNX_PAD_LABEL_NONE};
        snx_pad_map_labels(&labeled, unknown);
        assert(!memcmp(&labeled, &map, sizeof map));
    }
    snx_pad_map_labels(&map, sony);
    assert(map.bit[SNX_PAD_SOUTH] == A && map.bit[SNX_PAD_EAST] == B && map.bit[SNX_PAD_WEST] == X &&
           map.bit[SNX_PAD_NORTH] == Y);
    snx_pad_map_labels(&map, console);
    assert(map.bit[SNX_PAD_SOUTH] == B && map.bit[SNX_PAD_EAST] == A && map.bit[SNX_PAD_WEST] == Y &&
           map.bit[SNX_PAD_NORTH] == X);

    /* the player's own: what it names, any case, and None; what it leaves out goes as the labels say */
    snx_pad_map_unset(&own);
    assert(!snx_pad_map_read(&own, "south:B  east:a\tLT:R lb:none", error, sizeof error));
    assert(own.bit[SNX_PAD_SOUTH] == B && own.bit[SNX_PAD_EAST] == A && own.bit[SNX_PAD_LT] == R &&
           own.bit[SNX_PAD_LB] == SNX_PAD_NONE && own.bit[SNX_PAD_WEST] == SNX_PAD_UNSET);
    snx_pad_map_labels(&labeled, xbox);
    map = own;
    snx_pad_map_fill(&map, &labeled);
    assert(map.bit[SNX_PAD_SOUTH] == B && map.bit[SNX_PAD_WEST] == X && map.bit[SNX_PAD_RT] == ZR);
    assert(snx_pad_map_find(&map, B) == SNX_PAD_SOUTH && snx_pad_map_find(&map, L) == -1);
    /* written back as it reads: the set ones, in the controls' order */
    snx_pad_map_write(&own, text, sizeof text);
    assert(!strcmp(text, "south:B east:A lb:None lt:R"));
    snx_pad_map_unset(&map);
    assert(!snx_pad_map_read(&map, text, error, sizeof error) && !memcmp(&map, &own, sizeof map));
    /* every control set fits its room */
    snx_pad_map_position(&map);
    snx_pad_map_write(&map, text, sizeof text);
    assert(strlen(text) < SNX_PAD_MAP_TEXT && !strncmp(text, "south:B east:A west:Y north:X", 29));
    /* a word it cannot read is named, and the map stays as it was: a button no console has, a control no pad has, no
       colon, a control named twice */
    map = own;
    assert(snx_pad_map_read(&map, "east:B south:Q", error, sizeof error) && !strcmp(error, "south:Q"));
    assert(snx_pad_map_read(&map, "paddle:A", error, sizeof error) && !strcmp(error, "paddle:A"));
    assert(snx_pad_map_read(&map, "south", error, sizeof error) && !strcmp(error, "south"));
    assert(snx_pad_map_read(&map, "south:A south:B", error, sizeof error) && !strcmp(error, "south:B"));
    assert(!memcmp(&map, &own, sizeof map));
    assert(!snx_pad_map_read(&map, "", error, sizeof error) && !memcmp(&map, &own, sizeof map));

    /* names: a face button as its pad shows it, the D-pad's in the font's arrows; the console's buttons; None */
    assert(!strcmp(snx_pad_control_name(SNX_PAD_SOUTH, sony), "Cross") &&
           !strcmp(snx_pad_control_name(SNX_PAD_SOUTH, NULL), "A"));
    assert(!strcmp(snx_pad_control_name(SNX_PAD_UP, xbox), "\x18") &&
           !strcmp(snx_pad_control_name(SNX_PAD_LT, sony), "LT"));
    assert(!strcmp(snx_pad_button_name(ZL), "ZL") && !strcmp(snx_pad_button_name(SNX_PAD_NONE), "None"));
    for (c = 0; c < SNX_PAD_CONTROLS; ++c) assert(snx_pad_control_key(c)[0]);
    assert(!snx_pad_control_key(SNX_PAD_CONTROLS)[0] && !snx_pad_control_name(-1, NULL)[0]);
    puts("pad_map passed");
    return 0;
}
