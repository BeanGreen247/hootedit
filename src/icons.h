/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef HE_ICONS_H
#define HE_ICONS_H
#include <stdint.h>
#include <vector>

#include "engine/atlas.h"

enum Icon { IC_EXPLORER, IC_SEARCH, IC_GIT, IC_PIN, IC_PIN_OUT, IC_CHEV_R, IC_CHEV_D, IC_CLOSE, IC_DOT, IC_FILE, IC_ARROW_L, IC_ARROW_R, IC_FOLDER, IC_LOGO_BODY, IC_LOGO_EYES, IC_CHECK, IC_PLUS, IC_MINUS, IC_REFRESH, IC_ARROW_U, IC_ARROW_D, IC_TRI_R, IC_HATCH, IC_OPEN_FILE, IC_UNDO, IC_GEAR, IC_SIDEBAR, IC_COUNT };

/* Alpha bitmap of icon `ic` at size x size pixels, drawn from shapes on a 16x16 grid with 4x4 supersampling.
 * No icon font and no image files: the same code serves every display scale. */
std::vector<uint8_t> icon_bitmap(Icon ic, int size);

/* IC_LOGO_BODY and IC_LOGO_EYES together are the Hootedit owl (the face inside a < > tag) as two single-colour layers for the start screen:
 * the head and brackets, and the eyes and beak. Drawn from the same shapes as the app icon, at any size, with no image file. */

/* Cached in the glyph atlas; the returned glyph is placed with r_make_glyph(x, y) at its top-left corner. */
const AtlasGlyph *icon_glyph(Atlas *a, Icon ic, int size);
#endif
