/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef SD_RENDER_H
#define SD_RENDER_H
#include <stddef.h>
#include <stdint.h>
#include "atlas.h"

typedef struct { float x, y, w, h; uint16_t u0, v0, u1, v1; uint32_t rgba; uint32_t pad; } RInst;

typedef struct Renderer Renderer;
typedef struct { int x, y, w, h; } RSoftRect;

Renderer *r_new(Atlas *a);
void r_free(Renderer *r);
Atlas *r_atlas(Renderer *r);
void r_begin(Renderer *r, int w, int h, uint32_t clear_rgba);
void r_end(Renderer *r);
void r_flush(Renderer *r);
void r_soft_bgr(Renderer *r, int on);   /* keep blue in the low byte, the layout of an X11 XRGB8888 surface */
int r_soft_is_bgr(const Renderer *r);
/* CPU path: the rectangles that changed in the last r_end (empty: nothing to present); r_soft_invalidate makes the next frame repaint and present everything */
const RSoftRect *r_soft_damage(const Renderer *r, int *n);
void r_soft_invalidate(Renderer *r);
void r_soft_target(Renderer *r, uint32_t *pixels);   /* draw straight into these w*h pixels (the window surface) instead of an own buffer; NULL goes back to the own buffer */
const uint32_t *r_soft_pixels(const Renderer *r, int *w, int *h);   /* the CPU rasterizer's frame (RGBA bytes), NULL on the GL path */
void r_clip(Renderer *r, int x, int y, int w, int h);
void r_clip_off(Renderer *r);
void r_rect(Renderer *r, float x, float y, float w, float h, uint32_t rgba);
void r_rrect(Renderer *r, float x, float y, float w, float h, uint32_t rgba, int radius, int border);
void r_push(Renderer *r, const RInst *inst, int n);
void r_push_at(Renderer *r, const RInst *inst, int n, float dx, float dy);
size_t r_mark(const Renderer *r);
void r_rewind(Renderer *r, size_t mark);
void r_make_rect(Renderer *r, RInst *o, float x, float y, float w, float h, uint32_t rgba);
void r_make_glyph(Renderer *r, RInst *o, const AtlasGlyph *g, float pen_x, float baseline_y, uint32_t rgba);
#define RGBA(r, g, b, a) ((uint32_t)(r) | (uint32_t)(g) << 8 | (uint32_t)(b) << 16 | (uint32_t)(a) << 24)
#endif
