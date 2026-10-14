/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "icons.h"

#include <math.h>

namespace {

float seg_d(float x, float y, float x0, float y0, float x1, float y1) {
    float dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy;
    float t = l2 > 0 ? ((x - x0) * dx + (y - y0) * dy) / l2 : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float px = x0 + t * dx - x, py = y0 + t * dy - y;
    return sqrtf(px * px + py * py);
}
bool seg(float x, float y, float x0, float y0, float x1, float y1, float r) { return seg_d(x, y, x0, y0, x1, y1) <= r; }
bool circ(float x, float y, float cx, float cy, float r) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; }
bool ring(float x, float y, float cx, float cy, float r, float t) { float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)); return fabsf(d - r) <= t / 2; }
bool box(float x, float y, float x0, float y0, float x1, float y1) { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
bool box_out(float x, float y, float x0, float y0, float x1, float y1, float t) { return box(x, y, x0, y0, x1, y1) && !box(x, y, x0 + t, y0 + t, x1 - t, y1 - t); }

bool inside(Icon ic, float x, float y) {
    switch (ic) {
    case IC_EXPLORER:   /* two sheets */
        return box_out(x, y, 2, 4.5f, 10, 14.5f, 1.2f) || (box_out(x, y, 6, 1.5f, 14, 11.5f, 1.2f) && !box(x, y, 6, 4.5f, 10, 11.5f));
    case IC_SEARCH:
        return ring(x, y, 6.6f, 6.6f, 4.3f, 1.5f) || seg(x, y, 9.9f, 9.9f, 14.3f, 14.3f, 0.85f);
    case IC_GIT:
        return circ(x, y, 4.2f, 3.2f, 1.9f) || circ(x, y, 4.2f, 12.8f, 1.9f) || ring(x, y, 11.8f, 5.6f, 1.9f, 1.3f) || seg(x, y, 4.2f, 4.5f, 4.2f, 11.5f, 0.65f) ||
               seg(x, y, 11.8f, 7.4f, 11.8f, 8.2f, 0.65f) || seg(x, y, 11.8f, 8.2f, 4.4f, 11.2f, 0.65f);
    case IC_PIN: case IC_PIN_OUT: {   /* push-pin drawn upright, then rotated 45 degrees so the needle points down-left */
        const float c = 0.70710678f;
        float dx = x - 8, dy = y - 8;
        float u = 8 + (dx * c + dy * c), v = 8 + (-dx * c + dy * c);
        bool filled = ic == IC_PIN;
        bool body = box(u, v, 5.2f, 1.6f, 10.8f, 3.4f) || box(u, v, 6.4f, 3.4f, 9.6f, 8.4f) || box(u, v, 3.6f, 8.4f, 12.4f, 10.0f);
        if (filled) return body || seg(u, v, 8, 10.0f, 8, 15.2f, 0.75f);
        bool inner = box(u, v, 6.4f, 2.6f, 9.6f, 3.4f) || box(u, v, 7.4f, 3.4f, 8.6f, 8.4f) || box(u, v, 5.0f, 9.0f, 11.0f, 9.4f);
        return (body && !inner && !box(u, v, 6.9f, 3.4f, 9.1f, 8.4f)) || seg(u, v, 8, 10.0f, 8, 15.2f, 0.75f);
    }
    case IC_CHEV_R: return seg(x, y, 6, 3.5f, 10.5f, 8, 0.85f) || seg(x, y, 10.5f, 8, 6, 12.5f, 0.85f);
    case IC_CHEV_D: return seg(x, y, 3.5f, 6, 8, 10.5f, 0.85f) || seg(x, y, 8, 10.5f, 12.5f, 6, 0.85f);
    case IC_CLOSE:  return seg(x, y, 4, 4, 12, 12, 0.8f) || seg(x, y, 12, 4, 4, 12, 0.8f);
    case IC_DOT:    return circ(x, y, 8, 8, 3.4f);
    case IC_FILE:
        return seg(x, y, 3.5f, 1.5f, 9, 1.5f, 0.7f) || seg(x, y, 9, 1.5f, 12.5f, 5, 0.7f) || seg(x, y, 12.5f, 5, 12.5f, 14.5f, 0.7f) || seg(x, y, 12.5f, 14.5f, 3.5f, 14.5f, 0.7f) ||
               seg(x, y, 3.5f, 14.5f, 3.5f, 1.5f, 0.7f) || seg(x, y, 9, 1.5f, 9, 5, 0.7f) || seg(x, y, 9, 5, 12.5f, 5, 0.7f);
    case IC_FOLDER: return box_out(x, y, 1.5f, 5.6f, 14.5f, 13.6f, 1.2f) || box_out(x, y, 1.5f, 3.2f, 6.6f, 5.8f, 1.2f);
    case IC_ARROW_L: return seg(x, y, 3, 8, 13, 8, 0.75f) || seg(x, y, 3, 8, 7.5f, 3.5f, 0.75f) || seg(x, y, 3, 8, 7.5f, 12.5f, 0.75f);
    case IC_ARROW_R: return seg(x, y, 3, 8, 13, 8, 0.75f) || seg(x, y, 13, 8, 8.5f, 3.5f, 0.75f) || seg(x, y, 13, 8, 8.5f, 12.5f, 0.75f);
    default: return false;
    }
}

}  // namespace

std::vector<uint8_t> icon_bitmap(Icon ic, int size) {
    std::vector<uint8_t> out((size_t)size * (size_t)size);
    const int N = 4;
    float k = 16.f / (float)size;
    for (int py = 0; py < size; py++)
        for (int px = 0; px < size; px++) {
            int n = 0;
            for (int sy = 0; sy < N; sy++)
                for (int sx = 0; sx < N; sx++)
                    if (inside(ic, ((float)px + ((float)sx + .5f) / N) * k, ((float)py + ((float)sy + .5f) / N) * k)) n++;
            out[(size_t)py * (size_t)size + (size_t)px] = (uint8_t)(n * 255 / (N * N));
        }
    return out;
}

const AtlasGlyph *icon_glyph(Atlas *a, Icon ic, int size) {
    const uint32_t FONT_ID = 0xfffe0001u;                      /* never equals a real font id (those count up from 1) */
    uint32_t key = (uint32_t)ic * 4096u + (uint32_t)size;
    const AtlasGlyph *g = atlas_find_custom(a, FONT_ID, key);
    if (g) return g;
    std::vector<uint8_t> bits = icon_bitmap(ic, size);
    return atlas_custom(a, FONT_ID, key, bits.data(), size, size);
}
