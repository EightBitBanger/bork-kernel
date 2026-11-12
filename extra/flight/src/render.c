/*
 * render.c: the 3D picture, all on the CPU.
 *
 * A sky and ground gradient with distance haze fills the picture first.
 * Then flat things on the ground (a map's colored areas, hazed pixel by
 * pixel like the ground; fields, runways, SAM and target pads) in order,
 * then everything standing up (hills, buildings, SAM sites, missiles,
 * explosions) farthest first: the painter's algorithm. Polygons are flat
 * shaded, clipped at the near plane and the picture's edges, and filled a
 * row at a time. Smoke trails go on top. The HUD (hud.c) comes last.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flight.h"

wm_window_t    win;
gfx_surface_t* surf;
bool           quit;

uint32_t*      frame;
int32_t        sw, sh;
bool           chunky;
vec3           cam_pos, cam_r, cam_u, cam_f;
bool           paused, show_help = false, show_fps;
unsigned       fps;
float          world_time;

static uint32_t* scene;             // rw x rh: the 3D view (frame itself unless chunky)
static int32_t   rw, rh;
static uint32_t* half_buf;          // Scene when chunky
static int32_t*  span_l;
static int32_t*  span_r;
static int32_t*  row_d;             // Ground: each row's view ray height at x = 0 (16.16, LUT units) ...
static int32_t   step_fx;           // ... and its change a pixel to the right
static float     cam_fl, cam_cx, cam_cy;
static unsigned  fps_frames;
static uint64_t  fps_since;

static void setup_buffers(void) {
    int32_t w = surf ? gfx_surface_width(surf) : WIN_W, h = surf ? gfx_surface_height(surf) : WIN_H;
    if (w > MAX_W) w = MAX_W;
    if (h > MAX_H) h = MAX_H;
    if (w < 16) w = 16;
    if (h < 16) h = 16;
    if (w != sw || h != sh || !frame) {
        uint32_t* f = realloc(frame, (size_t)w * h * sizeof(uint32_t));
        uint32_t* hb = realloc(half_buf, (size_t)((w + 1) / 2) * ((h + 1) / 2) * sizeof(uint32_t));
        int32_t* l = realloc(span_l, (size_t)h * sizeof(int32_t));
        int32_t* r = realloc(span_r, (size_t)h * sizeof(int32_t));
        int32_t* rd = realloc(row_d, (size_t)h * sizeof(int32_t));
        if (rd) row_d = rd;
        if (f) frame = f;
        if (hb) half_buf = hb;
        if (l) span_l = l;
        if (r) span_r = r;
        if (!f || !hb || !l || !r || !rd) {
            fprintf(stderr, APP_NAME ": not enough memory\n");
            quit = true;
            return;
        }
        sw = w;
        sh = h;
    }
    if (chunky) {
        scene = half_buf;
        rw = (sw + 1) / 2;
        rh = (sh + 1) / 2;
    } else {
        scene = frame;
        rw = sw;
        rh = sh;
    }
}

//
// Sky and ground: for every pixel, how far its view ray points up or down
// (linear across a row), looked up in a table made for this frame's
// altitude: sky blue fading to haze at the horizon, ground hazing with
// distance.
//

#define LUT_SIZE        4096
#define LUT_SCALE       1024        // Entries per unit of ray height
static uint32_t sky_lut[LUT_SIZE];

static void build_sky_lut(float altitude) {
    if (altitude < 1)
        altitude = 1;
    for (int i = 0; i < LUT_SIZE; i++) {
        float dy = (float)(i - LUT_SIZE / 2) / LUT_SCALE;
        if (dy >= 0) {
            float t = clampf(dy * 1.5f, 0, 1);
            t = 1 - (1 - t) * (1 - t);
            sky_lut[i] = mix(C_HAZE, C_ZENITH, (int)(t * 256));
        } else {
            sky_lut[i] = mix(map.loaded ? map.outside : C_GROUND, C_HAZE, fog_amount(altitude / -dy));
        }
    }
}

static void draw_sky_ground(void) {
    build_sky_lut(cam_pos.y);
    float a = 1.0f / cam_fl;
    float step = cam_r.y * a;
    step_fx = (int32_t)(step * (LUT_SCALE * 65536.0f));
    for (int32_t y = 0; y < rh; y++) {
        float dy = cam_f.y - ((float)y + 0.5f - cam_cy) * a * cam_u.y + (0.5f - cam_cx) * step;
        int32_t d = (int32_t)(clampf(dy, -2.5f, 2.5f) * (LUT_SCALE * 65536.0f));
        row_d[y] = d;
        uint32_t* out = scene + (size_t)y * rw;
        for (int32_t x = 0; x < rw; x++, d += step_fx) {
            int32_t i = (d >> 16) + LUT_SIZE / 2;
            out[x] = sky_lut[i < 0 ? 0 : i >= LUT_SIZE ? LUT_SIZE - 1 : i];
        }
    }
}

// The same haze for a ground color of a map: a table per color, made
// when first needed in a frame
#define GROUND_LUTS     16

static struct {
    uint32_t color;
    unsigned frame;
    uint32_t lut[LUT_SIZE / 2];
} ground_luts[GROUND_LUTS];
static unsigned frame_no;
static int      next_lut;

static const uint32_t* ground_lut(uint32_t color) {
    for (int i = 0; i < GROUND_LUTS; i++)
        if (ground_luts[i].frame == frame_no && ground_luts[i].color == color)
            return ground_luts[i].lut;
    int k = next_lut;
    next_lut = (next_lut + 1) % GROUND_LUTS;
    float altitude = cam_pos.y < 1 ? 1 : cam_pos.y;
    ground_luts[k].color = color;
    ground_luts[k].frame = frame_no;
    for (int i = 0; i < LUT_SIZE / 2; i++) {
        float dy = (float)(i - LUT_SIZE / 2) / LUT_SCALE;
        ground_luts[k].lut[i] = dy < 0 ? mix(color, C_HAZE, fog_amount(altitude / -dy)) : C_HAZE;
    }
    return ground_luts[k].lut;
}

//
// Polygons
//

#define MAX_POLY        16

// Keep the part of polygon `in` on one side of a screen edge
static int clip_edge(const vec2* in, int n, vec2* out, int axis, float limit, bool keep_below) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        vec2 a = in[i], b = in[(i + 1) % n];
        float va = axis ? a.y : a.x, vb = axis ? b.y : b.x;
        bool ina = keep_below ? va <= limit : va >= limit;
        bool inb = keep_below ? vb <= limit : vb >= limit;
        if (ina)
            out[m++] = a;
        if (ina != inb && m < MAX_POLY) {
            float t = (limit - va) / (vb - va);
            vec2 p = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
            if (axis) p.y = limit; else p.x = limit;
            out[m++] = p;
        }
        if (m >= MAX_POLY)
            break;
    }
    return m;
}

// Fill with a color, or (lut not NULL) with the ground's haze for it
static void fill_poly(const vec2* p, int n, uint32_t color, const uint32_t* lut) {
    float fy0 = p[0].y, fy1 = p[0].y;
    for (int i = 1; i < n; i++) {
        if (p[i].y < fy0) fy0 = p[i].y;
        if (p[i].y > fy1) fy1 = p[i].y;
    }
    int32_t ymin = (int32_t)ceilf(fy0 - 0.5f), ymax = (int32_t)ceilf(fy1 - 0.5f);  // Rows ymin .. ymax-1
    if (ymin < 0) ymin = 0;
    if (ymax > rh) ymax = rh;
    if (ymin >= ymax)
        return;
    for (int32_t y = ymin; y < ymax; y++) {
        span_l[y] = INT32_MAX;
        span_r[y] = INT32_MIN;
    }

    for (int i = 0; i < n; i++) {
        vec2 a = p[i], b = p[(i + 1) % n];
        if (a.y > b.y) { vec2 t = a; a = b; b = t; }
        int32_t ys = (int32_t)ceilf(a.y - 0.5f), ye = (int32_t)ceilf(b.y - 0.5f);
        if (ys < ymin) ys = ymin;
        if (ye > ymax) ye = ymax;
        if (ys >= ye)
            continue;
        float slope = (b.x - a.x) / (b.y - a.y);
        int32_t x = (int32_t)((a.x + ((float)ys + 0.5f - a.y) * slope) * 65536.0f);
        int32_t dx = fabsf(slope) < 16384.0f ? (int32_t)(slope * 65536.0f) : 0;   // Steeper: one row only
        for (int32_t y = ys; y < ye; y++, x += dx) {
            if (x < span_l[y]) span_l[y] = x;
            if (x > span_r[y]) span_r[y] = x;
        }
    }

    for (int32_t y = ymin; y < ymax; y++) {
        if (span_l[y] > span_r[y])
            continue;
        int32_t x0 = (span_l[y] + 0x7FFF) >> 16, x1 = (span_r[y] + 0x7FFF) >> 16;
        if (lut) {
            // Ground areas: a pixel wider each side, so neighbors leave no cracks
            x0 = span_l[y] >> 16;
            x1 = (span_r[y] >> 16) + 1;
        }
        if (x0 < 0) x0 = 0;
        if (x1 > rw) x1 = rw;
        uint32_t* out = scene + (size_t)y * rw;
        if (lut) {
            int32_t d = row_d[y] + x0 * step_fx;
            for (int32_t x = x0; x < x1; x++, d += step_fx) {
                int32_t i = (d >> 16) + LUT_SIZE / 2;
                out[x] = lut[i < 0 ? 0 : i >= LUT_SIZE / 2 ? LUT_SIZE / 2 - 1 : i];
            }
        } else {
            for (int32_t x = x0; x < x1; x++)
                out[x] = color;
        }
    }
}

static inline vec3 to_view(vec3 p) {
    vec3 d = vsub(p, cam_pos);
    return v3(vdot(d, cam_r), vdot(d, cam_u), vdot(d, cam_f));
}

// Frustum culling: could a ball of radius r at c be in the picture? (The
// view's four sides and the plane behind the camera; draw_scene sets the
// slopes of the sides from the picture's size.)
static float view_tan_x, view_tan_y, view_sec_x, view_sec_y;

static bool in_view(vec3 c, float r) {
    vec3 v = to_view(c);
    if (v.z < -r)
        return false;
    if (fabsf(v.x) - v.z * view_tan_x > r * view_sec_x)
        return false;
    if (fabsf(v.y) - v.z * view_tan_y > r * view_sec_y)
        return false;
    return true;
}

// The same for something flat on the ground, by its box
static bool box_in_view(float x0, float z0, float x1, float z1) {
    float hx = (x1 - x0) * 0.5f, hz = (z1 - z0) * 0.5f;
    return in_view(v3(x0 + hx, 0, z0 + hz), sqrtf(hx * hx + hz * hz));
}

// Too small to see: under about a pixel across at that distance
static bool too_small(vec3 c, float r) {
    float z = vdot(vsub(c, cam_pos), cam_f);
    return z > 0 && r * cam_fl < z * 0.6f;
}

// Draw a flat world-space polygon: to the camera's view, clipped at the
// near plane, projected, clipped to the picture, filled
static void draw_poly_lut(const vec3* world, int n, uint32_t color, const uint32_t* lut) {
    vec3 view[MAX_POLY], clipped[MAX_POLY];
    for (int i = 0; i < n; i++)
        view[i] = to_view(world[i]);

    int m = 0;
    for (int i = 0; i < n && m < MAX_POLY - 1; i++) {
        vec3 a = view[i], b = view[(i + 1) % n];
        bool ina = a.z >= NEAR_Z, inb = b.z >= NEAR_Z;
        if (ina)
            clipped[m++] = a;
        if (ina != inb) {
            float t = (NEAR_Z - a.z) / (b.z - a.z);
            clipped[m++] = v3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, NEAR_Z);
        }
    }
    if (m < 3)
        return;

    vec2 pa[MAX_POLY], pb[MAX_POLY];
    for (int i = 0; i < m; i++) {
        float k = cam_fl / clipped[i].z;
        pa[i].x = cam_cx + clipped[i].x * k;
        pa[i].y = cam_cy - clipped[i].y * k;
    }
    m = clip_edge(pa, m, pb, 0, 0, false);
    m = clip_edge(pb, m, pa, 0, (float)rw, true);
    m = clip_edge(pa, m, pb, 1, 0, false);
    m = clip_edge(pb, m, pa, 1, (float)rh, true);
    if (m >= 3)
        fill_poly(pa, m, color, lut);
}

static void draw_poly(const vec3* world, int n, uint32_t color) {
    draw_poly_lut(world, n, color, NULL);
}

static void fill_circle(float cx, float cy, float r, uint32_t color) {
    int32_t y0 = (int32_t)(cy - r), y1 = (int32_t)(cy + r);
    if (y0 < 0) y0 = 0;
    if (y1 >= rh) y1 = rh - 1;
    for (int32_t y = y0; y <= y1; y++) {
        float dy = (float)y + 0.5f - cy, w2 = r * r - dy * dy;
        if (w2 <= 0)
            continue;
        float w = sqrtf(w2);
        int32_t x0 = (int32_t)(cx - w + 0.5f), x1 = (int32_t)(cx + w + 0.5f);
        if (x0 < 0) x0 = 0;
        if (x1 > rw) x1 = rw;
        uint32_t* out = scene + (size_t)y * rw;
        for (int32_t x = x0; x < x1; x++)
            out[x] = color;
    }
}

// A world-space line (smoke), clipped at the near plane and the picture
static void draw_line3d(vec3 a, vec3 b, uint32_t color) {
    vec3 va = to_view(a), vb = to_view(b);
    if (va.z < NEAR_Z && vb.z < NEAR_Z)
        return;
    if (va.z < NEAR_Z || vb.z < NEAR_Z) {
        float t = (NEAR_Z - va.z) / (vb.z - va.z);
        vec3 p = v3(va.x + (vb.x - va.x) * t, va.y + (vb.y - va.y) * t, NEAR_Z);
        if (va.z < NEAR_Z) va = p; else vb = p;
    }
    float x0 = cam_cx + va.x * cam_fl / va.z, y0 = cam_cy - va.y * cam_fl / va.z;
    float x1 = cam_cx + vb.x * cam_fl / vb.z, y1 = cam_cy - vb.y * cam_fl / vb.z;

    // Liang-Barsky to the picture, then Bresenham
    float t0 = 0, t1 = 1, dx = x1 - x0, dy = y1 - y0;
    float p[4] = { -dx, dx, -dy, dy };
    float q[4] = { x0, (float)rw - 1 - x0, y0, (float)rh - 1 - y0 };
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0) {
            if (q[i] < 0)
                return;
        } else {
            float t = q[i] / p[i];
            if (p[i] < 0) { if (t > t1) return; if (t > t0) t0 = t; }
            else          { if (t < t0) return; if (t < t1) t1 = t; }
        }
    }
    int32_t ax = (int32_t)(x0 + dx * t0 + 0.5f), ay = (int32_t)(y0 + dy * t0 + 0.5f);
    int32_t bx = (int32_t)(x0 + dx * t1 + 0.5f), by = (int32_t)(y0 + dy * t1 + 0.5f);
    int32_t ddx = abs(bx - ax), ddy = -abs(by - ay);
    int32_t sx = ax < bx ? 1 : -1, sy = ay < by ? 1 : -1, err = ddx + ddy;
    for (;;) {
        if (ax >= 0 && ay >= 0 && ax < rw && ay < rh)
            scene[(size_t)ay * rw + ax] = color;
        if (ax == bx && ay == by)
            break;
        int32_t e2 = 2 * err;
        if (e2 >= ddy) { err += ddy; ax += sx; }
        if (e2 <= ddx) { err += ddx; ay += sy; }
    }
}

//
// Flat on the ground: fields, base pavement and markings, SAM pads
//

static void draw_fields(void) {
    int32_t ci = cell_of(cam_pos.x, TILE), cj = cell_of(cam_pos.z, TILE);
    int32_t n = (int32_t)(FIELD_DRAW_RANGE / TILE) < TILE_RANGE ? (int32_t)(FIELD_DRAW_RANGE / TILE) : TILE_RANGE;
    float range = n * TILE;
    for (int32_t j = cj - n; j <= cj + n; j++) {
        for (int32_t i = ci - n; i <= ci + n; i++) {
            float x0 = (float)i * TILE, z0 = (float)j * TILE;
            vec3 center = v3(x0 + TILE * 0.5f, 0, z0 + TILE * 0.5f);
            vec3 d = vsub(center, cam_pos);
            if (d.x * d.x + d.z * d.z > range * range)
                continue;
            if (!in_view(center, TILE * 0.71f))
                continue;
            int type = tile_type(i, j);
            if (!type)
                continue;
            vec3 quad[4] = {
                v3(x0, 0, z0), v3(x0 + TILE, 0, z0), v3(x0 + TILE, 0, z0 + TILE), v3(x0, 0, z0 + TILE),
            };
            draw_poly(quad, 4, mix(tile_colors[type], C_HAZE, fog_amount(vlen(d))));
        }
    }
}

// A map's ground: its own color inside the bounds (if not the same as
// outside), then its areas in the file's order
static void draw_map_ground(void) {
    if (!map.loaded)
        return;
    if (map.ground != map.outside) {
        vec3 quad[4] = { v3(map.x0, 0, map.z0), v3(map.x1, 0, map.z0), v3(map.x1, 0, map.z1), v3(map.x0, 0, map.z1) };
        draw_poly_lut(quad, 4, map.ground, ground_lut(map.ground));
    }
    float far2 = FOG_FAR * FOG_FAR;
    for (int i = 0; i < map.tri_count; i++) {
        const map_tri_t* t = &map.tris[i];
        float dx = t->x0 > cam_pos.x ? t->x0 - cam_pos.x : cam_pos.x > t->x1 ? cam_pos.x - t->x1 : 0;
        float dz = t->z0 > cam_pos.z ? t->z0 - cam_pos.z : cam_pos.z > t->z1 ? cam_pos.z - t->z1 : 0;
        if (dx * dx + dz * dz > far2 || !box_in_view(t->x0, t->z0, t->x1, t->z1))
            continue;
        vec3 tri[3] = { v3(t->ax, 0, t->az), v3(t->bx, 0, t->bz), v3(t->cx, 0, t->cz) };
        draw_poly_lut(tri, 3, t->color, ground_lut(t->color));
    }
}

static void base_rect(const base_t* b, float a0, float a1, float c0, float c1, uint32_t color) {
    vec3 quad[4] = {
        base_point(b, a0, c0, 0), base_point(b, a1, c0, 0), base_point(b, a1, c1, 0), base_point(b, a0, c1, 0),
    };
    vec3 mid = vsub(base_point(b, (a0 + a1) * 0.5f, (c0 + c1) * 0.5f, 0), cam_pos);
    draw_poly(quad, 4, mix(color, C_HAZE, fog_amount(vlen(mid))));
}

#define MAX_NEAR        64

static void draw_base_ground(void) {
    base_t near[MAX_NEAR];
    int count = bases_near(cam_pos.x, cam_pos.z, BASE_DRAW_RANGE + RUNWAY_HALF_LEN, near, MAX_NEAR);
    for (int n = 0; n < count; n++) {
        const base_t b = near[n];
        float dist = vlen(vsub(v3(b.x, 0, b.z), cam_pos));
        if (dist > BASE_DRAW_RANGE + RUNWAY_HALF_LEN || !in_view(v3(b.x, 0, b.z), RUNWAY_HALF_LEN + 300))
            continue;
        for (size_t k = 0; k < COUNT(base_pavement); k++) {
            const base_rect_t* r = &base_pavement[k];
            base_rect(&b, r->a0, r->a1, r->c0, r->c1, r->color);
        }
        if (dist > DETAIL_RANGE)
            continue;
        // Centerline dashes, threshold stripes and aiming blocks at both ends
        for (float a = -RUNWAY_HALF_LEN + 120; a < RUNWAY_HALF_LEN - 120; a += 60)
            base_rect(&b, a, a + 30, -0.9f, 0.9f, C_MARKING);
        for (int end = -1; end <= 1; end += 2) {
            float a0 = end * (RUNWAY_HALF_LEN - 10), a1 = end * (RUNWAY_HALF_LEN - 60);
            for (float c = -RUNWAY_HALF_W + 4; c < RUNWAY_HALF_W - 4; c += 6)
                base_rect(&b, a0 < a1 ? a0 : a1, a0 < a1 ? a1 : a0, c, c + 3, C_MARKING);
            float b0 = end * (RUNWAY_HALF_LEN - 300), b1 = end * (RUNWAY_HALF_LEN - 360);
            float lo = b0 < b1 ? b0 : b1, hi = b0 < b1 ? b1 : b0;
            base_rect(&b, lo, hi, -16, -8, C_MARKING);
            base_rect(&b, lo, hi, 8, 16, C_MARKING);
        }
    }
}

static void draw_sam_pads(void) {
    sam_site_t near[MAX_NEAR];
    int count = sams_near(cam_pos.x, cam_pos.z, SAM_DRAW_RANGE, near, MAX_NEAR);
    for (int n = 0; n < count; n++) {
        const sam_site_t s = near[n];
        float dist = vlen(vsub(v3(s.x, 0, s.z), cam_pos));
        if (dist > SAM_DRAW_RANGE || !in_view(v3(s.x, 0, s.z), SAM_RING + 30))
            continue;
        // An octagon of packed earth under the site
        static const float ring[8][2] = {
            { 1, 0.414f }, { 0.414f, 1 }, { -0.414f, 1 }, { -1, 0.414f },
            { -1, -0.414f }, { -0.414f, -1 }, { 0.414f, -1 }, { 1, -0.414f },
        };
        float r = SAM_RING + 30;
        vec3 oct[8];
        for (int k = 0; k < 8; k++)
            oct[k] = v3(s.x + ring[k][0] * r, 0, s.z + ring[k][1] * r);
        draw_poly(oct, 8, mix(sam_destroyed(s.ci, s.cj) ? C_SCORCHED : C_SAM_PAD, C_HAZE, fog_amount(dist)));
    }

    // A map's targets stand on pads of their own
    for (int i = 0; i < map.target_count; i++) {
        const map_target_t* t = &map.targets[i];
        float dist = vlen(vsub(v3(t->x, 0, t->z), cam_pos));
        if (t->radius <= 0 || dist > TARGET_DRAW_RANGE || !in_view(v3(t->x, 0, t->z), t->radius * 1.42f))
            continue;
        vec3 ax = vscale(t->ax, t->radius), az = vscale(t->az, t->radius), c = v3(t->x, 0, t->z);
        vec3 quad[4] = { vsub(vsub(c, ax), az), vsub(vadd(c, ax), az), vadd(vadd(c, ax), az), vadd(vsub(c, ax), az) };
        draw_poly(quad, 4, mix(t->destroyed ? C_SCORCHED : map_target_pad_color(t->type), C_HAZE, fog_amount(dist)));
    }
}

//
// Standing things, farthest first
//

enum { OBJ_HILL, OBJ_BOX, OBJ_BLAST, OBJ_PLANE, OBJ_WRECK, OBJ_CHUTE, OBJ_TRAFFIC, OBJ_TANK, OBJ_SHIP, OBJ_NUKE };

typedef struct {
    int         type;
    float       dist2;
    union {
        hill_t  hill;
        struct {
            vec3     center, ax, ay, az;    // Half extents along the box's three axes
            uint32_t color;
        } box;
        int     blast;
        int     bandit;
        int     traffic;
        int     ship;
    };
} object_t;

#define MAX_OBJECTS     2048

static object_t objects[MAX_OBJECTS];
static int      object_count;

static void add_object(object_t* o, vec3 center) {
    if (object_count >= MAX_OBJECTS)
        return;
    vec3 d = vsub(center, cam_pos);
    o->dist2 = vdot(d, d);
    objects[object_count++] = *o;
}

static int farther_first(const void* a, const void* b) {
    float da = ((const object_t*)a)->dist2, db = ((const object_t*)b)->dist2;
    return da < db ? 1 : da > db ? -1 : 0;
}

static void add_box(vec3 center, vec3 ax, vec3 ay, vec3 az, uint32_t color) {
    float r = vlen(ax) + vlen(ay) + vlen(az);
    if (!in_view(center, r) || too_small(center, r))
        return;                             // Out of the picture, or too small to see
    object_t o;
    o.type = OBJ_BOX;
    o.box.center = center;
    o.box.ax = ax;
    o.box.ay = ay;
    o.box.az = az;
    o.box.color = color;
    add_object(&o, center);
}

// A tank or silo standing on the ground: r across, h high (in the box's
// fields: center at its middle, ax.x the radius, ay.y half the height)
static void add_tank(float x, float z, float r, float h, uint32_t color) {
    vec3 center = v3(x, h * 0.5f, z);
    float br = r + h * 0.5f;
    if (!in_view(center, br) || too_small(center, br))
        return;
    object_t o;
    o.type = OBJ_TANK;
    o.box.center = center;
    o.box.ax = v3(r, 0, 0);
    o.box.ay = v3(0, h * 0.5f, 0);
    o.box.az = v3(0, 0, 0);
    o.box.color = color;
    add_object(&o, center);
}

static const vec3 sun = { 0.42f, 0.78f, 0.46f };     // Toward the sun, normalized

static uint32_t lit(uint32_t color, vec3 normal, int fog) {
    float light = 0.42f + 0.62f * clampf(vdot(normal, sun), 0, 1);
    return mix(shade(color, (int)(light * 256)), C_HAZE, fog);
}

static void draw_hill(const hill_t* h, int fog) {
    vec3 apex = v3(h->x, h->height, h->z);
    vec3 corner[4] = {
        v3(h->x - h->half, 0, h->z - h->half), v3(h->x + h->half, 0, h->z - h->half),
        v3(h->x + h->half, 0, h->z + h->half), v3(h->x - h->half, 0, h->z + h->half),
    };
    // Outward normals of the four sides (south, east, north, west)
    float hn = h->height, sn = h->half;
    vec3 normal[4] = { v3(0, sn, -hn), v3(hn, sn, 0), v3(0, sn, hn), v3(-hn, sn, 0) };
    for (int f = 0; f < 4; f++) {
        vec3 nrm = vnorm(normal[f]);
        if (vdot(nrm, vsub(cam_pos, corner[f])) <= 0)
            continue;                       // Facing away
        vec3 tri[3] = { corner[f], corner[(f + 1) % 4], apex };
        draw_poly(tri, 3, lit(h->color, nrm, fog));
    }
}

// A box: the faces turned toward the camera (at most three)
static void draw_box(vec3 c, vec3 ax, vec3 ay, vec3 az, uint32_t color, int fog) {
    vec3 axes[3] = { ax, ay, az };
    for (int k = 0; k < 3; k++) {
        vec3 a = axes[k], u = axes[(k + 1) % 3], v = axes[(k + 2) % 3];
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            vec3 fc = vadd(c, vscale(a, (float)sgn));
            vec3 n = vscale(vnorm(a), (float)sgn);
            if (vdot(n, vsub(cam_pos, fc)) <= 0)
                continue;
            vec3 quad[4] = {
                vsub(vsub(fc, u), v), vsub(vadd(fc, u), v), vadd(vadd(fc, u), v), vadd(vsub(fc, u), v),
            };
            draw_poly(quad, 4, lit(color, n, fog));
        }
    }
}

// A tank: TANK_SIDES flat sides (those facing the camera) and its top
#define TANK_SIDES 10
static void draw_tank(vec3 c, float r, float hh, uint32_t color, int fog) {
    float bottom = c.y - hh, top = c.y + hh;
    vec3 ring[TANK_SIDES];
    for (int k = 0; k < TANK_SIDES; k++) {
        float a = (float)k * (6.2831853f / TANK_SIDES);
        ring[k] = v3(c.x + cosf(a) * r, top, c.z + sin_f(a) * r);
    }
    for (int k = 0; k < TANK_SIDES; k++) {
        vec3 p0 = ring[k], p1 = ring[(k + 1) % TANK_SIDES];
        vec3 mid = v3((p0.x + p1.x) * 0.5f, c.y, (p0.z + p1.z) * 0.5f);
        vec3 n = vnorm(v3(mid.x - c.x, 0, mid.z - c.z));
        if (vdot(n, vsub(cam_pos, mid)) <= 0)
            continue;
        vec3 quad[4] = { v3(p0.x, bottom, p0.z), v3(p1.x, bottom, p1.z), p1, p0 };
        draw_poly(quad, 4, lit(color, n, fog));
    }
    if (cam_pos.y > top)
        draw_poly(ring, TANK_SIDES, lit(shade(color, 236), v3(0, 1, 0), fog));
}

static void draw_blast(const blast_t* b) {
    vec3 v = to_view(b->pos);
    if (v.z < 80)
        return;                             // Too close to see as a ball (the one that hit us)
    float t = b->age;
    float size = 45.0f * clampf(t / 0.35f, 0.15f, 1) + t * 10.0f;
    float k = cam_fl / v.z;
    float x = cam_cx + v.x * k, y = cam_cy - v.y * k, r = size * k;
    if (r < 1.5f)
        r = 1.5f;
    int fog = fog_amount(v.z) / 2;
    uint32_t smoke = mix(GFX_RGB(255, 150, 40), GFX_RGB(60, 58, 56), (int)(clampf(t / 1.2f, 0, 1) * 256));
    fill_circle(x, y, r, mix(smoke, C_HAZE, fog));
    if (t < 0.6f)
        fill_circle(x, y, r * (0.6f - t), mix(GFX_RGB(255, 240, 160), C_HAZE, fog));
}

// A column: a tank seen from anywhere (its underside too, from below)
static void draw_column(vec3 c, float r, float hh, uint32_t color, int fog) {
    draw_tank(c, r, hh, color, fog);
    if (cam_pos.y < c.y - hh) {
        vec3 ring[TANK_SIDES];
        for (int k = 0; k < TANK_SIDES; k++) {
            float a = (float)k * (6.2831853f / TANK_SIDES);
            ring[k] = v3(c.x + cosf(a) * r, c.y - hh, c.z + sin_f(a) * r);
        }
        draw_poly(ring, TANK_SIDES, lit(shade(color, 150), v3(0, -1, 0), fog));
    }
}

// The nuke: a white fireball that swells and rises, cools to orange and
// red as its cloud spreads into a mushroom cap on a dusty stem, over a
// ring of dust thrown out along the ground. It is so big it shows through
// most of the haze
#define NUKE_TOP        6000.0f     // How high the cloud climbs ...
#define NUKE_RISE       10.0f       // ... (half way after this many seconds)
#define NUKE_BALL       1200.0f     // The fireball's radius
#define NUKE_CAP        2600.0f     // The cap's radius, spread out

static float nuke_top(void) {
    return nuke.pos.y + NUKE_TOP * nuke.age / (nuke.age + NUKE_RISE);
}

static uint32_t nuke_fire(float t) {        // White hot, cooling
    if (t < 1.0f)
        return mix(GFX_RGB(255, 255, 240), GFX_RGB(255, 236, 150), (int)(t * 256));
    if (t < 4.0f)
        return mix(GFX_RGB(255, 236, 150), GFX_RGB(255, 140, 40), (int)((t - 1) / 3 * 256));
    return mix(GFX_RGB(255, 140, 40), GFX_RGB(150, 60, 34), (int)(clampf((t - 4) / 10, 0, 1) * 256));
}

static void draw_nuke_ball(int fog) {
    float t = nuke.age;
    if (t > 10.0f)
        return;                             // Inside the cap by now
    vec3 c = v3(nuke.pos.x, nuke_top(), nuke.pos.z);
    vec3 v = to_view(c);
    float size = NUKE_BALL * clampf(t / 1.5f, 0.15f, 1);
    if (v.z < size * 0.5f)
        return;
    float k = cam_fl / v.z;
    float x = cam_cx + v.x * k, y = cam_cy - v.y * k, r = size * k;
    uint32_t c0 = mix(nuke_fire(t), GFX_RGB(158, 146, 134), (int)(clampf((t - 6) / 4, 0, 1) * 200));
    fill_circle(x, y, r, mix(c0, C_HAZE, fog / 3));
    if (t < 2.5f)
        fill_circle(x, y, r * 0.6f, mix(GFX_RGB(255, 255, 250), C_HAZE, fog / 3));
}

static void draw_nuke(float dist) {
    int fog = fog_amount(dist * 0.2f);
    if (fog > 200)
        fog = 200;
    float t = nuke.age, top = nuke_top(), ground = nuke.pos.y;
    float glow = clampf(1 - t / 25, 0, 1);  // How much the cloud still glows from inside
    uint32_t smoke = mix(GFX_RGB(158, 146, 134), nuke_fire(t), (int)(glow * 200));
    uint32_t dust = GFX_RGB(150, 132, 108);

    // The ring of dust along the ground
    if (t > 1.0f)
        draw_column(v3(nuke.pos.x, ground + 90, nuke.pos.z), 3800 * clampf((t - 1) / 20, 0.05f, 1), 90 + 60 * clampf(t / 20, 0, 1),
                    dust, fog);
    // The cap and its dome
    float cap = NUKE_BALL * 1.05f + (NUKE_CAP - NUKE_BALL) * clampf((t - 2) / 30, 0, 1), hh = cap * 0.3f;
    // The stem, up into the cap
    if (t > 1.5f) {
        float sr = 220 + 260 * clampf(t / 25, 0, 1), sh = (top - hh - ground) * 0.5f;
        if (sh > 0)
            draw_column(v3(nuke.pos.x, ground + sh, nuke.pos.z), sr, sh, mix(dust, smoke, 128), fog);
    }
    if (t > 1.5f) {
        draw_column(v3(nuke.pos.x, top, nuke.pos.z), cap, hh, smoke, fog);
        draw_column(v3(nuke.pos.x, top + hh * 1.5f, nuke.pos.z), cap * 0.62f, hh * 0.5f, shade(smoke, 270), fog);
        // Its underside, still lit by the fire
        if (glow > 0)
            draw_column(v3(nuke.pos.x, top - hh * 1.15f, nuke.pos.z), cap * 0.7f, hh * 0.15f, nuke_fire(t * 0.6f), fog);
    }
    draw_nuke_ball(fog);
}

// The flash: everything washed white, fading
static void nuke_flash(void) {
    if (!nuke.active || nuke.age > 1.2f)
        return;
    int k = (int)((1 - nuke.age / 1.2f) * 256);
    vec3 d = vsub(nuke.pos, cam_pos);
    float dist = vlen(d);
    if (dist > 40000)
        return;
    if (dist > 20000)
        k = (int)((float)k * 20000 / dist);
    size_t n = (size_t)rw * rh;
    for (size_t i = 0; i < n; i++)
        scene[i] = mix(scene[i], GFX_RGB(255, 255, 250), k);
}

#define TOWER_A         420.0f      // A base's control tower: where (base_buildings[3]) and how tall its shaft
#define TOWER_C         230.0f
#define TOWER_H         32.0f

static void add_buildings(void) {
    float range = BASE_BUILDING_RANGE + 1000.0f;
    base_t near[MAX_NEAR];
    int count = bases_near(cam_pos.x, cam_pos.z, range, near, MAX_NEAR);
    for (int n = 0; n < count; n++) {
        const base_t b = near[n];
        if (vlen(vsub(v3(b.x, 0, b.z), cam_pos)) > range || !in_view(v3(b.x, 0, b.z), RUNWAY_HALF_LEN + 500))
            continue;
        for (size_t k = 0; k < COUNT(base_buildings); k++) {
            const building_t* bl = &base_buildings[k];
            add_box(base_point(&b, bl->a, bl->c, bl->height * 0.5f), vscale(b.along, bl->half_a),
                    v3(0, bl->height * 0.5f, 0), vscale(b.across, bl->half_c), bl->color);
        }
        // The control tower's cab: tinted glass all round, a roof, an antenna
        add_box(base_point(&b, TOWER_A, TOWER_C, TOWER_H + 0.6f), vscale(b.along, 5.5f), v3(0, 0.6f, 0),
                vscale(b.across, 5.5f), GFX_RGB(196, 190, 176));
        add_box(base_point(&b, TOWER_A, TOWER_C, TOWER_H + 3.4f), vscale(b.along, 6.2f), v3(0, 2.2f, 0),
                vscale(b.across, 6.2f), GFX_RGB(52, 84, 104));
        add_box(base_point(&b, TOWER_A, TOWER_C, TOWER_H + 6.0f), vscale(b.along, 6.8f), v3(0, 0.4f, 0),
                vscale(b.across, 6.8f), GFX_RGB(90, 90, 92));
        add_box(base_point(&b, TOWER_A, TOWER_C, TOWER_H + 10.4f), vscale(b.along, 0.3f), v3(0, 4, 0),
                vscale(b.across, 0.3f), GFX_RGB(200, 60, 50));
    }

    // A map's buildings: cities, targets and ones of their own
    for (int i = 0; i < map.building_count; i++) {
        const map_building_t* m = &map.buildings[i];
        float dx = m->x - cam_pos.x, dz = m->z - cam_pos.z;
        float range = m->target >= 0 ? TARGET_DRAW_RANGE : BUILDING_RANGE;
        if (dx * dx + dz * dz > range * range)
            continue;
        if (m->target >= 0 && map.targets[m->target].destroyed) {
            // Rubble
            if (m->round)
                add_tank(m->x, m->z, m->half_w, 2, C_RUBBLE);
            else
                add_box(v3(m->x, 1, m->z), vscale(m->ax, m->half_w), v3(0, 1, 0), vscale(m->az, m->half_d), C_RUBBLE);
            continue;
        }
        if (m->round)
            add_tank(m->x, m->z, m->half_w, m->height, m->color);
        else
            add_box(v3(m->x, m->height * 0.5f, m->z), vscale(m->ax, m->half_w), v3(0, m->height * 0.5f, 0),
                    vscale(m->az, m->half_d), m->color);
    }

    // Villages and farms
    country_site_t sites[64];
    count = country_sites_near(cam_pos.x, cam_pos.z, COUNTRY_DRAW_RANGE, sites, (int)COUNT(sites));
    for (int n = 0; n < count; n++) {
        vec3 c = v3(sites[n].x, 0, sites[n].z);
        vec3 d = vsub(c, cam_pos);
        if (d.x * d.x + d.z * d.z > COUNTRY_DRAW_RANGE * COUNTRY_DRAW_RANGE || !in_view(c, 320))
            continue;
        map_building_t b[24];
        int k = country_buildings(&sites[n], b, (int)COUNT(b));
        for (int i = 0; i < k; i++)
            if (b[i].round)
                add_tank(b[i].x, b[i].z, b[i].half_w, b[i].height, b[i].color);
            else
                add_box(v3(b[i].x, b[i].height * 0.5f, b[i].z), vscale(b[i].ax, b[i].half_w), v3(0, b[i].height * 0.5f, 0),
                        vscale(b[i].az, b[i].half_d), b[i].color);
    }
}

// Farm fields
static void draw_country_ground(void) {
    country_site_t sites[96];
    int count = country_sites_near(cam_pos.x, cam_pos.z, COUNTRY_FIELD_RANGE, sites, (int)COUNT(sites));
    for (int n = 0; n < count; n++) {
        vec3 c = v3(sites[n].x, 0, sites[n].z);
        vec3 d = vsub(c, cam_pos);
        float dist2 = d.x * d.x + d.z * d.z;
        if (sites[n].kind != COUNTRY_FARM || dist2 > COUNTRY_FIELD_RANGE * COUNTRY_FIELD_RANGE || !in_view(c, 600))
            continue;
        country_field_t f[4];
        int k = country_fields(&sites[n], f, (int)COUNT(f));
        int fog = fog_amount(sqrtf(dist2));
        for (int i = 0; i < k; i++)
            draw_poly(f[i].corner, 4, mix(f[i].color, C_HAZE, fog));
    }
}

static void add_sam_sites(void) {
    sam_site_t near[MAX_NEAR];
    int count = sams_near(cam_pos.x, cam_pos.z, SAM_DRAW_RANGE, near, MAX_NEAR);
    for (int k = 0; k < count; k++) {
        const sam_site_t s = near[k];
        if (sam_destroyed(s.ci, s.cj))
            continue;               // Nothing left standing
        if (vlen(vsub(v3(s.x, 0, s.z), cam_pos)) > SAM_DRAW_RANGE || !in_view(v3(s.x, 5, s.z), SAM_RING + 15))
            continue;
        uint32_t body = s.enemy ? C_SAM_ENEMY : C_SAM_FRIEND;

        // Radar: a cabin, a mast and a turning dish
        float a = s.angle, c = cosf(a), si = sin_f(a);
        add_box(v3(s.x, 2, s.z), v3(c * 4, 0, si * 4), v3(0, 2, 0), v3(-si * 3, 0, c * 3), body);
        add_box(v3(s.x, 7, s.z), v3(0.4f, 0, 0), v3(0, 3, 0), v3(0, 0, 0.4f), GFX_RGB(90, 90, 90));
        float spin = world_time * 1.3f + s.angle, sc = cosf(spin), ss = sin_f(spin);
        add_box(v3(s.x, 11.5f, s.z), v3(sc * 6, 0, ss * 6), v3(0, 2.2f, 0), v3(-ss * 0.3f, 0, sc * 0.3f),
                GFX_RGB(190, 190, 184));

        // Launchers: a trailer each, with a rail and maybe a missile on it
        uint8_t loaded = sam_loaded(&s);
        for (int n = 0; n < s.launchers; n++) {
            vec3 pos, out, side, rail;
            sam_launcher(&s, n, &pos, &out, &side, &rail);
            add_box(vadd(pos, v3(0, 1.2f, 0)), vscale(out, 3), v3(0, 1.2f, 0), vscale(side, 2), body);
            vec3 lift = vnorm(vcross(side, rail));
            bool has = loaded & (1u << n);
            add_box(vadd(vadd(pos, v3(0, 3, 0)), vscale(rail, 4)), vscale(rail, 4.5f),
                    vscale(lift, has ? 0.45f : 0.2f), vscale(side, has ? 0.45f : 0.3f),
                    has ? C_MISSILE : GFX_RGB(60, 60, 60));
        }
    }
}

// A fighter: a long box for the body, flat delta wings and tailplanes, a
// fin. As a dot when too far to make out.
static void plane_tri(vec3 p, vec3 f, vec3 r, vec3 u, const float a[3][3], uint32_t color, int fog) {
    vec3 tri[3];
    for (int k = 0; k < 3; k++)
        tri[k] = vadd(p, vadd(vscale(r, a[k][0]), vadd(vscale(u, a[k][1]), vscale(f, a[k][2]))));
    vec3 n = vnorm(vcross(vsub(tri[1], tri[0]), vsub(tri[2], tri[0])));
    if (vdot(n, vsub(cam_pos, tri[0])) < 0)
        n = vscale(n, -1);          // Both sides show
    draw_poly(tri, 3, lit(color, n, fog));
}

// An airliner (size 1: a 747, about 70 m long): a long body, swept wings
// with engines under them, tailplanes and a tall fin in its colors
static void airliner_tri(const bandit_t* b, vec3 f, vec3 r, vec3 u, const float a[3][3], uint32_t color, int fog) {
    float s[3][3];
    for (int k = 0; k < 3; k++)
        for (int c = 0; c < 3; c++)
            s[k][c] = a[k][c] * b->size;
    plane_tri(b->pos, f, r, u, (const float(*)[3])s, color, fog);
}

static void draw_airliner(const bandit_t* b, int fog) {
    vec3 f = b->fwd, u = b->up, r = vnorm(vcross(u, f));
    float z = b->size;
    uint32_t body = b->color, wing = shade(body, 200);
    uint32_t fin_color = (body & 0xFF) > 200 ? GFX_RGB(28, 58, 140) : body;
    static const float wl1[3][3] = { { -3, -1.5f, 6 }, { -32, -1, -16 }, { -3, -1.5f, -8 } };
    static const float wl2[3][3] = { { -3, -1.5f, -8 }, { -32, -1, -16 }, { -32, -1, -20 } };
    static const float wr1[3][3] = { { 3, -1.5f, 6 }, { 32, -1, -16 }, { 3, -1.5f, -8 } };
    static const float wr2[3][3] = { { 3, -1.5f, -8 }, { 32, -1, -16 }, { 32, -1, -20 } };
    static const float tl[3][3] = { { 0, 1, -24 }, { -11, 1, -33 }, { 0, 1, -34 } };
    static const float tr[3][3] = { { 0, 1, -24 }, { 11, 1, -33 }, { 0, 1, -34 } };
    static const float fin[3][3] = { { 0, 2.5f, -22 }, { 0, 2.5f, -35 }, { 0, 15, -36 } };
    static const float nose[3][3] = { { -2.4f, 0, 33 }, { 2.4f, 0, 33 }, { 0, -0.5f, 37 } };
    bool above = vdot(u, vsub(cam_pos, b->pos)) > 0;
    int engines = z >= 0.75f ? 4 : 2;
    for (int pass = 0; pass < 2; pass++) {
        if ((pass == 0) == above) {         // Wings under the body from above, over it from below
            airliner_tri(b, f, r, u, wl1, wing, fog);
            airliner_tri(b, f, r, u, wl2, wing, fog);
            airliner_tri(b, f, r, u, wr1, wing, fog);
            airliner_tri(b, f, r, u, wr2, wing, fog);
            airliner_tri(b, f, r, u, tl, wing, fog);
            airliner_tri(b, f, r, u, tr, wing, fog);
            for (int e = 0; e < engines; e++) {
                float side = (e & 1 ? 1.0f : -1.0f) * (engines == 4 && e >= 2 ? 19 : 11) * z;
                vec3 c = vadd(b->pos, vadd(vscale(r, side), vadd(vscale(u, -3.2f * z), vscale(f, (-2 - fabsf(side) * 0.5f) ))));
                draw_box(c, vscale(r, 1.3f * z), vscale(u, 1.3f * z), vscale(f, 3 * z), GFX_RGB(90, 94, 100), fog);
            }
        } else {
            draw_box(b->pos, vscale(r, 3.2f * z), vscale(u, 3.2f * z), vscale(f, 33 * z), body, fog);
            airliner_tri(b, f, r, u, nose, body, fog);
        }
    }
    airliner_tri(b, f, r, u, fin, fin_color, fog);
}

static void draw_plane(const bandit_t* b, int fog) {
    vec3 v = to_view(b->pos);
    if (v.z < NEAR_Z)
        return;
    float k = cam_fl / v.z;
    float span = b->look == LOOK_AIRLINER ? 32 * b->size : 16;
    uint32_t color = mix(b->color ? b->color : GFX_RGB(112, 120, 128), C_HAZE, fog);
    if (k * span < 3) {
        fill_circle(cam_cx + v.x * k, cam_cy - v.y * k, b->look == LOOK_AIRLINER ? 1.3f : 1.0f, color);
        return;
    }
    if (b->look == LOOK_AIRLINER) {
        draw_airliner(b, fog);
        return;
    }
    vec3 f = b->fwd, u = b->up, r = vnorm(vcross(u, f));
    uint32_t body = b->color ? b->color : b->type == 1 ? GFX_RGB(160, 164, 168) : GFX_RGB(112, 120, 128);
    static const float wing_l[3][3] = { { 0, 0, 2 }, { -6.5f, 0, -4 }, { 0, 0, -5 } };
    static const float wing_r[3][3] = { { 0, 0, 2 }, { 6.5f, 0, -4 }, { 0, 0, -5 } };
    static const float tail_l[3][3] = { { 0, 0, -5.5f }, { -3, 0, -8.5f }, { 0, 0, -8.5f } };
    static const float tail_r[3][3] = { { 0, 0, -5.5f }, { 3, 0, -8.5f }, { 0, 0, -8.5f } };
    static const float fin[3][3] = { { 0, 0.8f, -4.5f }, { 0, 0.8f, -8.5f }, { 0, 4.2f, -8.5f } };
    static const float nose[3][3] = { { -0.8f, 0, 8 }, { 0.8f, 0, 8 }, { 0, 0, 10 } };
    bool above = vdot(u, vsub(cam_pos, b->pos)) > 0;
    // Seen from above: wings under the body; from below: body under the wings
    if (above) {
        plane_tri(b->pos, f, r, u, wing_l, body, fog);
        plane_tri(b->pos, f, r, u, wing_r, body, fog);
        plane_tri(b->pos, f, r, u, tail_l, body, fog);
        plane_tri(b->pos, f, r, u, tail_r, body, fog);
    }
    draw_box(b->pos, vscale(r, 0.9f), vscale(u, 0.8f), vscale(f, 8), body, fog);
    plane_tri(b->pos, f, r, u, nose, shade(body, 200), fog);
    if (!above) {
        plane_tri(b->pos, f, r, u, wing_l, body, fog);
        plane_tri(b->pos, f, r, u, wing_r, body, fog);
        plane_tri(b->pos, f, r, u, tail_l, body, fog);
        plane_tri(b->pos, f, r, u, tail_r, body, fog);
    }
    plane_tri(b->pos, f, r, u, fin, body, fog);
}

static void add_bandits(void) {
    object_t o;
    if (wreck.active) {
        o.type = OBJ_WRECK;
        add_object(&o, wreck.pos);
    }
    if (chute.active) {
        o.type = OBJ_CHUTE;
        add_object(&o, vadd(chute.pos, v3(0, 4, 0)));
    }
    for (int i = 0; i < MAX_BANDITS; i++) {
        if (!bandits[i].active)
            continue;
        vec3 d = vsub(bandits[i].pos, cam_pos);
        if (vdot(d, d) > PLANE_DRAW_RANGE * PLANE_DRAW_RANGE || !in_view(bandits[i].pos, 12))
            continue;
        object_t o;
        o.type = OBJ_PLANE;
        o.bandit = i;
        add_object(&o, bandits[i].pos);
    }
    for (int i = 0; i < MAX_TRAFFIC; i++) {
        const bandit_t* p = &traffic[i].p;
        if (!p->active)
            continue;
        vec3 d = vsub(p->pos, cam_pos);
        if (vdot(d, d) > TRAFFIC_DRAW_RANGE * TRAFFIC_DRAW_RANGE || !in_view(p->pos, 40 * p->size + 12))
            continue;
        object_t o;
        o.type = OBJ_TRAFFIC;
        o.traffic = i;
        add_object(&o, p->pos);
    }
}

// You under the parachute (seen from behind it): a canopy of orange and
// white panels, the lines down, you hanging below
static void draw_chute(int fog) {
    vec3 p = chute.pos;
    vec3 f = v3(sin_f(chute.heading), 0, cosf(chute.heading)), r = v3(f.z, 0, -f.x), u = v3(0, 1, 0);
    uint32_t suit = GFX_RGB(84, 96, 60);
    draw_box(vadd(p, v3(0, 0.9f, 0)), vscale(r, 0.25f), vscale(u, 0.9f), vscale(f, 0.2f), suit, fog);
    if (!chute.open) {
        draw_box(vadd(p, v3(0, 0.6f, -0.3f)), vscale(r, 0.35f), vscale(u, 0.6f), vscale(f, 0.15f), GFX_RGB(60, 60, 60), fog);
        return;
    }
    if (chute.down)
        return;                     // Down: the canopy collapses
    vec3 top = vadd(p, v3(0, 1.8f, 0));
    for (int k = -3; k <= 3; k++) {
        float a = (float)k * 0.28f;
        vec3 c = vadd(p, vadd(vscale(u, 7.5f * cosf(a) + 1.8f), vscale(r, 7.5f * sin_f(a))));
        vec3 pr = vadd(vscale(r, cosf(a)), vscale(u, -sin_f(a)));     // Along the canopy at this panel
        vec3 pu = vadd(vscale(u, cosf(a)), vscale(r, sin_f(a)));
        draw_box(c, vscale(pr, 1.05f), vscale(pu, 0.12f), vscale(f, 2.4f), (k & 1) ? GFX_RGB(240, 240, 236) : GFX_RGB(240, 120, 30), fog);
        if (k == -3 || k == 3 || k == 0) {
            draw_line3d(top, vadd(c, vscale(f, 2.2f)), GFX_RGB(40, 40, 40));
            draw_line3d(top, vsub(c, vscale(f, 2.2f)), GFX_RGB(40, 40, 40));
        }
    }
}

static void add_missiles(void) {
    for (int i = 0; i < MAX_MISSILES; i++) {
        const missile_t* m = &missiles[i];
        if (!m->active)
            continue;
        vec3 side = vcross(m->dir, v3(0, 1, 0));
        side = vdot(side, side) < 1e-4f ? v3(1, 0, 0) : vnorm(side);
        vec3 up = vcross(side, m->dir);
        if (m->kind == MSL_PAVEWAY || m->kind == MSL_JDAM) {
            // A bomb: fat, dark, no flame
            float r = m->kind == MSL_JDAM ? 0.5f : 0.3f, l = m->kind == MSL_JDAM ? 2.0f : 1.7f;
            add_box(m->pos, vscale(m->dir, l), vscale(up, r), vscale(side, r), m->kind == MSL_JDAM ? GFX_RGB(90, 94, 88) : GFX_RGB(104, 110, 80));
            continue;
        }
        add_box(vsub(m->pos, vscale(m->dir, 2.5f)), vscale(m->dir, 2.5f), vscale(up, 0.3f), vscale(side, 0.3f), C_MISSILE);
    }
    for (int i = 0; i < MAX_BLASTS; i++) {
        if (!blasts[i].active)
            continue;
        object_t o;
        o.type = OBJ_BLAST;
        o.blast = i;
        add_object(&o, blasts[i].pos);
    }
}

//
// Ships
//

#define SHIP_DRAW_RANGE NEAR_OF(25000.0f)
#define C_HULL          GFX_RGB(112, 118, 124)      // Navy gray
#define C_HULL_ENEMY    GFX_RGB(96, 102, 96)
#define C_FLIGHT_DECK   GFX_RGB(62, 64, 68)
#define C_WAKE          GFX_RGB(70, 116, 162)

static vec3 ship_at(const ship_t* s, float a, float c, float y) {
    return vadd(s->pos, vadd(vscale(s->fwd, a), vadd(vscale(s->right, c), v3(0, y, 0))));
}

// An upright prism on the ship: a convex outline (ship's own a, c) from y0
// up to y1, its sides that face the camera and (from above) its top
static void ship_prism(const ship_t* s, const float (*pts)[2], int n, float y0, float y1, uint32_t side,
                       uint32_t top, bool sides, bool lid, int fog) {
    vec3 up[8], mid = v3(0, 0, 0);
    for (int k = 0; k < n; k++) {
        up[k] = ship_at(s, pts[k][0], pts[k][1], y1);
        mid = vadd(mid, up[k]);
    }
    mid = vscale(mid, 1.0f / (float)n);
    if (sides)
        for (int k = 0; k < n; k++) {
            vec3 p0 = up[k], p1 = up[(k + 1) % n];
            vec3 e = vsub(p1, p0), nrm = vnorm(v3(e.z, 0, -e.x));
            vec3 em = vscale(vadd(p0, p1), 0.5f);
            if (vdot(nrm, vsub(em, mid)) < 0)
                nrm = vscale(nrm, -1);
            if (vdot(nrm, vsub(cam_pos, em)) <= 0)
                continue;
            float dy = y1 - y0;
            vec3 quad[4] = { v3(p0.x, p0.y - dy, p0.z), v3(p1.x, p1.y - dy, p1.z), p1, p0 };
            draw_poly(quad, 4, lit(side, nrm, fog));
        }
    if (lid && cam_pos.y > s->pos.y + y1)
        draw_poly(up, n, lit(top, v3(0, 1, 0), fog));
}

// A box on the ship: center a, c, y and half sizes
typedef struct {
    float    a, c, y, ha, hc, hy;
    uint32_t color;
    int      plane;                     // A parked fighter instead (heading 1: forward, -1: aft)
} ship_part_t;

static void draw_ship_parts(const ship_t* s, ship_part_t* p, int n, int fog) {
    // Farthest first
    float d2[24];
    int order[24];
    for (int k = 0; k < n; k++) {
        vec3 d = vsub(ship_at(s, p[k].a, p[k].c, p[k].y), cam_pos);
        d2[k] = vdot(d, d);
        order[k] = k;
    }
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && d2[order[j]] > d2[order[j - 1]]; j--) {
            int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    for (int k = 0; k < n; k++) {
        const ship_part_t* q = &p[order[k]];
        vec3 c = ship_at(s, q->a, q->c, q->y);
        if (q->plane) {
            bandit_t b;
            memset(&b, 0, sizeof(b));
            b.pos = c;
            b.fwd = vscale(s->fwd, (float)q->plane);
            b.up = v3(0, 1, 0);
            b.color = q->color;
            b.look = LOOK_FIGHTER;
            draw_plane(&b, fog);
            continue;
        }
        draw_box(c, vscale(s->fwd, q->ha), v3(0, q->hy, 0), vscale(s->right, q->hc), q->color, fog);
    }
}

static int add_part(ship_part_t* p, int n, float a, float c, float y0, float la, float lc, float h, uint32_t color) {
    if (n >= 24)
        return n;
    p[n] = (ship_part_t){ a, c, y0 + h * 0.5f, la * 0.5f, lc * 0.5f, h * 0.5f, color, 0 };
    return n + 1;
}

static void draw_carrier(const ship_t* s, int index, float dist, int fog) {
    float L = ship_length(s->kind), B = ship_beam(s->kind), D = ship_deck(s->kind);
    uint32_t hull = C_HULL;
    // Hull: square stern, pointed bow, a little narrower than the deck
    const float h[5][2] = { { -L / 2 + 6, -B / 2 }, { L / 2 - 50, -B / 2 }, { L / 2 - 4, 0 }, { L / 2 - 50, B / 2 + 4 }, { -L / 2 + 6, B / 2 + 4 } };
    ship_prism(s, h, 5, 0, D - 2, hull, hull, true, false, fog);

    // Flight deck: the straight deck, the bow, the angled deck's overhang to port
    const float deck[4][2] = { { -160, -18 }, { 130, -18 }, { 130, 24 }, { -160, 24 } };
    const float bow[4][2] = { { 130, -18 }, { 166, -8 }, { 166, 10 }, { 130, 24 } };
    float ang[4][2];
    vec3 pa;
    const float st[4][2] = { { 0, -LANDING_HALF_W }, { 230, -LANDING_HALF_W }, { 230, LANDING_HALF_W }, { 0, LANDING_HALF_W } };
    for (int k = 0; k < 4; k++) {
        ship_angled_point(index, st[k][0], st[k][1], &pa);
        vec3 d = vsub(pa, s->pos);
        ang[k][0] = vdot(d, s->fwd);
        ang[k][1] = vdot(d, s->right);
    }
    uint32_t top = s->sunk ? C_SCORCHED : C_FLIGHT_DECK;
    ship_prism(s, deck, 4, D - 2, D, hull, top, true, false, fog);
    ship_prism(s, bow, 4, D - 2, D, hull, top, true, false, fog);
    ship_prism(s, (const float(*)[2])ang, 4, D - 2, D, hull, top, true, false, fog);
    ship_prism(s, deck, 4, D - 2, D, hull, top, false, true, fog);
    ship_prism(s, bow, 4, D - 2, D, hull, top, false, true, fog);
    ship_prism(s, (const float(*)[2])ang, 4, D - 2, D, hull, top, false, true, fog);

    // Markings: the landing deck's edges and centerline, the wires, the bow's line
    if (dist < 6000 && cam_pos.y > s->pos.y + D) {
        uint32_t paint = mix(GFX_RGB(236, 236, 228), C_HAZE, fog), yellow = mix(GFX_RGB(230, 200, 60), C_HAZE, fog);
        vec3 p0, p1;
        for (float t = -LANDING_HALF_W + 1; t <= LANDING_HALF_W; t += 2 * LANDING_HALF_W - 2) {
            ship_angled_point(index, 0, t, &p0);
            ship_angled_point(index, 230, t, &p1);
            p0.y += 0.1f;
            p1.y += 0.1f;
            draw_line3d(p0, p1, paint);
        }
        for (float a = 0; a < 230; a += 16) {
            ship_angled_point(index, a, 0, &p0);
            ship_angled_point(index, a + 8, 0, &p1);
            p0.y += 0.1f;
            p1.y += 0.1f;
            draw_line3d(p0, p1, paint);
        }
        for (int w = 0; w < 4; w++) {
            ship_angled_point(index, 40 + 12 * (float)w, -12, &p0);
            ship_angled_point(index, 40 + 12 * (float)w, 12, &p1);
            p0.y += 0.15f;
            p1.y += 0.15f;
            draw_line3d(p0, p1, mix(GFX_RGB(20, 20, 20), C_HAZE, fog));
        }
        ship_deck_point(index, 20, 3, &p0);
        ship_deck_point(index, 160, 3, &p1);
        p0.y += 0.1f;
        p1.y += 0.1f;
        draw_line3d(p0, p1, yellow);
    }

    // Island, its mast and radar, and fighters parked along the starboard edge
    ship_part_t p[24];
    int n = 0;
    n = add_part(p, n, 30, 21, D, 44, 9, 16, hull);
    n = add_part(p, n, 36, 21, D + 16, 22, 8, 7, shade(hull, 230));
    n = add_part(p, n, 34, 21, D + 23, 1.5f, 1.5f, 16, GFX_RGB(70, 70, 72));
    n = add_part(p, n, 34, 21, D + 33, 1.2f, 9, 2, GFX_RGB(190, 190, 186));
    if (dist < 5000 && !s->sunk) {
        for (int k = 0; k < 5; k++)
            if (n < 24) {
                p[n] = (ship_part_t){ 70 + 15 * (float)k, 14, D + 1.6f, 0, 0, 0, GFX_RGB(150, 154, 160), -1 };
                n++;
            }
        for (int k = 0; k < 3; k++)
            if (n < 24) {
                p[n] = (ship_part_t){ -30 - 16 * (float)k, 17, D + 1.6f, 0, 0, 0, GFX_RGB(150, 154, 160), 1 };
                n++;
            }
    }
    draw_ship_parts(s, p, n, fog);
}

static void draw_warship(const ship_t* s, float dist, int fog) {
    (void)dist;
    float L = ship_length(s->kind), B = ship_beam(s->kind), D = ship_deck(s->kind);
    uint32_t hull = s->enemy ? C_HULL_ENEMY : C_HULL, top = shade(hull, 210);
    uint32_t dark = GFX_RGB(52, 54, 58);
    if (s->kind == SHIP_TANKER) {
        hull = GFX_RGB(110, 40, 34);
        top = GFX_RGB(150, 60, 48);
    }
    if (s->sunk)
        top = C_SCORCHED;
    const float h[5][2] = { { -L / 2, -B / 2 }, { L / 2 - B * 1.4f, -B / 2 }, { L / 2, 0 }, { L / 2 - B * 1.4f, B / 2 }, { -L / 2, B / 2 } };
    ship_prism(s, h, 5, 0, D, hull, top, true, true, fog);

    ship_part_t p[24];
    int n = 0;
    switch (s->kind) {
        case SHIP_TANKER:
            // White house aft, a funnel, pipes down the deck
            n = add_part(p, n, -L / 2 + 22, 0, D, 22, B * 0.7f, 16, GFX_RGB(232, 230, 222));
            n = add_part(p, n, -L / 2 + 22, 0, D + 16, 12, B * 0.8f, 3, GFX_RGB(232, 230, 222));
            n = add_part(p, n, -L / 2 + 10, 0, D, 7, 6, 24, GFX_RGB(40, 40, 40));
            n = add_part(p, n, 20, 0, D, L * 0.6f, 3, 1.5f, GFX_RGB(90, 120, 90));
            n = add_part(p, n, L / 2 - 30, 0, D, 8, 6, 5, GFX_RGB(150, 60, 48));
            break;
        case SHIP_MISSILE_BOAT:
            n = add_part(p, n, 2, 0, D, 10, B * 0.6f, 3.5f, top);
            n = add_part(p, n, 3, 0, D + 3.5f, 1, 1, 5, dark);
            for (int k = -1; k <= 1; k += 2)
                n = add_part(p, n, -11, (float)k * 2.2f, D + 0.5f, 9, 1.6f, 1.6f, GFX_RGB(150, 152, 150));
            n = add_part(p, n, 13, 0, D, 2.5f, 2.5f, 1.8f, dark);
            break;
        default: {
            // Warships: a gun forward, the bridge, a mast, funnels, a hangar aft
            float u = L / 155;
            n = add_part(p, n, L / 2 - 35 * u, 0, D, 6 * u, 5 * u, 2.5f, top);
            n = add_part(p, n, L / 2 - 30 * u, 0, D + 1.5f, 7 * u, 0.6f, 0.6f, dark);
            n = add_part(p, n, 25 * u, 0, D, 26 * u, B * 0.6f, 6, top);
            n = add_part(p, n, 25 * u, 0, D + 6, 12 * u, B * 0.5f, 5, shade(top, 230));
            n = add_part(p, n, 18 * u, 0, D + 11, 1, 1, 14, dark);
            n = add_part(p, n, 18 * u, 0, D + 20, 1, 8 * u, 1, dark);
            n = add_part(p, n, -5 * u, 0, D, 9 * u, B * 0.35f, 12, dark);
            if (s->kind == SHIP_CRUISER || s->kind == SHIP_DESTROYER)
                n = add_part(p, n, -25 * u, 0, D, 8 * u, B * 0.35f, 11, dark);
            n = add_part(p, n, -45 * u, 0, D, 22 * u, B * 0.6f, 6, top);
            if (s->kind == SHIP_CRUISER)
                n = add_part(p, n, -L / 2 + 30 * u, 0, D, 6 * u, 5 * u, 2.5f, top);
            break;
        }
    }
    draw_ship_parts(s, p, n, fog);
}

static void draw_ship(int i, float dist, int fog) {
    const ship_t* s = &ships[i];
    vec3 v = to_view(s->pos);
    float L = ship_length(s->kind);
    if (v.z > NEAR_Z && cam_fl * L / v.z < 4) {
        fill_circle(cam_cx + v.x * cam_fl / v.z, cam_cy - v.y * cam_fl / v.z, 1.2f, mix(C_HULL, C_HAZE, fog));
        return;
    }
    if (s->kind == SHIP_CARRIER)
        draw_carrier(s, i, dist, fog);
    else
        draw_warship(s, dist, fog);
}

static void add_ships(void) {
    for (int i = 0; i < ship_count; i++) {
        const ship_t* s = &ships[i];
        if (!s->active)
            continue;
        float L = ship_length(s->kind);
        vec3 c = vadd(s->pos, v3(0, ship_deck(s->kind) * 0.5f, 0));
        vec3 d = vsub(c, cam_pos);
        if (vdot(d, d) > SHIP_DRAW_RANGE * SHIP_DRAW_RANGE || !in_view(c, L * 0.6f + 30))
            continue;
        object_t o;
        o.type = OBJ_SHIP;
        o.ship = i;
        add_object(&o, c);
    }
}

// Wakes: a pale stripe on the sea behind each ship under way
static void draw_wakes(void) {
    for (int i = 0; i < ship_count; i++) {
        const ship_t* s = &ships[i];
        if (!s->active || s->pos.y < -3 || s->speed < 1)
            continue;
        float L = ship_length(s->kind), B = ship_beam(s->kind), len = L * 1.5f + s->speed * 25;
        vec3 d = vsub(s->pos, cam_pos);
        float dist = sqrtf(d.x * d.x + d.z * d.z);
        if (dist > SHIP_DRAW_RANGE || !in_view(ship_at(s, -len * 0.5f, 0, 0), len * 0.6f + L))
            continue;
        vec3 quad[4] = {
            ship_at(s, -L * 0.45f, -B * 0.4f, 0), ship_at(s, -L * 0.45f, B * 0.4f, 0),
            ship_at(s, -L * 0.45f - len, B * 1.2f, 0), ship_at(s, -L * 0.45f - len, -B * 1.2f, 0),
        };
        draw_poly(quad, 4, mix(C_WAKE, C_HAZE, fog_amount(dist)));
        vec3 bow[3] = { ship_at(s, L * 0.5f + 2, 0, 0), ship_at(s, L * 0.2f, B * 1.1f, 0), ship_at(s, L * 0.2f, -B * 1.1f, 0) };
        draw_poly(bow, 3, mix(C_WAKE, C_HAZE, fog_amount(dist)));
    }
}

static void draw_objects(void) {
    object_count = 0;
    float range = FOG_FAR + 1000.0f;

    static hill_t hills[512];
    int count = hills_near(cam_pos.x, cam_pos.z, range, hills, (int)COUNT(hills));
    for (int k = 0; k < count; k++) {
        object_t o;
        o.hill = hills[k];
        vec3 center = v3(o.hill.x, o.hill.height * 0.3f, o.hill.z);
        vec3 d = vsub(center, cam_pos);
        if (vdot(d, d) > range * range ||
            !in_view(v3(o.hill.x, o.hill.height * 0.5f, o.hill.z), o.hill.half * 1.42f + o.hill.height * 0.5f))
            continue;
        o.type = OBJ_HILL;
        add_object(&o, center);
    }
    add_buildings();
    add_sam_sites();
    add_bandits();
    add_missiles();
    add_ships();
    if (nuke.active && in_view(v3(nuke.pos.x, nuke.pos.y + NUKE_TOP * 0.5f, nuke.pos.z), NUKE_TOP)) {
        object_t o;
        o.type = OBJ_NUKE;
        add_object(&o, v3(nuke.pos.x, nuke_top(), nuke.pos.z));
    }
    qsort(objects, (size_t)object_count, sizeof(object_t), farther_first);

    for (int n = 0; n < object_count; n++) {
        const object_t* o = &objects[n];
        int fog = fog_amount(sqrtf(o->dist2));
        switch (o->type) {
            case OBJ_HILL:  draw_hill(&o->hill, fog); break;
            case OBJ_BOX:   draw_box(o->box.center, o->box.ax, o->box.ay, o->box.az, o->box.color, fog); break;
            case OBJ_TANK:  draw_tank(o->box.center, o->box.ax.x, o->box.ay.y, o->box.color, fog); break;
            case OBJ_BLAST: draw_blast(&blasts[o->blast]); break;
            case OBJ_PLANE: draw_plane(&bandits[o->bandit], fog); break;
            case OBJ_WRECK: draw_plane(&wreck, fog); break;
            case OBJ_CHUTE: draw_chute(fog); break;
            case OBJ_TRAFFIC: draw_plane(&traffic[o->traffic].p, fog); break;
            case OBJ_SHIP:  draw_ship(o->ship, sqrtf(o->dist2), fog); break;
            case OBJ_NUKE:  draw_nuke(sqrtf(o->dist2)); break;
        }
    }
}

// Flares: bright balls; chaff: a glitter of foil
static void draw_decoy(const decoy_t* d, bool glow) {
    vec3 v = to_view(d->pos);
    if (v.z < NEAR_Z || v.z > FOG_FAR)
        return;
    float k = cam_fl / v.z;
    float x = cam_cx + v.x * k, y = cam_cy - v.y * k;
    if (d->kind == DECOY_FLARE) {
        fill_circle(x, y, clampf(2.5f * k, 1.2f, 8), glow ? GFX_RGB(255, 220, 140) : GFX_RGB(255, 236, 190));
        return;
    }
    if (glow)
        return;                     // Chaff doesn't shine
    float spread = 6 + d->age * 10;
    for (int i = 0; i < 24; i++) {
        uint32_t h = hash2(i, (int32_t)(d - decoys), 17);
        float ox = ((float)(h & 255) / 128.0f - 1) * spread, oy = ((float)((h >> 8) & 255) / 128.0f - 1) * spread;
        int32_t px = (int32_t)(x + ox * k), py = (int32_t)(y + oy * k * 0.6f);
        if (px >= 0 && py >= 0 && px < rw && py < rh)
            scene[(size_t)py * rw + px] = (h >> 16) & 1 ? GFX_RGB(230, 230, 230) : GFX_RGB(150, 150, 150);
    }
}

// Smoke trails and the missiles' flames, over everything else
static void draw_smoke(void) {
    for (int i = 0; i < MAX_DECOYS; i++)
        if (decoys[i].active)
            draw_decoy(&decoys[i], false);
    for (int i = 0; i < MAX_MISSILES; i++) {
        const missile_t* m = &missiles[i];
        if (m->trail_count < 1 || (!m->active && m->fade >= TRAIL_FADE))
            continue;
        int fade = m->active ? 0 : (int)(m->fade / TRAIL_FADE * 256);
        vec3 prev = m->trail[m->trail_head];
        for (int k = 1; k <= m->trail_count; k++) {
            vec3 p = k < m->trail_count ? m->trail[(m->trail_head + k) % TRAIL_LEN] : m->pos;
            if (k == m->trail_count && !m->active)
                break;
            float dist = vlen(vsub(p, cam_pos));
            int f = fog_amount(dist) + fade;
            draw_line3d(prev, p, mix(C_SMOKE, C_HAZE, f > 256 ? 256 : f));
            prev = p;
        }
        if (m->active) {
            vec3 v = to_view(m->pos);
            if (v.z > NEAR_Z) {
                float k = cam_fl / v.z;
                float r = clampf(1.2f * k, 1.2f, 6.0f);
                bool bomb = m->kind == MSL_PAVEWAY || m->kind == MSL_JDAM;
                fill_circle(cam_cx + v.x * k, cam_cy - v.y * k, bomb ? r * 0.6f : r,
                            bomb ? GFX_RGB(70, 74, 70) : m->age < MISSILE_BOOST ? C_FLAME : C_MISSILE);
            }
        }
    }
}

static void upscale_scene(void) {
    if (!chunky)
        return;
    for (int32_t y = 0; y < sh; y++) {
        const uint32_t* in = half_buf + (size_t)(y >> 1) * rw;
        uint32_t* out = frame + (size_t)y * sw;
        for (int32_t x = 0; x < sw; x++)
            out[x] = in[x >> 1];
    }
}

//
// Night: the picture darkened toward blue, with stars above, and lights
// on the ground (runway edges, windows in town), blasts and missile
// flames bright on top
//

#define NIGHT_R         0.20f       // How much of each color is left at night
#define NIGHT_G         0.24f
#define NIGHT_B         0.38f
#define NIGHT_STARS     400
#define NIGHT_LIGHTS    20000.0f    // Ground lights out to this far
#define NIGHT_STREET    3           // Street lights round each of a map's buildings
#define NIGHT_COUNTRY   12000.0f    // Village and farm windows out to this far
#define STAR            0xFFFFFFu   // A star's color (left as it is by the darkening)

bool night;

static uint8_t night_r[256], night_g[256], night_b[256];

static void draw_stars(void) {
    for (int i = 0; i < NIGHT_STARS; i++) {
        uint32_t h = hash2(i, 7, 77);
        float az = (float)(h & 0xffff) * (2.0f * (float)M_PI / 65536.0f);
        float el = (float)((h >> 16) & 0x3fff) / 16384.0f;
        el = 0.03f + el * el * 1.5f;                        // More near the horizon
        vec3 dir = v3(cosf(az) * cosf(el), sin_f(el), sin_f(az) * cosf(el));
        float z = vdot(dir, cam_f);
        if (z < 0.05f)
            continue;
        int32_t x = (int32_t)(cam_cx + vdot(dir, cam_r) * cam_fl / z);
        int32_t y = (int32_t)(cam_cy - vdot(dir, cam_u) * cam_fl / z);
        if (x >= 0 && y >= 0 && x < rw && y < rh && dir.y > 0)
            scene[(size_t)y * rw + x] = STAR;
    }
}

static void darken(void) {
    if (!night_b[255]) {
        for (int i = 0; i < 256; i++) {
            night_r[i] = (uint8_t)((float)i * NIGHT_R);
            night_g[i] = (uint8_t)((float)i * NIGHT_G);
            night_b[i] = (uint8_t)((float)i * NIGHT_B);
        }
    }
    size_t n = (size_t)rw * rh;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = scene[i];
        if ((c & 0xFFFFFFu) == STAR)
            continue;
        scene[i] = ((uint32_t)night_r[(c >> 16) & 255] << 16) | ((uint32_t)night_g[(c >> 8) & 255] << 8) |
                   night_b[c & 255];
    }
}

// A point of light, bigger close up, dimmer far away
static void light_at(vec3 p, uint32_t color) {
    vec3 v = to_view(p);
    if (v.z < NEAR_Z || v.z > NIGHT_LIGHTS)
        return;
    int32_t x = (int32_t)(cam_cx + v.x * cam_fl / v.z), y = (int32_t)(cam_cy - v.y * cam_fl / v.z);
    uint32_t c = mix(color, GFX_RGB(20, 26, 44), (int)(v.z / NIGHT_LIGHTS * 110));
    int size = v.z < 3000 ? 2 : 1;
    for (int32_t j = 0; j < size; j++)
        for (int32_t i = 0; i < size; i++)
            if (x + i >= 0 && y + j >= 0 && x + i < rw && y + j < rh)
                scene[(size_t)(y + j) * rw + x + i] = c;
}

// At night: masthead and side lights, and a carrier's deck lights
static void ship_lights(void) {
    for (int i = 0; i < ship_count; i++) {
        const ship_t* s = &ships[i];
        if (!s->active || s->sunk)
            continue;
        vec3 d = vsub(s->pos, cam_pos);
        if (d.x * d.x + d.z * d.z > NIGHT_LIGHTS * NIGHT_LIGHTS * 4)
            continue;
        float L = ship_length(s->kind), B = ship_beam(s->kind), D = ship_deck(s->kind);
        light_at(ship_at(s, L * 0.1f, 0, D + (s->kind == SHIP_CARRIER ? 45 : 20)), GFX_RGB(255, 250, 230));
        light_at(ship_at(s, -L * 0.5f, 0, D + 1), GFX_RGB(255, 250, 230));
        light_at(ship_at(s, L * 0.15f, -B * 0.5f, D + 3), GFX_RGB(255, 60, 40));
        light_at(ship_at(s, L * 0.15f, B * 0.5f, D + 3), GFX_RGB(60, 255, 100));
        if (s->kind == SHIP_CARRIER) {
            vec3 p;
            for (float a = 0; a <= 230; a += 15) {
                for (int side = -1; side <= 1; side += 2) {
                    ship_angled_point(i, a, (float)side * LANDING_HALF_W, &p);
                    light_at(vadd(p, v3(0, 0.3f, 0)), GFX_RGB(255, 240, 200));
                }
                ship_angled_point(i, a, 0, &p);
                light_at(vadd(p, v3(0, 0.3f, 0)), GFX_RGB(255, 255, 255));
            }
            // The drop lights down the stern, and the ball
            for (float y = 2; y < D; y += 4) {
                ship_angled_point(i, -2, 0, &p);
                p.y = s->pos.y + y;
                light_at(p, GFX_RGB(255, 170, 60));
            }
            ship_angled_point(i, 110, -16, &p);
            light_at(vadd(p, v3(0, 2, 0)), GFX_RGB(255, 190, 60));
        }
    }
}

static void draw_night_lights(void) {
    // Runways: white edge lights, green and red across the ends
    base_t near[MAX_NEAR];
    int count = bases_near(cam_pos.x, cam_pos.z, NIGHT_LIGHTS, near, MAX_NEAR);
    for (int n = 0; n < count; n++) {
        const base_t* b = &near[n];
        for (float a = -RUNWAY_HALF_LEN; a <= RUNWAY_HALF_LEN; a += 100) {
            light_at(base_point(b, a, -RUNWAY_HALF_W - 2, 0.5f), GFX_RGB(255, 244, 210));
            light_at(base_point(b, a, RUNWAY_HALF_W + 2, 0.5f), GFX_RGB(255, 244, 210));
        }
        for (float c = -RUNWAY_HALF_W; c <= RUNWAY_HALF_W; c += 10) {
            light_at(base_point(b, -RUNWAY_HALF_LEN - 4, c, 0.5f), GFX_RGB(80, 255, 120));
            light_at(base_point(b, RUNWAY_HALF_LEN + 4, c, 0.5f), GFX_RGB(255, 70, 50));
        }
        // The tower: its cab lit, a red light on the antenna, and the beacon
        // flashing green and white
        for (int k = 0; k < 4; k++)
            light_at(base_point(b, TOWER_A + (k & 1 ? 6.3f : -6.3f) * (k < 2), TOWER_C + (k & 1 ? 6.3f : -6.3f) * (k >= 2),
                                TOWER_H + 3.4f), GFX_RGB(150, 220, 200));
        light_at(base_point(b, TOWER_A, TOWER_C, TOWER_H + 14.6f), GFX_RGB(255, 60, 40));
        int beat = (int)(world_time * 2) % 4;
        if (beat == 0 || beat == 2)
            light_at(base_point(b, TOWER_A, TOWER_C, TOWER_H + 7), beat ? GFX_RGB(255, 255, 255) : GFX_RGB(60, 255, 110));
    }

    // Windows lit in a map's buildings (not all of them, and not rubble)
    static const uint32_t window[] = { GFX_RGB(255, 214, 140), GFX_RGB(255, 190, 100), GFX_RGB(240, 240, 220) };
    for (int i = 0; i < map.building_count; i++) {
        const map_building_t* m = &map.buildings[i];
        float dx = m->x - cam_pos.x, dz = m->z - cam_pos.z;
        if (dx * dx + dz * dz > NIGHT_LIGHTS * NIGHT_LIGHTS)
            continue;
        if (m->target >= 0 && map.targets[m->target].destroyed)
            continue;
        uint32_t h = hash2((int32_t)m->x, (int32_t)m->z, 5);
        if (h % 10 >= 3)
            light_at(v3(m->x, m->height + 0.5f, m->z), window[(h >> 8) % COUNT(window)]);
        // Street lights round it
        for (int k = 0; k < NIGHT_STREET; k++) {
            uint32_t g = hash2((int32_t)m->x + k, (int32_t)m->z, 6);
            float ox = (float)((int)(g & 255) - 128), oz = (float)((int)((g >> 8) & 255) - 128);
            light_at(v3(m->x + ox, 1, m->z + oz), GFX_RGB(255, 200, 120));
        }
    }

    // Windows in villages and farms
    country_site_t sites[64];
    int sites_n = country_sites_near(cam_pos.x, cam_pos.z, NIGHT_COUNTRY, sites, (int)COUNT(sites));
    for (int n = 0; n < sites_n; n++) {
        map_building_t b[24];
        int k = country_buildings(&sites[n], b, (int)COUNT(b));
        for (int i = 0; i < k; i++) {
            uint32_t h = hash2((int32_t)b[i].x, (int32_t)b[i].z, 7);
            if (h % 10 < 4)
                light_at(v3(b[i].x, b[i].height * 0.6f, b[i].z), window[(h >> 8) % COUNT(window)]);
        }
    }

    ship_lights();

    // What burns stays bright
    for (int i = 0; i < MAX_BLASTS; i++)
        if (blasts[i].active)
            draw_blast(&blasts[i]);
    for (int i = 0; i < MAX_MISSILES; i++)
        if (missiles[i].active && missiles[i].kind != MSL_PAVEWAY && missiles[i].kind != MSL_JDAM)
            light_at(missiles[i].pos, C_FLAME);
    if (nuke.active)
        draw_nuke_ball(0);
    for (int i = 0; i < MAX_DECOYS; i++)
        if (decoys[i].active)
            draw_decoy(&decoys[i], true);

    // Traffic: red on the left wingtip, green on the right, white on the
    // tail, and a white strobe
    bool strobe = fmodf(world_time, 1.2f) < 0.1f;
    for (int i = 0; i < MAX_TRAFFIC; i++) {
        const bandit_t* p = &traffic[i].p;
        if (!p->active)
            continue;
        float span = p->look == LOOK_AIRLINER ? 32 * p->size : 6.5f;
        float tail = p->look == LOOK_AIRLINER ? -36 * p->size : -8.5f;
        vec3 r = vnorm(vcross(p->up, p->fwd));
        light_at(vadd(p->pos, vscale(r, -span)), GFX_RGB(255, 50, 40));
        light_at(vadd(p->pos, vscale(r, span)), GFX_RGB(60, 255, 90));
        light_at(vadd(p->pos, vscale(p->fwd, tail)), GFX_RGB(255, 255, 255));
        if (strobe)
            light_at(vadd(p->pos, vscale(p->up, -3 * p->size)), GFX_RGB(255, 255, 255));
    }
}

// The 3D picture from the camera (cam_pos, cam_r, cam_u, cam_f)
static void draw_scene(void) {
    cam_fl = (float)rw * FOV_SCALE;
    cam_cx = (float)rw * 0.5f;
    cam_cy = (float)rh * 0.5f;
    view_tan_x = cam_cx / cam_fl;
    view_tan_y = cam_cy / cam_fl;
    view_sec_x = sqrtf(1 + view_tan_x * view_tan_x);
    view_sec_y = sqrtf(1 + view_tan_y * view_tan_y);

    frame_no++;
    draw_sky_ground();
    if (night)
        draw_stars();
    draw_map_ground();
    draw_wakes();
    draw_fields();
    draw_country_ground();
    draw_base_ground();
    draw_sam_pads();
    draw_objects();
    draw_smoke();
    if (night) {
        darken();
        draw_night_lights();
    }
    nuke_flash();
    upscale_scene();
}

// The frame and its text to the window
static void present(uint64_t now, bool light_text) {
    if (!fps_since)
        fps_since = now;
    int32_t w = gfx_surface_width(surf), h = gfx_surface_height(surf);
    if (w > sw) w = sw;
    if (h > sh) h = sh;
    gfx_write_pixels(surf, 0, 0, w, h, frame, sw);
    draw_texts(light_text);
    wm_window_commit(win, NULL);

    fps_frames++;
    if (now - fps_since >= 1000) {
        fps = (unsigned)(fps_frames * 1000 / (now - fps_since));
        fps_frames = 0;
        fps_since = now;
    }
}

void render(uint64_t now) {
    setup_buffers();
    if (!surf || quit)
        return;
    if (plane.ejected) {
        // From behind and above the parachute, looking at it
        vec3 back = v3(-sin_f(chute.heading), 0, -cosf(chute.heading));
        vec3 look = vadd(chute.pos, v3(0, chute.open && !chute.down ? 4.5f : 1, 0));
        cam_pos = vadd(look, vadd(vscale(back, 24), v3(0, 6, 0)));
        float ground = terrain_height(cam_pos.x, cam_pos.z) + 2;
        if (cam_pos.y < ground)
            cam_pos.y = ground;
        cam_f = vnorm(vsub(look, cam_pos));
        cam_r = vnorm(vcross(v3(0, 1, 0), cam_f));
        cam_u = vcross(cam_f, cam_r);
    } else {
        cam_pos = plane.pos;
        cam_r = plane.right;
        cam_u = plane.up;
        cam_f = plane.fwd;
    }
    draw_scene();
    draw_hud(now);
    present(now, night);
}

// After a fatal crash: nothing but black
void render_black(uint64_t now) {
    setup_buffers();
    if (!surf || quit)
        return;
    memset(frame, 0, (size_t)sw * sh * sizeof(uint32_t));
    present(now, false);
}

// The menu, over a slow fly-around of where the chosen theater starts
void render_menu(uint64_t now) {
    setup_buffers();
    if (!surf || quit)
        return;
    float cx = 0, cz = 0;
    if (map.loaded) {
        cx = map.start_x;
        cz = map.start_z;
    }
    float a = (float)(now % 125664) * 0.00005f;     // A turn in about two minutes
    cam_pos = v3(cx + cosf(a) * 3500, 500, cz + sin_f(a) * 3500);
    cam_f = vnorm(vsub(v3(cx, 0, cz), cam_pos));
    cam_f = vnorm(v3(cam_f.x, -0.12f, cam_f.z));
    cam_r = vnorm(vcross(v3(0, 1, 0), cam_f));
    cam_u = vcross(cam_f, cam_r);
    draw_scene();
    if (pilot_screen)
        pilot_draw();
    else
        menu_draw();
    present(now, false);
}
