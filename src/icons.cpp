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

bool in_poly(float x, float y, const float (*p)[2], int n);

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
    case IC_CHECK: return seg(x, y, 3, 8.5f, 6.5f, 12, 0.95f) || seg(x, y, 6.5f, 12, 13, 4.5f, 0.95f);
    case IC_PLUS: return seg(x, y, 3, 8, 13, 8, 0.8f) || seg(x, y, 8, 3, 8, 13, 0.8f);
    case IC_MINUS: return seg(x, y, 3, 8, 13, 8, 0.8f);
    case IC_REFRESH: {   /* an open ring with an arrow head at the top right */
        float dx = x - 8, dy = y - 8, a = atan2f(dy, dx);
        bool gap = a > -1.35f && a < -0.25f;
        return (ring(x, y, 8, 8, 4.6f, 1.5f) && !gap) || seg(x, y, 12.9f, 2.4f, 12.9f, 6.2f, 0.8f) || seg(x, y, 12.9f, 6.2f, 9.2f, 6.2f, 0.8f);
    }
    case IC_ARROW_U: return seg(x, y, 8, 13, 8, 3, 0.75f) || seg(x, y, 8, 3, 3.8f, 7.2f, 0.75f) || seg(x, y, 8, 3, 12.2f, 7.2f, 0.75f);
    case IC_ARROW_D: return seg(x, y, 8, 3, 8, 13, 0.75f) || seg(x, y, 8, 13, 3.8f, 8.8f, 0.75f) || seg(x, y, 8, 13, 12.2f, 8.8f, 0.75f);
    case IC_TRI_R: { const float t[3][2] = {{3, 2}, {13, 8}, {3, 14}}; return in_poly(x, y, t, 3); }
    case IC_OPEN_FILE:   /* a sheet with a small arrow leaving it */
        return seg(x, y, 3.5f, 1.5f, 8.5f, 1.5f, 0.7f) || seg(x, y, 3.5f, 1.5f, 3.5f, 14.5f, 0.7f) || seg(x, y, 3.5f, 14.5f, 12.5f, 14.5f, 0.7f) || seg(x, y, 12.5f, 14.5f, 12.5f, 10, 0.7f) ||
               seg(x, y, 7, 9, 13.5f, 2.5f, 0.75f) || seg(x, y, 13.5f, 2.5f, 9.5f, 2.5f, 0.75f) || seg(x, y, 13.5f, 2.5f, 13.5f, 6.5f, 0.75f);
    case IC_SIDEBAR:   /* a window frame with its left panel filled */
        return seg(x, y, 2, 3, 14, 3, 0.7f) || seg(x, y, 2, 13, 14, 13, 0.7f) || seg(x, y, 2, 3, 2, 13, 0.7f) || seg(x, y, 14, 3, 14, 13, 0.7f) || (x > 2 && x < 6.2f && y > 3 && y < 13);
    case IC_GEAR: {   /* a ring with eight teeth: settings */
        float dx = x - 8, dy = y - 8, r = sqrtf(dx * dx + dy * dy), a = atan2f(dy, dx);
        if (ring(x, y, 8, 8, 4.4f, 1.7f)) return true;
        float t = fabsf(sinf(a * 4.f));   /* eight lobes */
        return r > 5.2f && r < 7.6f && t > 0.55f;
    }
    case IC_UNDO: {   /* an arc swinging back to an arrow head at the left: discard changes */
        float dx = x - 8.5f, dy = y - 9.5f, a = atan2f(dy, dx);
        bool gap = a > 1.5f && a < 2.9f;
        return (ring(x, y, 8.5f, 9.5f, 4.4f, 1.5f) && !gap && y < 13.6f) || seg(x, y, 3.6f, 9.6f, 3.6f, 5.0f, 0.8f) || seg(x, y, 3.6f, 9.6f, 8.2f, 9.6f, 0.8f);
    }
    default: return false;
    }
}

/* ---- the logo, in the 256-unit space of the app icon: the owl head scaled by 0.84 and shifted down 6, between a < and a > ---- */

bool in_poly(float x, float y, const float (*p)[2], int n) {
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++)
        if (((p[i][1] > y) != (p[j][1] > y)) && (x < (p[j][0] - p[i][0]) * (y - p[i][1]) / (p[j][1] - p[i][1]) + p[i][0])) in = !in;
    return in;
}

const float HEAD[10][2] = {{56, 36}, {106, 66}, {128, 60}, {150, 66}, {200, 36}, {210, 118}, {190, 188}, {128, 226}, {66, 188}, {46, 118}};
const float BEAK[4][2] = {{128, 152}, {115, 128}, {128, 120}, {141, 128}};

bool logo_inside(Icon ic, float x, float y) {
    const float K = 0.84f;
    float dx = 128 + (x - 128) / K, dy = 128 + (y - 134) / K;   /* back into the unscaled head's coordinates */
    bool eye_disc = circ(dx, dy, 97, 108, 31) || circ(dx, dy, 159, 108, 31);
    if (ic == IC_LOGO_BODY) {
        if (in_poly(dx, dy, HEAD, 10) && !eye_disc) return true;
        for (int s = -1; s <= 1; s += 2) {
            float arm = 128 + (float)s * 80, tip = 128 + (float)s * 108;
            if (seg_d(x, y, arm, 86, tip, 128) <= 7.5f || seg_d(x, y, tip, 128, arm, 170) <= 7.5f) return true;
        }
        return false;
    }
    /* eyes: a ring (iris) around a small pupil, and the beak */
    for (float cx : {97.f, 159.f}) if (circ(dx, dy, cx, 108, 20) && !circ(dx, dy, cx, 108, 9)) return true;
    return in_poly(dx, dy, BEAK, 4);
}

}  // namespace

static std::vector<uint8_t> logo_bitmap(Icon ic, int size) {
    std::vector<uint8_t> out((size_t)size * (size_t)size);
    const int N = 2;   /* 4 samples a pixel: the logo is a faint watermark and this keeps its one-time cost to a few ms */
    float k = 256.f / (float)size;
    for (int py = 0; py < size; py++)
        for (int px = 0; px < size; px++) {
            int n = 0;
            for (int sy = 0; sy < N; sy++)
                for (int sx = 0; sx < N; sx++)
                    if (logo_inside(ic, ((float)px + ((float)sx + .5f) / N) * k, ((float)py + ((float)sy + .5f) / N) * k)) n++;
            out[(size_t)py * (size_t)size + (size_t)px] = (uint8_t)(n * 255 / (N * N));
        }
    return out;
}

static std::vector<uint8_t> hatch_bitmap(int size) {   /* diagonal stripes that tile seamlessly: the period divides the tile */
    int period = size;
    for (int p = 10; p >= 4; p--) if (size % p == 0) { period = p; break; }
    std::vector<uint8_t> out((size_t)size * (size_t)size, 0);
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) out[(size_t)y * (size_t)size + (size_t)x] = (x + y) % period < std::max(1, period / 4) ? 255 : 0;
    return out;
}

std::vector<uint8_t> icon_bitmap(Icon ic, int size) {
    if (ic == IC_HATCH) return hatch_bitmap(size);
    if (ic == IC_LOGO_BODY || ic == IC_LOGO_EYES) return logo_bitmap(ic, size);
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
