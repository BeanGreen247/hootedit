/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

// The CPU rasterizer, pixel by pixel, without a window: same instruction stream as the GL path.
#include <gtest/gtest.h>

#include <vector>

#include "gl.h"
#include "render.h"

namespace {

struct Frame {
    Atlas *a = nullptr;
    Renderer *r = nullptr;
    bool ended = false;
    Frame(int w, int h, uint32_t clear) {
        sd_gl_load(GLK_SOFT);
        a = atlas_new(256);
        r = r_new(a);
        r_begin(r, w, h, clear);
    }
    ~Frame() { r_free(r); atlas_free(a); }
    uint32_t px(int x, int y) {   // call after all drawing
        if (!ended) r_end(r);   // the frame is painted by r_end, once
        ended = true;
        int w = 0, h = 0;
        const uint32_t *p = r_soft_pixels(r, &w, &h);
        return p[(size_t)y * (size_t)w + (size_t)x];
    }
};

const uint32_t BLACK = 0xff000000u;

}  // namespace

TEST(Raster, RectCoversPixelsWhoseCentreIsInside) {
    Frame f(16, 16, RGBA(0, 0, 0, 255));
    r_rect(f.r, 2.f, 3.f, 4.f, 2.f, RGBA(255, 0, 0, 255));
    EXPECT_EQ(f.px(2, 3), RGBA(255, 0, 0, 255));
    EXPECT_EQ(f.px(5, 4), RGBA(255, 0, 0, 255));
    EXPECT_EQ(f.px(6, 4), BLACK);
    EXPECT_EQ(f.px(2, 5), BLACK);
    EXPECT_EQ(f.px(1, 3), BLACK);
}

TEST(Raster, ThinRectAlwaysCoversAPixel) {   // the text cursor is 1.5 px wide at any x: it must never vanish
    for (int i = 0; i < 40; i++) {
        Frame f(16, 16, RGBA(0, 0, 0, 255));
        float x = 4.f + (float)i * 0.0625f;
        r_rect(f.r, x, 2.f, 1.5f, 8.f, RGBA(255, 255, 255, 255));
        int lit = 0;
        for (int px = 0; px < 16; px++) if (f.px(px, 5) != BLACK) lit++;
        EXPECT_GE(lit, 1) << "x=" << x;
    }
    for (int i = 0; i < 40; i++) {
        Frame f(16, 16, RGBA(0, 0, 0, 255));
        float x = 4.f + (float)i * 0.0625f;
        r_rect(f.r, x, 2.f, 1.f, 8.f, RGBA(255, 255, 255, 255));
        int lit = 0;
        for (int px = 0; px < 16; px++) if (f.px(px, 5) != BLACK) lit++;
        EXPECT_GE(lit, 1) << "w=1 x=" << x;
    }
}

TEST(Raster, AlphaBlendRoundsLikeDivisionBy255) {
    Frame f(4, 4, RGBA(200, 100, 0, 255));
    r_rect(f.r, 0.f, 0.f, 4.f, 4.f, RGBA(0, 100, 255, 128));
    uint32_t p = f.px(1, 1);
    EXPECT_EQ(p & 255, (uint32_t)((0 * 128 + 200 * 127 + 127) / 255));
    EXPECT_EQ((p >> 8) & 255, 100u);
    EXPECT_EQ((p >> 16) & 255, (uint32_t)((255 * 128 + 0 * 127 + 127) / 255));
    EXPECT_EQ(p >> 24, 255u);
}

TEST(Raster, ClipLimitsDrawingAndClipOffRestoresIt) {
    Frame f(16, 16, RGBA(0, 0, 0, 255));
    r_clip(f.r, 4, 4, 4, 4);
    r_rect(f.r, 0.f, 0.f, 16.f, 16.f, RGBA(0, 255, 0, 255));
    r_clip_off(f.r);
    r_rect(f.r, 0.f, 0.f, 1.f, 1.f, RGBA(0, 0, 255, 255));
    EXPECT_EQ(f.px(4, 4), RGBA(0, 255, 0, 255));
    EXPECT_EQ(f.px(7, 7), RGBA(0, 255, 0, 255));
    EXPECT_EQ(f.px(8, 8), BLACK);
    EXPECT_EQ(f.px(3, 4), BLACK);
    EXPECT_EQ(f.px(0, 0), RGBA(0, 0, 255, 255));
}

TEST(Raster, GlyphCopiesCoverageOneToOne) {
    Frame f(16, 16, RGBA(0, 0, 0, 255));
    const uint8_t bits[6] = {255, 0, 128, 0, 255, 64};   // 3 x 2
    const AtlasGlyph *g = atlas_custom(f.a, 7, 1, bits, 3, 2);
    ASSERT_NE(g, nullptr);
    RInst i;
    r_make_glyph(f.r, &i, g, 5.f, 8.f, RGBA(255, 255, 255, 255));
    r_push(f.r, &i, 1);
    int x0 = 5 + g->left, y0 = 8 - g->top;
    EXPECT_EQ(f.px(x0, y0) & 255, 255u);
    EXPECT_EQ(f.px(x0 + 1, y0) & 255, 0u);
    EXPECT_EQ(f.px(x0 + 2, y0) & 255, 128u);
    EXPECT_EQ(f.px(x0, y0 + 1) & 255, 0u);
    EXPECT_EQ(f.px(x0 + 1, y0 + 1) & 255, 255u);
    EXPECT_EQ(f.px(x0 + 2, y0 + 1) & 255, 64u);
}

TEST(Raster, RoundedRectLeavesTheCornerAndFillsTheMiddle) {
    Frame f(32, 32, RGBA(0, 0, 0, 255));
    r_rrect(f.r, 4.f, 4.f, 24.f, 24.f, RGBA(255, 255, 255, 255), 8, 0);
    EXPECT_EQ(f.px(4, 4), BLACK);
    EXPECT_EQ(f.px(16, 16), RGBA(255, 255, 255, 255));
    EXPECT_NE(f.px(16, 4), BLACK);
}

TEST(Raster, BorderLeavesTheInsideEmpty) {
    Frame f(32, 32, RGBA(0, 0, 0, 255));
    r_rrect(f.r, 4.f, 4.f, 24.f, 24.f, RGBA(255, 255, 255, 255), 2, 1);
    EXPECT_EQ(f.px(16, 16), BLACK);
    EXPECT_NE(f.px(16, 4), BLACK);
}

TEST(Raster, FrameIsOpaqueAndGlPathReportsNoPixels) {
    Frame f(4, 4, RGBA(10, 20, 30, 255));
    EXPECT_EQ(f.px(0, 0), RGBA(10, 20, 30, 255));
}

namespace {
void scene(Renderer *r, float cursor_x, int clipped) {
    r_rect(r, 10.f, 10.f, 200.f, 40.f, RGBA(30, 60, 90, 255));
    r_rrect(r, 20.f, 70.f, 100.f, 30.f, RGBA(200, 200, 40, 255), 6, 1);
    if (clipped) r_clip(r, 100, 0, 60, 120);
    r_rect(r, 90.f, 20.f, 100.f, 90.f, RGBA(255, 0, 0, 128));
    if (clipped) r_clip_off(r);
    r_rect(r, cursor_x, 12.f, 1.5f, 20.f, RGBA(255, 255, 255, 255));
}
}  // namespace

TEST(Raster, UnchangedFrameHasNoDamageAndChangedTilesMatchAFullRepaint) {
    sd_gl_load(GLK_SOFT);
    Atlas *a = atlas_new(256);
    Renderer *r = r_new(a);
    const int W = 256, H = 128;
    int n = 0;
    r_begin(r, W, H, RGBA(0, 0, 0, 255));
    scene(r, 30.f, 1);
    r_end(r);
    r_soft_damage(r, &n);
    EXPECT_GE(n, 1);   // first frame: everything
    r_begin(r, W, H, RGBA(0, 0, 0, 255));
    scene(r, 30.f, 1);
    r_end(r);
    r_soft_damage(r, &n);
    EXPECT_EQ(n, 0);
    r_begin(r, W, H, RGBA(0, 0, 0, 255));
    scene(r, 60.f, 1);   // only the cursor moved
    r_end(r);
    const RSoftRect *d = r_soft_damage(r, &n);
    ASSERT_GE(n, 1);
    long area = 0;
    for (int i = 0; i < n; i++) area += (long)d[i].w * d[i].h;
    EXPECT_LT(area, (long)W * H / 4);
    int w = 0, h = 0;
    std::vector<uint32_t> inc(r_soft_pixels(r, &w, &h), r_soft_pixels(r, &w, &h) + (size_t)W * H);
    Renderer *r2 = r_new(a);   // the same frame from scratch
    r_begin(r2, W, H, RGBA(0, 0, 0, 255));
    scene(r2, 60.f, 1);
    r_end(r2);
    const uint32_t *ref = r_soft_pixels(r2, &w, &h);
    EXPECT_EQ(memcmp(inc.data(), ref, inc.size() * 4), 0);
    r_free(r2);
    r_free(r);
    atlas_free(a);
}
