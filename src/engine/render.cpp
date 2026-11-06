/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "render.h"

#include <math.h>
#include <unistd.h>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl.h"

typedef struct { float x, y, u, v; uint32_t rgba; float lx, ly, sw, sh, ar, ab; } V2;
struct SClip { int x, y, w, h; };
#define TILE 32
struct SoftState {
    std::vector<RInst> insts;          /* the whole frame, in draw order */
    std::vector<int> inst_clip;        /* per instance: index into clips, -1 = none */
    std::vector<SClip> clips;
    int cur_clip = -1;
    std::vector<int> bbox;             /* per instance x0,y0,x1,y1 in pixels, clipped; empty when it draws nothing */
    std::vector<uint32_t> bin_start, bin_item;
    std::vector<uint64_t> hash, prev_hash;
    std::vector<uint8_t> dirty;
    std::vector<uint32_t> ref;         /* verifier: the same frame painted from scratch */
    int tw = 0, th = 0, pw = 0, ph = 0;   /* tile grid, size of the frame the hashes belong to */
    uint32_t clear_px = 0, gen = ~0u;
    int force_full = 1, verify = 0;
    std::vector<RSoftRect> rects;
};
#define MAXQ 16384

struct Renderer {
    Atlas *atlas;
    GLuint prog, vao, vbo, ibo;
    V2 *v2;
    GLint u_res, u_atlas;
    RInst *buf;
    size_t n, cap;
    size_t vbo_cap;
    int w, h;
    int clip_on, cx, cy, cw, ch;
    struct SoftState *ss;   /* CPU path: deferred frame, tile hashes, damage */
    int swap_rb;        /* soft: the target keeps blue in the low byte (X11 XRGB8888): colours are swapped on the way in so the frame can be copied as it is */
    int soft;           /* 1: the CPU rasterizer draws into fb, no GL object exists */
    uint32_t *ext_fb;   /* soft: caller-owned pixels used as fb for this frame, or NULL */
    uint32_t *fb;       /* w*h pixels, RGBA byte order (R in the low byte), alpha stays 255 */
    size_t fb_cap;
};

#define SDF_BODY \
    "  float a = TEX(u_atlas, v_uv).r;\n" \
    "  if (v_aux.x + v_aux.y > 0.0) {\n" \
    "    vec2 half_size = 0.5 * v_size;\n" \
    "    float rad = min(v_aux.x, min(half_size.x, half_size.y));\n" \
    "    vec2 q = abs(v_local - half_size) - (half_size - vec2(rad));\n" \
    "    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - rad;\n" \
    "    float cov = clamp(0.5 - d, 0.0, 1.0);\n" \
    "    if (v_aux.y > 0.0) cov = max(cov - clamp(0.5 - (d + v_aux.y), 0.0, 1.0), 0.0);\n" \
    "    a *= cov;\n" \
    "  }\n"

static const char *VS3 =
    "layout(location=0) in vec4 a_rect;\n"
    "layout(location=1) in vec4 a_uv;\n"
    "layout(location=2) in vec4 a_col;\n"
    "layout(location=3) in vec4 a_aux;\n"
    "uniform vec2 u_res;\n"
    "out vec2 v_uv; out vec4 v_col; out vec2 v_local; out vec2 v_size; out vec2 v_aux;\n"
    "void main() {\n"
    "  vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
    "  vec2 p = a_rect.xy + c * a_rect.zw;\n"
    "  gl_Position = vec4(p.x / u_res.x * 2.0 - 1.0, 1.0 - p.y / u_res.y * 2.0, 0.0, 1.0);\n"
    "  v_uv = mix(a_uv.xy, a_uv.zw, c);\n"
    "  v_col = a_col;\n"
    "  v_local = c * a_rect.zw; v_size = a_rect.zw; v_aux = a_aux.xy * 255.0;\n"
    "}\n";

static const char *FS3 =
    "in vec2 v_uv; in vec4 v_col; in vec2 v_local; in vec2 v_size; in vec2 v_aux;\n"
    "uniform sampler2D u_atlas;\n"
    "out vec4 o;\n"
    "#define TEX texture\n"
    "void main() {\n"
    SDF_BODY
    "  o = vec4(v_col.rgb, v_col.a * a);\n"
    "}\n";

static const char *VS2 =
    "#version 100\n"
    "attribute vec2 a_pos; attribute vec2 a_uv; attribute vec4 a_col; attribute vec2 a_local; attribute vec2 a_size; attribute vec2 a_aux;\n"
    "uniform vec2 u_res;\n"
    "varying vec2 v_uv; varying vec4 v_col; varying vec2 v_local; varying vec2 v_size; varying vec2 v_aux;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_pos.x / u_res.x * 2.0 - 1.0, 1.0 - a_pos.y / u_res.y * 2.0, 0.0, 1.0);\n"
    "  v_uv = a_uv; v_col = a_col; v_local = a_local; v_size = a_size; v_aux = a_aux;\n"
    "}\n";

static const char *FS2 =
    "#version 100\n"
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n"
    "varying vec2 v_uv; varying vec4 v_col; varying vec2 v_local; varying vec2 v_size; varying vec2 v_aux;\n"
    "uniform sampler2D u_atlas;\n"
    "#define TEX texture2D\n"
    "void main() {\n"
    SDF_BODY
    "  gl_FragColor = vec4(v_col.rgb, v_col.a * a);\n"
    "}\n";


/* ---- CPU rasterizer: the same instruction stream as the GL path, drawn into a pixel buffer ----
   Pixel rule: a pixel belongs to a quad when its centre is inside (ceil(x - 0.5)), like the GPU. Blend: integer
   t = s*a + d*(255-a) + 128, (t + (t >> 8)) >> 8, which is exact division by 255 with rounding. */

static inline uint32_t blend_px(uint32_t d, uint32_t src, uint32_t a) {   /* src RGB in the low 24 bits, a in 0..255; two 16-bit lanes per multiply */
    uint32_t ia = 255 - a;
    uint32_t rb = (src & 0x00ff00ffu) * a + (d & 0x00ff00ffu) * ia + 0x00800080u;
    uint32_t g = (src & 0x0000ff00u) * a + (d & 0x0000ff00u) * ia + 0x00008000u;
    rb = ((rb + ((rb >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
    g = ((g + ((g >> 8) & 0x0000ff00u)) >> 8) & 0x0000ff00u;
    return rb | g | 0xff000000u;
}

static inline int ceil_i(float v) { int i = (int)v; return (float)i < v ? i + 1 : i; }

/* draws one instance inside [cx0,cx1) x [cy0,cy1), which the caller has already cut down to the frame and the instance's clip */
static void soft_inst(Renderer *r, const RInst *s, int cx0, int cy0, int cx1, int cy1) {
    int x0 = ceil_i(s->x - 0.5f), x1 = ceil_i(s->x + s->w - 0.5f), y0 = ceil_i(s->y - 0.5f), y1 = ceil_i(s->y + s->h - 0.5f);
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    if (x0 >= x1 || y0 >= y1) return;
    uint32_t ca = s->rgba >> 24, src = s->rgba & 0xffffffu;
    if (!ca) return;
    if (r->swap_rb) src = ((src & 255) << 16) | (src & 0xff00u) | (src >> 16);
    const uint8_t *pix = atlas_pixels(r->atlas);
    const int asz = atlas_size(r->atlas);
    const float tscale = (float)asz / 65535.f;
    const int sdf = (s->pad & 0xffff) != 0;
    const int solid = s->u0 == s->u1 && s->v0 == s->v1;
    if (solid && !sdf) {   /* a flat rectangle: every texel it could sample is the white 2x2 block */
        if (ca == 255) {
            uint32_t px = src | 0xff000000u;
            for (int y = y0; y < y1; y++) {
                uint32_t *row = r->fb + (size_t)y * (size_t)r->w;
                for (int x = x0; x < x1; x++) row[x] = px;
            }
        } else {
            for (int y = y0; y < y1; y++) {
                uint32_t *row = r->fb + (size_t)y * (size_t)r->w;
                for (int x = x0; x < x1; x++) row[x] = blend_px(row[x], src, ca);
            }
        }
        return;
    }
    /* texel position of pixel centre (px+0.5, py+0.5): the quad maps linearly onto [u0,u1] x [v0,v1] */
    const float fu0 = (float)s->u0 * tscale, fv0 = (float)s->v0 * tscale, fu1 = (float)s->u1 * tscale, fv1 = (float)s->v1 * tscale;
    const float du = s->w > 0 ? (fu1 - fu0) / s->w : 0.f, dv = s->h > 0 ? (fv1 - fv0) / s->h : 0.f;
    if (!sdf && du == 1.f && dv == 1.f) {   /* a glyph copied 1:1: with integer pixels the texel is x + k, no per-pixel float */
        int kx = (int)floorf(fu0 - s->x + 0.5f), ky = (int)floorf(fv0 - s->y + 0.5f);
        if (x0 + kx < 0) x0 = -kx;
        if (x1 + kx > asz) x1 = asz - kx;
        if (y0 + ky < 0) y0 = -ky;
        if (y1 + ky > asz) y1 = asz - ky;
        for (int y = y0; y < y1; y++) {
            uint32_t *row = r->fb + (size_t)y * (size_t)r->w;
            const uint8_t *trow = pix + (size_t)(y + ky) * (size_t)asz + kx;
            if (ca == 255) {
                uint32_t full = src | 0xff000000u;
                for (int x = x0; x < x1; x++) {
                    uint32_t cov = trow[x];
                    if (cov) row[x] = cov == 255 ? full : blend_px(row[x], src, cov);
                }
            } else {
                for (int x = x0; x < x1; x++) {
                    uint32_t cov = trow[x];
                    if (cov) row[x] = blend_px(row[x], src, (ca * cov + 127) / 255);
                }
            }
        }
        return;
    }
    const float rad_in = (float)(s->pad & 255), bw = (float)((s->pad >> 8) & 255);
    const float hx = 0.5f * s->w, hy = 0.5f * s->h;
    const float rad = rad_in < hx ? (rad_in < hy ? rad_in : hy) : (hx < hy ? hx : hy);
    for (int y = y0; y < y1; y++) {
        uint32_t *row = r->fb + (size_t)y * (size_t)r->w;
        float ly = (float)y + 0.5f - s->y;
        int ty = (int)(fv0 + ly * dv);
        if (ty < 0) ty = 0;
        if (ty >= asz) ty = asz - 1;
        const uint8_t *trow = pix + (size_t)ty * (size_t)asz;
        for (int x = x0; x < x1; x++) {
            float lx = (float)x + 0.5f - s->x;
            int tx = (int)(fu0 + lx * du);
            if (tx < 0) tx = 0;
            if (tx >= asz) tx = asz - 1;
            uint32_t cov = trow[tx];
            if (!cov) continue;
            if (sdf) {
                float qx = (lx > hx ? lx - hx : hx - lx) - (hx - rad), qy = (ly > hy ? ly - hy : hy - ly) - (hy - rad);
                float mx = qx > 0 ? qx : 0, my = qy > 0 ? qy : 0;
                float d = sqrtf(mx * mx + my * my) + (qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0)) - rad;
                float c = 0.5f - d;
                c = c < 0 ? 0 : c > 1 ? 1 : c;
                if (bw > 0) {
                    float c2 = 0.5f - (d + bw);
                    c2 = c2 < 0 ? 0 : c2 > 1 ? 1 : c2;
                    c = c - c2 > 0 ? c - c2 : 0;
                }
                cov = (uint32_t)((float)cov * c + 0.5f);
                if (!cov) continue;
            }
            uint32_t a = ca == 255 ? cov : (ca * cov + 127) / 255;
            if (!a) continue;
            row[x] = a == 255 ? (src | 0xff000000u) : blend_px(row[x], src, a);
        }
    }
}

static inline uint64_t mix64(uint64_t h, uint64_t v) { h = (h ^ v) * 0x9E3779B97F4A7C15ull; return h ^ (h >> 29); }

static inline void clip_of(const Renderer *r, int ci, int *x0, int *y0, int *x1, int *y1) {
    *x0 = 0; *y0 = 0; *x1 = r->w; *y1 = r->h;
    if (ci < 0) return;
    const SClip &c = r->ss->clips[(size_t)ci];
    if (c.x > *x0) *x0 = c.x;
    if (c.y > *y0) *y0 = c.y;
    if (c.x + c.w < *x1) *x1 = c.x + c.w;
    if (c.y + c.h < *y1) *y1 = c.y + c.h;
}

static void soft_flush(Renderer *r) {   /* the frame is only collected here; r_end hashes it per tile and paints what changed */
    SoftState *ss = r->ss;
    ss->insts.insert(ss->insts.end(), r->buf, r->buf + r->n);
    ss->inst_clip.insert(ss->inst_clip.end(), r->n, ss->cur_clip);
    r->n = 0;
}

static void soft_paint_all(Renderer *r, uint32_t *fb) {
    SoftState *ss = r->ss;
    uint32_t *keep = r->fb;
    r->fb = fb;
    size_t total = (size_t)r->w * (size_t)r->h;
    for (size_t i = 0; i < total; i++) fb[i] = ss->clear_px;
    for (size_t i = 0; i < ss->insts.size(); i++) {
        int x0, y0, x1, y1;
        clip_of(r, ss->inst_clip[i], &x0, &y0, &x1, &y1);
        soft_inst(r, &ss->insts[i], x0, y0, x1, y1);
    }
    r->fb = keep;
}

static void soft_bin(Renderer *r, int tw, int th) {   /* pixel box of every instance, then the instances listed per tile in draw order */
    SoftState *ss = r->ss;
    const size_t n = ss->insts.size(), ntiles = (size_t)tw * (size_t)th;
    ss->bbox.resize(n * 4);
    ss->bin_start.assign(ntiles + 1, 0);
    for (size_t i = 0; i < n; i++) {
        const RInst *s = &ss->insts[i];
        int cx0, cy0, cx1, cy1;
        clip_of(r, ss->inst_clip[i], &cx0, &cy0, &cx1, &cy1);
        int x0 = ceil_i(s->x - 0.5f), x1 = ceil_i(s->x + s->w - 0.5f), y0 = ceil_i(s->y - 0.5f), y1 = ceil_i(s->y + s->h - 0.5f);
        if (x0 < cx0) x0 = cx0;
        if (y0 < cy0) y0 = cy0;
        if (x1 > cx1) x1 = cx1;
        if (y1 > cy1) y1 = cy1;
        if (x0 >= x1 || y0 >= y1 || !(s->rgba >> 24)) x1 = x0;
        int *b = &ss->bbox[i * 4];
        b[0] = x0; b[1] = y0; b[2] = x1; b[3] = y1;
        if (x0 >= x1) continue;
        for (int ty = y0 / TILE; ty <= (y1 - 1) / TILE; ty++)
            for (int tx = x0 / TILE; tx <= (x1 - 1) / TILE; tx++) ss->bin_start[(size_t)ty * (size_t)tw + (size_t)tx + 1]++;
    }
    for (size_t t = 0; t < ntiles; t++) ss->bin_start[t + 1] += ss->bin_start[t];
    ss->bin_item.resize(ss->bin_start[ntiles]);
    std::vector<uint32_t> fill(ss->bin_start.begin(), ss->bin_start.end() - 1);
    for (size_t i = 0; i < n; i++) {
        const int *b = &ss->bbox[i * 4];
        if (b[0] >= b[2]) continue;
        for (int ty = b[1] / TILE; ty <= (b[3] - 1) / TILE; ty++)
            for (int tx = b[0] / TILE; tx <= (b[2] - 1) / TILE; tx++) ss->bin_item[fill[(size_t)ty * (size_t)tw + (size_t)tx]++] = (uint32_t)i;
    }
}

static void soft_end(Renderer *r) {
    SoftState *ss = r->ss;
    soft_flush(r);
    const int w = r->w, h = r->h;
    ss->rects.clear();
    if (w > 0 && h > 0 && r->fb) {
        const uint32_t gen = atlas_generation(r->atlas);
        if (ss->pw != w || ss->ph != h || ss->gen != gen) ss->force_full = 1;   /* a recycled atlas reuses texel positions for other glyphs */
        const int tw = (w + TILE - 1) / TILE, th = (h + TILE - 1) / TILE;
        const size_t ntiles = (size_t)tw * (size_t)th;
        soft_bin(r, tw, th);
        ss->hash.assign(ntiles, 0);
        ss->dirty.assign(ntiles, 0);
        size_t ndirty = 0;
        for (size_t t = 0; t < ntiles; t++) {   /* one hash per tile over everything that can reach it, in draw order */
            uint64_t hv = mix64(0x1234567ull, ss->clear_px);
            for (uint32_t k = ss->bin_start[t]; k < ss->bin_start[t + 1]; k++) {
                uint32_t i = ss->bin_item[k];
                uint64_t words[4];
                memcpy(words, &ss->insts[i], sizeof words);   /* RInst is 32 bytes */
                for (int q = 0; q < 4; q++) hv = mix64(hv, words[q]);
                int ci = ss->inst_clip[i];
                if (ci >= 0) { const SClip &c = ss->clips[(size_t)ci]; hv = mix64(hv, (uint64_t)(uint16_t)c.x | (uint64_t)(uint16_t)c.y << 16 | (uint64_t)(uint16_t)c.w << 32 | (uint64_t)(uint16_t)c.h << 48); }
            }
            ss->hash[t] = hv;
            if (t >= ss->prev_hash.size() || ss->prev_hash[t] != hv) { ss->dirty[t] = 1; ndirty++; }
        }
        if (ss->force_full || ndirty * 10 > ntiles * 6) {   /* most of the frame changes: one pass over the instances beats per tile setup */
            soft_paint_all(r, r->fb);
            ss->rects.push_back({0, 0, w, h});
        } else if (ndirty) {
            for (int ty = 0; ty < th; ty++)
                for (int tx = 0; tx < tw; tx++) {
                    size_t t = (size_t)ty * (size_t)tw + (size_t)tx;
                    if (!ss->dirty[t]) continue;
                    int bx0 = tx * TILE, by0 = ty * TILE, bx1 = bx0 + TILE < w ? bx0 + TILE : w, by1 = by0 + TILE < h ? by0 + TILE : h;
                    for (int y = by0; y < by1; y++) { uint32_t *row = r->fb + (size_t)y * (size_t)w; for (int x = bx0; x < bx1; x++) row[x] = ss->clear_px; }
                    for (uint32_t k = ss->bin_start[t]; k < ss->bin_start[t + 1]; k++) {
                        uint32_t i = ss->bin_item[k];
                        const int *b = &ss->bbox[(size_t)i * 4];
                        soft_inst(r, &ss->insts[i], b[0] > bx0 ? b[0] : bx0, b[1] > by0 ? b[1] : by0, b[2] < bx1 ? b[2] : bx1, b[3] < by1 ? b[3] : by1);
                    }
                }
            for (int ty = 0; ty < th; ty++) {   /* damage rectangles: runs of dirty tiles in a row, equal runs of consecutive rows joined */
                int tx = 0;
                while (tx < tw) {
                    if (!ss->dirty[(size_t)ty * (size_t)tw + (size_t)tx]) { tx++; continue; }
                    int e = tx;
                    while (e < tw && ss->dirty[(size_t)ty * (size_t)tw + (size_t)e]) e++;
                    RSoftRect rc = {tx * TILE, ty * TILE, (e < tw ? e * TILE : w) - tx * TILE, (ty + 1) * TILE < h ? TILE : h - ty * TILE};
                    bool merged = false;
                    for (RSoftRect &o : ss->rects)
                        if (o.x == rc.x && o.w == rc.w && o.y + o.h == rc.y) { o.h += rc.h; merged = true; break; }
                    if (!merged) ss->rects.push_back(rc);
                    tx = e;
                }
            }
        }
        ss->prev_hash.swap(ss->hash);
        ss->pw = w; ss->ph = h; ss->gen = gen; ss->force_full = 0;
        if (ss->verify) {   /* the incremental frame must equal one painted from scratch; a mismatch is a bug in the damage tracking */
            ss->ref.resize((size_t)w * (size_t)h);
            soft_paint_all(r, ss->ref.data());
            if (memcmp(ss->ref.data(), r->fb, (size_t)w * (size_t)h * 4) != 0) {
                size_t bad = 0, first = 0;
                for (size_t i = 0; i < (size_t)w * (size_t)h; i++) if (ss->ref[i] != r->fb[i]) { if (!bad) first = i; bad++; }
                fprintf(stderr, "hootedit: CPU rasterizer verify: %zu pixels differ, first at %zu,%zu\n", bad, first % (size_t)w, first / (size_t)w);
                _exit(3);
            }
        }
    }
    ss->insts.clear(); ss->inst_clip.clear(); ss->clips.clear(); ss->cur_clip = -1;
}

const RSoftRect *r_soft_damage(const Renderer *r, int *n) {
    if (!r->soft || !r->ss) { *n = 0; return NULL; }
    *n = (int)r->ss->rects.size();
    return r->ss->rects.data();
}

void r_soft_target(Renderer *r, uint32_t *pixels) {
    if (!r->soft) return;
    if (!pixels && r->ext_fb) { r->fb = NULL; r->fb_cap = 0; r->ss->force_full = 1; }   /* the own buffer has to be allocated again */
    r->ext_fb = pixels;
}

void r_soft_invalidate(Renderer *r) { if (r->soft && r->ss) r->ss->force_full = 1; }

void r_soft_bgr(Renderer *r, int on) { r->swap_rb = on ? 1 : 0; }
int r_soft_is_bgr(const Renderer *r) { return r->swap_rb; }

const uint32_t *r_soft_pixels(const Renderer *r, int *w, int *h) {
    if (!r->soft) return NULL;
    *w = r->w; *h = r->h;
    return r->fb;
}

static GLuint compile(GLenum kind, const char *src) {
    GLuint s = gl.CreateShader(kind);
    gl.ShaderSource(s, 1, &src, NULL);
    gl.CompileShader(s);
    GLint ok = 0;
    gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        gl.GetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "shader: %s\n", log);
        return 0;
    }
    return s;
}

static const char *glsl_head(void) {
    return sd_gl_kind == GLK_ES3 ? "#version 300 es\nprecision highp float;\nprecision highp int;\n" : "#version 330 core\n";
}

static GLuint compile_src(GLenum kind, const char *head, const char *body) {
    const char *parts[2] = {head, body};
    GLuint s = gl.CreateShader(kind);
    gl.ShaderSource(s, 2, parts, NULL);
    gl.CompileShader(s);
    GLint ok = 0;
    gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        gl.GetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "shader: %s\n", log);
        return 0;
    }
    return s;
}

Renderer *r_new(Atlas *a) {
    Renderer *r = static_cast<Renderer*>(calloc(1, sizeof *r));
    if (!r) return NULL;
    r->atlas = a;
    if (sd_gl_kind == GLK_SOFT) { r->soft = 1; r->ss = new SoftState(); r->ss->verify = getenv("HOOTEDIT_SOFT_VERIFY") != NULL; return r; }
    GLuint vs, fs;
    if (sd_gl_kind == GLK_ES2) { vs = compile_src(GL_VERTEX_SHADER, "", VS2); fs = compile_src(GL_FRAGMENT_SHADER, "", FS2); }
    else { vs = compile_src(GL_VERTEX_SHADER, glsl_head(), VS3); fs = compile_src(GL_FRAGMENT_SHADER, glsl_head(), FS3); }
    if (!vs || !fs) { free(r); return NULL; }
    r->prog = gl.CreateProgram();
    gl.AttachShader(r->prog, vs);
    gl.AttachShader(r->prog, fs);
    if (sd_gl_kind == GLK_ES2) {
        static const char *const names[] = {"a_pos", "a_uv", "a_col", "a_local", "a_size", "a_aux"};
        for (GLuint i = 0; i < 6; i++) gl.BindAttribLocation(r->prog, i, names[i]);
    }
    gl.LinkProgram(r->prog);
    GLint ok = 0;
    gl.GetProgramiv(r->prog, GL_LINK_STATUS, &ok);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    if (!ok) { free(r); return NULL; }
    r->u_res = gl.GetUniformLocation(r->prog, "u_res");
    r->u_atlas = gl.GetUniformLocation(r->prog, "u_atlas");
    gl.GenBuffers(1, &r->vbo);
    if (sd_gl_kind == GLK_ES2) {
        uint16_t *idx = static_cast<uint16_t*>(malloc((size_t)MAXQ * 6 * sizeof *idx));
        r->v2 = static_cast<V2*>(malloc((size_t)MAXQ * 4 * sizeof *r->v2));
        if (!idx || !r->v2) { free(idx); free(r->v2); free(r); return NULL; }
        for (int q = 0; q < MAXQ; q++) {
            uint16_t base = (uint16_t)(q * 4);
            uint16_t *p = idx + (size_t)q * 6;
            p[0] = base; p[1] = (uint16_t)(base + 1); p[2] = (uint16_t)(base + 2);
            p[3] = (uint16_t)(base + 2); p[4] = (uint16_t)(base + 1); p[5] = (uint16_t)(base + 3);
        }
        gl.GenBuffers(1, &r->ibo);
        gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, r->ibo);
        gl.BufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)((size_t)MAXQ * 6 * sizeof *idx), idx, GL_STATIC_DRAW);
        free(idx);
        return r;
    }
    gl.GenVertexArrays(1, &r->vao);
    gl.BindVertexArray(r->vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, r->vbo);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(RInst), (void *)0);
    gl.VertexAttribDivisor(0, 1);
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(1, 4, GL_UNSIGNED_SHORT, GL_TRUE, sizeof(RInst), (void *)16);
    gl.VertexAttribDivisor(1, 1);
    gl.EnableVertexAttribArray(2);
    gl.VertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(RInst), (void *)24);
    gl.VertexAttribDivisor(2, 1);
    gl.EnableVertexAttribArray(3);
    gl.VertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(RInst), (void *)28);
    gl.VertexAttribDivisor(3, 1);
    return r;
}

void r_free(Renderer *r) {
    if (!r) return;
    free(r->buf);
    free(r->v2);
    if (r->fb != r->ext_fb) free(r->fb);
    delete r->ss;
    free(r);
}

Atlas *r_atlas(Renderer *r) { return r->atlas; }

void r_begin(Renderer *r, int w, int h, uint32_t clear_rgba) {
    r->w = w; r->h = h; r->n = 0; r->clip_on = 0;
    if (r->soft) {
        size_t need = (size_t)w * (size_t)h;
        if (r->ext_fb) {
            if (r->fb != r->ext_fb) r->ss->force_full = 1;   /* new target: it holds nothing of ours */
            if (r->fb && r->fb_cap) { free(r->fb); r->fb_cap = 0; }
            r->fb = r->ext_fb;
            if (w != r->ss->pw || h != r->ss->ph) r->ss->force_full = 1;
        } else if (need > r->fb_cap || w != r->ss->pw || h != r->ss->ph) {   /* a new size: the old pixels and hashes mean nothing */
            if (need > r->fb_cap) {
                free(r->fb);
                r->fb = static_cast<uint32_t*>(malloc(need * sizeof *r->fb));
                r->fb_cap = r->fb ? need : 0;
            }
            r->ss->force_full = 1;
        }
        if (!r->fb) { r->w = r->h = 0; return; }
        uint32_t px = clear_rgba & 0xffffffu;
        if (r->swap_rb) px = ((px & 255) << 16) | (px & 0xff00u) | (px >> 16);
        px |= 0xff000000u;
        if (px != r->ss->clear_px) { r->ss->clear_px = px; r->ss->force_full = 1; }
        r->ss->insts.clear(); r->ss->inst_clip.clear(); r->ss->clips.clear(); r->ss->cur_clip = -1;
        return;
    }
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor((clear_rgba & 255) / 255.f, ((clear_rgba >> 8) & 255) / 255.f, ((clear_rgba >> 16) & 255) / 255.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

static void flush_es2(Renderer *r) {
    gl.BindBuffer(GL_ARRAY_BUFFER, r->vbo);
    gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, r->ibo);
    const GLsizei stride = (GLsizei)sizeof(V2);
    gl.EnableVertexAttribArray(0); gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void *)0);
    gl.EnableVertexAttribArray(1); gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void *)8);
    gl.EnableVertexAttribArray(2); gl.VertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void *)16);
    gl.EnableVertexAttribArray(3); gl.VertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, (void *)20);
    gl.EnableVertexAttribArray(4); gl.VertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, stride, (void *)28);
    gl.EnableVertexAttribArray(5); gl.VertexAttribPointer(5, 2, GL_FLOAT, GL_FALSE, stride, (void *)36);
    const float inv = 1.f / 65535.f;
    for (size_t first = 0; first < r->n; first += MAXQ) {
        size_t cnt = r->n - first < MAXQ ? r->n - first : MAXQ;
        V2 *v = r->v2;
        for (size_t i = 0; i < cnt; i++) {
            const RInst *s = &r->buf[first + i];
            float ar = (float)(s->pad & 255), ab = (float)((s->pad >> 8) & 255);
            float u0 = s->u0 * inv, v0 = s->v0 * inv, u1 = s->u1 * inv, v1 = s->v1 * inv;
            for (int c = 0; c < 4; c++, v++) {
                float cx = (float)(c & 1), cy = (float)(c >> 1);
                v->x = s->x + cx * s->w; v->y = s->y + cy * s->h;
                v->u = u0 + (u1 - u0) * cx; v->v = v0 + (v1 - v0) * cy;
                v->rgba = s->rgba;
                v->lx = cx * s->w; v->ly = cy * s->h; v->sw = s->w; v->sh = s->h; v->ar = ar; v->ab = ab;
            }
        }
        gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(cnt * 4 * sizeof(V2)), r->v2, GL_STREAM_DRAW);
        gl.DrawElements(GL_TRIANGLES, (GLsizei)(cnt * 6), GL_UNSIGNED_SHORT, (void *)0);
    }
}

void r_flush(Renderer *r) {
    if (!r->n) return;
    if (r->soft) { soft_flush(r); return; }
    gl.UseProgram(r->prog);
    gl.Uniform2f(r->u_res, (float)r->w, (float)r->h);
    gl.ActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atlas_texture(r->atlas));
    gl.Uniform1i(r->u_atlas, 0);
    if (sd_gl_kind == GLK_ES2) { flush_es2(r); r->n = 0; return; }
    gl.BindVertexArray(r->vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, r->vbo);
    size_t bytes = r->n * sizeof(RInst);
    if (bytes > r->vbo_cap) { r->vbo_cap = bytes * 2; gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)r->vbo_cap, NULL, GL_STREAM_DRAW); }
    else gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)r->vbo_cap, NULL, GL_STREAM_DRAW);
    gl.BufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)bytes, r->buf);
    gl.DrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)r->n);
    r->n = 0;
}

void r_end(Renderer *r) { if (r->soft) { soft_end(r); return; } r_flush(r); }

void r_clip(Renderer *r, int x, int y, int w, int h) {
    r_flush(r);
    if (r->soft) { r->ss->clips.push_back({x, y, w < 0 ? 0 : w, h < 0 ? 0 : h}); r->ss->cur_clip = (int)r->ss->clips.size() - 1; r->clip_on = 1; return; }
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, r->h - (y + h), w < 0 ? 0 : w, h < 0 ? 0 : h);
    r->clip_on = 1;
}

void r_clip_off(Renderer *r) { r_flush(r); if (r->soft) r->ss->cur_clip = -1; else glDisable(GL_SCISSOR_TEST); r->clip_on = 0; }

static void grow(Renderer *r, size_t extra) {
    if (r->n + extra <= r->cap) return;
    size_t nc = r->cap ? r->cap * 2 : 4096;
    while (nc < r->n + extra) nc *= 2;
    RInst *nb = static_cast<RInst*>(realloc(r->buf, nc * sizeof *nb));
    if (!nb) return;
    r->buf = nb; r->cap = nc;
}

void r_push(Renderer *r, const RInst *inst, int n) {
    grow(r, (size_t)n);
    if (r->n + (size_t)n > r->cap) return;
    memcpy(r->buf + r->n, inst, (size_t)n * sizeof *inst);
    r->n += (size_t)n;
}

void r_push_at(Renderer *r, const RInst *inst, int n, float dx, float dy) {
    grow(r, (size_t)n);
    if (r->n + (size_t)n > r->cap) return;
    RInst *o = r->buf + r->n;
    for (int i = 0; i < n; i++) { o[i] = inst[i]; o[i].x += dx; o[i].y += dy; }
    r->n += (size_t)n;
}

size_t r_mark(const Renderer *r) { return r->n; }
void r_rewind(Renderer *r, size_t mark) { if (mark <= r->n) r->n = mark; }

void r_make_rect(Renderer *r, RInst *o, float x, float y, float w, float h, uint32_t rgba) {
    uint16_t wx, wy;
    atlas_white(r->atlas, &wx, &wy);
    float inv = 65535.f / (float)atlas_size(r->atlas);
    uint16_t u = (uint16_t)((wx + 1) * inv), v = (uint16_t)((wy + 1) * inv);
    *o = (RInst){x, y, w, h, u, v, u, v, rgba, 0};
}

void r_make_glyph(Renderer *r, RInst *o, const AtlasGlyph *g, float pen_x, float baseline_y, uint32_t rgba) {
    float inv = 65535.f / (float)atlas_size(r->atlas);
    *o = (RInst){pen_x + g->left, baseline_y - g->top, (float)g->w, (float)g->h,
                 (uint16_t)(g->x * inv + 0.5f), (uint16_t)(g->y * inv + 0.5f),
                 (uint16_t)((g->x + g->w) * inv + 0.5f), (uint16_t)((g->y + g->h) * inv + 0.5f), rgba, 0};
}

void r_rrect(Renderer *r, float x, float y, float w, float h, uint32_t rgba, int radius, int border) {
    RInst i;
    r_make_rect(r, &i, x, y, w, h, rgba);
    i.pad = (uint32_t)(radius > 255 ? 255 : radius) | (uint32_t)(border > 255 ? 255 : border) << 8;
    r_push(r, &i, 1);
}

void r_rect(Renderer *r, float x, float y, float w, float h, uint32_t rgba) {
    RInst i;
    r_make_rect(r, &i, x, y, w, h, rgba);
    r_push(r, &i, 1);
}
