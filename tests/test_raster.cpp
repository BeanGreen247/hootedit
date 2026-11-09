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
    std::vector<uint32_t> buf;   // the frame, RGBA bytes: red in the low byte
    int bw = 0, bh = 0;
    Frame(int w, int h, uint32_t clear) : buf((size_t)w * (size_t)h, 0u), bw(w), bh(h) {
        sd_soft = true;
        a = atlas_new(256);
        r = r_new(a);
        r_set_target(r, buf.data(), w, h, w, 0, 8, 16, 0xff000000u);
        r_begin(r, w, h, clear);
    }
    ~Frame() { r_free(r); atlas_free(a); }
    uint32_t px(int x, int y) {   // call after all drawing
        if (!ended) r_end(r);   // the frame is painted by r_end, once
        ended = true;
        return buf[(size_t)y * (size_t)bw + (size_t)x];
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
    sd_soft = true;
    Atlas *a = atlas_new(256);
    Renderer *r = r_new(a);
    const int W = 256, H = 128;
    std::vector<uint32_t> fb((size_t)W * H, 0u), fb2((size_t)W * H, 0u);
    r_set_target(r, fb.data(), W, H, W, 0, 8, 16, 0xff000000u);
    size_t n = 0;
    r_begin(r, W, H, RGBA(0, 0, 0, 255));
    scene(r, 30.f, 1);
    r_end(r);
    r_damage(r, &n);
    EXPECT_GE(n, 1);   // first frame: everything
    r_begin(r, W, H, RGBA(0, 0, 0, 255));
    scene(r, 30.f, 1);
    r_end(r);
    r_damage(r, &n);
    EXPECT_EQ(n, 0);
    r_begin(r, W, H, RGBA(0, 0, 0, 255));
    scene(r, 60.f, 1);   // only the cursor moved
    r_end(r);
    const RDamage *d = r_damage(r, &n);
    ASSERT_GE(n, 1);
    long area = 0;
    for (size_t i = 0; i < n; i++) area += (long)d[i].w * d[i].h;
    EXPECT_LT(area, (long)W * H / 4);
    std::vector<uint32_t> inc(fb);
    Renderer *r2 = r_new(a);   // the same frame from scratch
    r_set_target(r2, fb2.data(), W, H, W, 0, 8, 16, 0xff000000u);
    r_begin(r2, W, H, RGBA(0, 0, 0, 255));
    scene(r2, 60.f, 1);
    r_end(r2);
    EXPECT_EQ(memcmp(inc.data(), fb2.data(), inc.size() * 4), 0);
    r_free(r2);
    r_free(r);
    atlas_free(a);
}

// Random scenes over many frames, window sizes that change, one pixel buffer reused at the same address: every incrementally painted frame
// must equal the same frame painted from scratch. This is the damage tracking (tile hashes, run merging, clips) under a stress test.
namespace {
struct Rng {
    uint64_t s;
    uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 11); }
    int range(int n) { return (int)(next() % (uint32_t)n); }
    float f(float lo, float hi) { return lo + (hi - lo) * (float)(next() % 10000) / 10000.f; }
};

struct Item { int kind; float x, y, w, h; uint32_t color; int radius, border; int clip; int cx, cy, cw, ch; };

void paint(Renderer *r, const std::vector<Item> &items, const AtlasGlyph *g) {
    for (const Item &it : items) {
        if (it.clip) r_clip(r, it.cx, it.cy, it.cw, it.ch); else r_clip_off(r);
        if (it.kind == 0) r_rect(r, it.x, it.y, it.w, it.h, it.color);
        else if (it.kind == 1) r_rrect(r, it.x, it.y, it.w, it.h, it.color, it.radius, it.border);
        else if (g) { RInst q; r_make_glyph(r, &q, g, it.x, it.y, it.color); r_push(r, &q, 1); }
    }
    r_clip_off(r);
}
}  // namespace

TEST(Raster, RandomScenesIncrementalEqualsFromScratch) {
    sd_soft = true;
    r_soft_parallel_min_area(1);   // every multi-run repaint goes through the worker threads
    struct Restore { ~Restore() { r_soft_parallel_min_area(120000); } } restore;
    Atlas *a = atlas_new(256);
    static const uint8_t bits[12] = {0, 255, 128, 0, 255, 255, 64, 255, 0, 200, 255, 90};
    const AtlasGlyph *g = atlas_custom(a, 11, 1, bits, 4, 3);
    Rng rng{0x9E3779B97F4A7C15ull};
    Renderer *r = r_new(a);
    std::vector<uint32_t> buf((size_t)400 * 300), ref((size_t)400 * 300);   // one allocation: every size reuses the same address
    std::vector<Item> items;
    int W = 200, H = 150;
    for (int frame = 0; frame < 300; frame++) {
        if (frame % 7 == 0) { W = 40 + rng.range(300); H = 40 + rng.range(200); }
        if (frame % 5 == 0 || items.empty()) {   // a new scene
            items.clear();
            const int n = 5 + rng.range(60);
            for (int i = 0; i < n; i++) {
                Item it{};
                it.kind = rng.range(3);
                it.x = rng.f(-20.f, (float)W); it.y = rng.f(-20.f, (float)H); it.w = rng.f(1.f, 120.f); it.h = rng.f(1.f, 60.f);
                it.color = RGBA(rng.range(256), rng.range(256), rng.range(256), rng.range(4) == 0 ? 80 + rng.range(150) : 255);
                it.radius = rng.range(12); it.border = rng.range(3) == 0 ? 1 + rng.range(2) : 0;
                if ((it.clip = rng.range(4) == 0)) { it.cx = rng.range(W); it.cy = rng.range(H); it.cw = 1 + rng.range(150); it.ch = 1 + rng.range(100); }
                items.push_back(it);
            }
        } else {   // small edits: move or recolour a few items, as typing and scrolling do
            for (int k = 0; k < 1 + rng.range(4); k++) {
                Item &it = items[(size_t)rng.range((int)items.size())];
                if (rng.range(2)) { it.x += rng.f(-6.f, 6.f); it.y += rng.f(-6.f, 6.f); } else it.color = RGBA(rng.range(256), rng.range(256), rng.range(256), 255);
            }
        }
        const uint32_t bg = RGBA(20, 20, 20, 255);
        r_set_target(r, buf.data(), W, H, W, 0, 8, 16, 0xff000000u);
        r_begin(r, W, H, bg);
        paint(r, items, g);
        r_end(r);
        Renderer *r2 = r_new(a);
        r_set_target(r2, ref.data(), W, H, W, 0, 8, 16, 0xff000000u);
        r_begin(r2, W, H, bg);
        paint(r2, items, g);
        r_end(r2);
        r_free(r2);
        ASSERT_EQ(memcmp(buf.data(), ref.data(), (size_t)W * H * 4), 0) << "frame " << frame << " size " << W << "x" << H;
    }
    r_free(r);
    atlas_free(a);
}
