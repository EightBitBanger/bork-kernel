/*
 * hud.c: the green HUD. Lines are drawn into the frame; text is queued and
 * drawn by gfx on the window's surface after the frame is written, so it is
 * in the desktop's font.
 *
 * Pitch ladder, boresight, heading tape (with a B toward the nearest
 * friendly base), speed and altitude boxes, the throttle slider and fuel
 * gauge, gear, brakes, engine damage and fuel warnings, the radar scope,
 * the stick, SAM site
 * markers (red diamonds for enemy, green for friendly), missile markers
 * with an arrow around the middle for ones off screen, the radar warning
 * (RADAR, LOCK, MISSILE), the sortie score and the messages.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flight.h"

//
// Layout: move HUD parts here. In pixels of the window.
//
// *_X and *_Y place a part from an edge: 0 or more counts from the left /
// top edge to the part's left / top side, less than 0 from the right /
// bottom edge to its right / bottom side (-8: 8 pixels in from the edge).
// *_DX and *_DY are offsets from the middle of the window instead.
//

#define UI_LADDER_W         60      // Pitch ladder area, percent of the window's width ...
#define UI_LADDER_H         70      // ... and height, centered
#define UI_HEADING_Y        6       // Heading tape, centered across (its numbers go above this)
#define UI_BASE_X           8       // Nearest base: distance and runway
#define UI_BASE_Y           6
#define UI_WARN_X           -8      // Radar warning (RADAR / LOCK / MISSILE) and frame rate
#define UI_WARN_Y           6
#define UI_SPEED_DX         -196    // Airspeed box: its right side, from the middle
#define UI_ALT_DX           196     // Altitude box: its left side, from the middle
#define UI_BOXES_DY         0       // Both boxes, up or down from the middle
#define UI_THROTTLE_X       10      // Throttle slider
#define UI_THROTTLE_Y       -8
#define UI_THROTTLE_W       12
#define UI_THROTTLE_H       110     // To 100%; the afterburner part goes above this
#define UI_THROTTLE_AB_H    18
#define UI_FUEL_X           44      // Fuel gauge (red when low)
#define UI_FUEL_Y           -8
#define UI_FUEL_W           12
#define UI_FUEL_H           128
#define UI_STATUS_X         70      // Gear, brakes, engine damage (lines going up from here)
#define UI_STATUS_Y         -8
#define UI_MODE_X           -8      // KEYS / MOUSE
#define UI_MODE_Y           -8
#define UI_RADAR_DX         0       // Radar scope: its middle, from the middle across ...
#define UI_RADAR_Y          -8      // ... and its bottom, up from the bottom
#define UI_RADAR_R          72      // Its radius
#define UI_RADAR_DIM        170     // How dark its background is, 0 .. 256
#define UI_MAP_DX           84      // Map: its left side, from the middle (right of the radar) ...
#define UI_MAP_Y            -8      // ... and its bottom, up from the bottom
#define UI_MAP_W            150
#define UI_MAP_H            144
#define UI_MAP_DIM          70      // How much darker than the ground's colors, 0 .. 256
#define NAV_MAP_TEXELS      640     // The map's picture: this many pixels along its long side
#define UI_STICK_DX         -104    // Stick box, from the middle ...
#define UI_STICK_Y          -8      // ... and up from the bottom
#define UI_MESSAGE_DY       28      // Warnings and ground messages, below the middle
#define UI_CRASH_DY         -60     // Crash and pause messages, above the middle
#define UI_HELP_DX          -200    // Help: its left side, from the middle ...
#define UI_HELP_Y           -170    // ... and its bottom, up from the bottom
#define UI_WEAPON_X         -8      // Missiles left (the armed kind marked), above KEYS / MOUSE
#define UI_WEAPON_Y         -30
#define UI_SEEKER_DEG       30      // Air-to-air seeker circle: degrees off the nose
#define UI_MISSILE_RING     22      // Arrows toward missiles off screen: percent of the height from the middle

static float tanf_deg(float deg) {
    float a = deg * ((float)M_PI / 180.0f);
    return sin_f(a) / cosf(a);
}

// A part `size` wide (or high) placed at `v` (see above) in a window `total` across
static int32_t ui_pos(int32_t v, int32_t size, int32_t total) {
    return v >= 0 ? v : total + v - size;
}

typedef struct { int32_t x0, y0, x1, y1; } box_t;

static box_t hud_box;               // The ladder stays inside this
static uint32_t hud_color = C_HUD;  // The HUD's lines: black by day, green at night, flashing red fast and low
static float ladder_sin[19], ladder_cos[19];    // -90 .. 90 in 10 degree steps

void hud_init(void) {
    for (int k = 0; k < 19; k++) {
        float a = (float)(k * 10 - 90) * ((float)M_PI / 180.0f);
        ladder_cos[k] = cosf(a);
        ladder_sin[k] = sin_f(a);
    }
}

static void put_pixel(int32_t x, int32_t y, uint32_t c) {
    if (x >= 0 && y >= 0 && x < sw && y < sh)
        frame[(size_t)y * sw + x] = c;
}

static void draw_line(float x0, float y0, float x1, float y1, uint32_t color, const box_t* clip) {
    // Liang-Barsky against the clip box
    float t0 = 0, t1 = 1, dx = x1 - x0, dy = y1 - y0;
    float p[4] = { -dx, dx, -dy, dy };
    float q[4] = { x0 - clip->x0, clip->x1 - 1 - x0, y0 - clip->y0, clip->y1 - 1 - y0 };
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
        put_pixel(ax, ay, color);
        if (ax == bx && ay == by)
            break;
        int32_t e2 = 2 * err;
        if (e2 >= ddy) { err += ddy; ax += sx; }
        if (e2 <= ddx) { err += ddx; ay += sy; }
    }
}

static void line(float x0, float y0, float x1, float y1, uint32_t color) {
    box_t all = { 0, 0, sw, sh };
    draw_line(x0, y0, x1, y1, color, &all);
}

void draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    line((float)x, (float)y, (float)(x + w), (float)y, color);
    line((float)(x + w), (float)y, (float)(x + w), (float)(y + h), color);
    line((float)(x + w), (float)(y + h), (float)x, (float)(y + h), color);
    line((float)x, (float)(y + h), (float)x, (float)y, color);
}

static void draw_diamond(float x, float y, float r, uint32_t color) {
    line(x, y - r, x + r, y, color);
    line(x + r, y, x, y + r, color);
    line(x, y + r, x - r, y, color);
    line(x - r, y, x, y - r, color);
}

//
// Text
//

#define MAX_TEXTS       128

typedef struct {
    int32_t  x, y;
    uint32_t color;
    char     s[96];
} hud_text_t;

static hud_text_t texts[MAX_TEXTS];
static int        text_count;

// Queue text; align -1 left, 0 centered, 1 right of x
void text_at(int32_t x, int32_t y, int align, uint32_t color, const char* fmt, ...) {
    if (text_count >= MAX_TEXTS)
        return;
    hud_text_t* t = &texts[text_count++];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t->s, sizeof(t->s), fmt, ap);
    va_end(ap);
    int32_t w = (int32_t)strlen(t->s) * gfx_char_width(surf);
    t->x = align < 0 ? x : align == 0 ? x - w / 2 : x - w;
    t->y = y;
    t->color = color;
}

void draw_texts(bool light) {
    for (int i = 0; i < text_count; i++) {
        uint32_t c = texts[i].color;
        if (light && c == C_TEXT)
            c = C_TEXT_NIGHT;
        gfx_draw_text(surf, texts[i].x + 1, texts[i].y + 1, texts[i].s, light ? C_TEXT : C_TEXT_SHADOW);
        gfx_draw_text(surf, texts[i].x, texts[i].y, texts[i].s, c);
    }
    text_count = 0;
}

//
// Projection onto the full-size frame
//

static float hud_fl(void) { return (float)sw * FOV_SCALE; }

// Where a world direction lands; false if behind
static bool project_dir(vec3 dir, float* x, float* y) {
    float z = vdot(dir, cam_f);
    if (z < 0.05f)
        return false;
    *x = (float)sw * 0.5f + vdot(dir, cam_r) * hud_fl() / z;
    *y = (float)sh * 0.5f - vdot(dir, cam_u) * hud_fl() / z;
    return true;
}

// Where a world point lands; false if behind or off the frame
static bool project_point(vec3 p, float* x, float* y) {
    return project_dir(vsub(p, cam_pos), x, y) && *x >= 0 && *y >= 0 && *x < sw && *y < sh;
}

//
// Parts of the HUD
//

static void draw_ladder(void) {
    // Heading in the horizontal plane (from the up vector when looking straight up or down)
    vec3 hf = v3(cam_f.x, 0, cam_f.z);
    if (vdot(hf, hf) < 1e-4f)
        hf = cam_f.y > 0 ? v3(-cam_u.x, 0, -cam_u.z) : v3(cam_u.x, 0, cam_u.z);
    hf = vnorm(hf);
    vec3 hr = v3(hf.z, 0, -hf.x);
    int32_t lh = gfx_line_height(surf);

    for (int k = 1; k < 18; k++) {
        int deg = k * 10 - 90;
        vec3 dir = v3(hf.x * ladder_cos[k], ladder_sin[k], hf.z * ladder_cos[k]);
        float outer = deg == 0 ? 0.45f : 0.12f, inner = deg == 0 ? 0.06f : 0.04f;
        float ax, ay, bx, by, cx, cy, dx, dy;
        if (!project_dir(vadd(dir, vscale(hr, -outer)), &ax, &ay) ||
            !project_dir(vadd(dir, vscale(hr, -inner)), &bx, &by) ||
            !project_dir(vadd(dir, vscale(hr, inner)), &cx, &cy) ||
            !project_dir(vadd(dir, vscale(hr, outer)), &dx, &dy))
            continue;
        if (deg < 0) {
            // Below the horizon: dashed
            for (int s = 0; s < 3; s++) {
                float t0 = s / 3.0f, t1 = t0 + 0.2f;
                draw_line(ax + (bx - ax) * t0, ay + (by - ay) * t0, ax + (bx - ax) * t1, ay + (by - ay) * t1, hud_color, &hud_box);
                draw_line(dx + (cx - dx) * t0, dy + (cy - dy) * t0, dx + (cx - dx) * t1, dy + (cy - dy) * t1, hud_color, &hud_box);
            }
        } else {
            draw_line(ax, ay, bx, by, hud_color, &hud_box);
            draw_line(cx, cy, dx, dy, hud_color, &hud_box);
        }
        if (deg != 0) {
            // Little end ticks pointing toward the horizon, and the number
            float tx = -(dy - ay) * 0.08f * (deg > 0 ? 1 : -1), ty = (dx - ax) * 0.08f * (deg > 0 ? 1 : -1);
            draw_line(ax, ay, ax + tx, ay + ty, hud_color, &hud_box);
            draw_line(dx, dy, dx + tx, dy + ty, hud_color, &hud_box);
            int32_t lx = (int32_t)dx + 6, ly = (int32_t)dy - lh / 2;
            if (lx > hud_box.x0 && lx < hud_box.x1 - 16 && ly > hud_box.y0 && ly < hud_box.y1 - lh)
                text_at(lx, ly, -1, C_TEXT, "%d", abs(deg));
        }
    }
}

// Heading tape across the top, with a "B" over the nearest base's bearing
// (or an arrow at the end of the tape toward it)
static void draw_heading_tape(float heading_deg, float base_bearing, bool have_base, int32_t top) {
    int32_t cx = sw / 2, lh = gfx_line_height(surf);
    float px_per_deg = (float)sw / 140.0f;
    box_t clip = { cx - (int32_t)(30 * px_per_deg), top, cx + (int32_t)(30 * px_per_deg) + 1, top + 16 };
    int first = (int)floorf((heading_deg - 30) / 5) * 5;
    for (int d = first; d <= heading_deg + 30; d += 5) {
        float x = (float)cx + ((float)d - heading_deg) * px_per_deg;
        bool major = d % 10 == 0;
        draw_line(x, (float)top + 12, x, (float)top + (major ? 4 : 8), hud_color, &clip);
        int label = ((d % 360) + 360) % 360;
        if (major && x > clip.x0 + 8 && x < clip.x1 - 8) {
            const char* name = label == 0 ? "N" : label == 90 ? "E" : label == 180 ? "S" : label == 270 ? "W" : NULL;
            if (name)
                text_at((int32_t)x, top - lh, 0, C_TEXT, "%s", name);
            else
                text_at((int32_t)x, top - lh, 0, C_TEXT, "%02d", label / 10);
        }
    }
    line((float)cx, (float)top + 13, (float)cx - 4, (float)top + 18, hud_color);
    line((float)cx, (float)top + 13, (float)cx + 4, (float)top + 18, hud_color);

    if (have_base) {
        float off = base_bearing - heading_deg;
        while (off > 180) off -= 360;
        while (off < -180) off += 360;
        int32_t by = top + 20;
        if (off > -30 && off < 30) {
            text_at(cx + (int32_t)(off * px_per_deg), by, 0, C_TEXT, "B");
        } else {
            int32_t x = off < 0 ? clip.x0 : clip.x1;
            text_at(x, by, off < 0 ? -1 : 1, C_TEXT, off < 0 ? "<B" : "B>");
        }
    }
}

#define MAX_NEAR        64
#define BASE_SEARCH     60000.0f    // The nearest friendly base is looked for this far
#define TARGET_RANGE    10000.0f    // Targets are marked this close

// The nearest friendly base or carrier: distance (meters), bearing
// (degrees), runway (or landing deck) heading and name (NULL without a map)
static bool nearest_base(float* dist, float* bearing, int* runway, const char** name, bool* deck) {
    base_t near[MAX_NEAR];
    int count = bases_near(plane.pos.x, plane.pos.z, BASE_SEARCH, near, MAX_NEAR);
    float best = -1;
    for (int k = 0; k < count; k++) {
        const base_t* b = &near[k];
        if (b->enemy)
            continue;
        float dx = b->x - plane.pos.x, dz = b->z - plane.pos.z;
        float d = sqrtf(dx * dx + dz * dz);
        if (best < 0 || d < best) {
            best = d;
            *dist = d;
            *bearing = atan2_f(dx, dz) * (180.0f / (float)M_PI);
            *runway = b->heading;
            *name = b->name;
            *deck = false;
        }
    }
    for (int i = 0; i < ship_count; i++) {
        const ship_t* s = &ships[i];
        if (!s->active || s->sunk || s->enemy || s->kind != SHIP_CARRIER || plane.deck == i + 1)
            continue;
        float dx = s->pos.x - plane.pos.x, dz = s->pos.z - plane.pos.z;
        float d = sqrtf(dx * dx + dz * dz);
        if (d < BASE_SEARCH && (best < 0 || d < best)) {
            best = d;
            *dist = d;
            *bearing = atan2_f(dx, dz) * (180.0f / (float)M_PI);
            int h = (int)((s->heading - DECK_ANGLE) * (180.0f / (float)M_PI) + 0.5f);
            *runway = ((h % 360) + 360) % 360;
            *name = s->name;
            *deck = true;
        }
    }
    if (best >= 0 && *bearing < 0)
        *bearing += 360;
    return best >= 0;
}

// Ships in view: enemy ones bracketed with their kind and distance, our
// carriers named
static void draw_ship_markers(void) {
    for (int i = 0; i < ship_count; i++) {
        const ship_t* s = &ships[i];
        float x, y;
        if (!s->active || s->sunk || !project_point(vadd(s->pos, v3(0, ship_deck(s->kind) + 10, 0)), &x, &y))
            continue;
        float dist = vlen(vsub(s->pos, cam_pos));
        if (s->enemy) {
            if (dist > 15000 || dist < 300)
                continue;
            line(x - 8, y, x - 5, y + 4, C_WARN);
            line(x - 5, y + 4, x + 5, y + 4, C_WARN);
            line(x + 5, y + 4, x + 8, y, C_WARN);
            line(x - 8, y, x + 8, y, C_WARN);
            text_at((int32_t)x, (int32_t)y + 7, 0, C_TEXT_ALERT, "%s %d.%d", s->name, (int)(dist / 1000), (int)(dist / 100) % 10);
        } else if (s->kind == SHIP_CARRIER && dist < 25000 && dist > 600 && plane.deck != i + 1) {
            text_at((int32_t)x, (int32_t)y - gfx_line_height(surf), 0, C_TEXT, "%s %d.%d", s->name, (int)(dist / 1000), (int)(dist / 100) % 10);
        }
    }
}

// A ship on the radar or the map: a short bar along its heading (x, y on
// screen; fx, fy its heading there)
static void ship_mark(float x, float y, float fx, float fy, const ship_t* s) {
    float l = sqrtf(fx * fx + fy * fy);
    if (l < 1e-3f) { fx = 0; fy = 1; l = 1; }
    fx /= l;
    fy /= l;
    float len = s->kind == SHIP_CARRIER || s->kind == SHIP_TANKER ? 4.5f : s->kind == SHIP_MISSILE_BOAT ? 1.5f : 3;
    uint32_t c = s->enemy ? C_ENEMY : C_FRIEND;
    line(x - fx * len, y + fy * len, x + fx * len, y - fy * len, c);
    line(x - fx * len - fy, y + fy * len - fx, x + fx * len - fy, y - fy * len - fx, c);
}

// Diamonds over SAM sites in view: red for enemy ones (with the distance),
// green for friendly ones
static void draw_sam_markers(void) {
    sam_site_t near[MAX_NEAR];
    int count = sams_near(cam_pos.x, cam_pos.z, RADAR_RANGE * 1.5f, near, MAX_NEAR);
    for (int k = 0; k < count; k++) {
        const sam_site_t* s = &near[k];
        float x, y;
        float dist = vlen(vsub(v3(s->x, 0, s->z), cam_pos));
        if (dist > RADAR_RANGE * 1.5f || dist < 200 || !project_point(v3(s->x, 10, s->z), &x, &y))
            continue;
        uint32_t color = s->enemy ? C_WARN : hud_color;
        draw_diamond(x, y, 5, color);
        const char* kind = s->launchers ? "SAM" : "RDR";
        if (s->enemy && s->name)
            text_at((int32_t)x, (int32_t)y + 7, 0, C_TEXT_ALERT, "%s %s %d.%d", kind, s->name, (int)(dist / 1000), (int)(dist / 100) % 10);
        else if (s->enemy)
            text_at((int32_t)x, (int32_t)y + 7, 0, C_TEXT_ALERT, "%s %d.%d", kind, (int)(dist / 1000), (int)(dist / 100) % 10);
        else
            text_at((int32_t)x, (int32_t)y + 7, 0, C_TEXT, "%s", "OK");
    }
}

// A map's targets in view and in range: a square, the name and distance
static void draw_target_markers(void) {
    for (int i = 0; i < map.target_count; i++) {
        const map_target_t* t = &map.targets[i];
        float x, y;
        float dist = vlen(vsub(v3(t->x, 0, t->z), cam_pos));
        if (dist > TARGET_RANGE || dist < 150 || !project_point(v3(t->x, 15, t->z), &x, &y))
            continue;
        draw_rect((int32_t)x - 4, (int32_t)y - 4, 8, 8, t->enemy ? C_WARN : hud_color);
        text_at((int32_t)x, (int32_t)y + 7, 0, t->enemy ? C_TEXT_ALERT : C_TEXT, "%s %d.%d",
                t->name, (int)(dist / 1000), (int)(dist / 100) % 10);
    }
}

// Missiles: a box around those in view, an arrow around the middle toward
// those that aren't
static uint32_t missile_color(int kind) {
    return player_missile(kind) ? C_OWN_MISSILE : heat_seeker(kind) ? C_HEAT : C_MISSILE;
}

static void draw_missile_markers(void) {
    float cx = (float)sw * 0.5f, cy = (float)sh * 0.5f, ring = (float)sh * (UI_MISSILE_RING / 100.0f);
    for (int i = 0; i < MAX_MISSILES; i++) {
        const missile_t* m = &missiles[i];
        if (!m->active || player_missile(m->kind))
            continue;               // Only what is coming for us
        float x, y;
        uint32_t c = heat_seeker(m->kind) ? C_HEAT : C_WARN;
        if (project_point(m->pos, &x, &y)) {
            draw_rect((int32_t)x - 5, (int32_t)y - 5, 10, 10, c);
            continue;
        }
        vec3 d = vsub(m->pos, cam_pos);
        float lx = vdot(d, cam_r), ly = vdot(d, cam_u), l = sqrtf(lx * lx + ly * ly);
        if (l < 1e-3f) { lx = 0; ly = -1; l = 1; }
        lx /= l;
        ly /= l;
        float px = cx + lx * ring, py = cy - ly * ring;     // Arrow tip on the ring
        float bx = px - lx * 12, by = py + ly * 12;         // Its base, toward the middle
        float nx = -ly * 6, ny = -lx * 6;
        line(px, py, bx + nx, by + ny, c);
        line(px, py, bx - nx, by - ny, c);
        line(bx + nx, by + ny, bx - nx, by - ny, c);
    }
}

void fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color, int every) {
    for (int32_t j = y < 0 ? 0 : y; j < y + h && j < sh; j++) {
        if (every > 1 && (j - y) % every)
            continue;                   // Hatched
        for (int32_t i = x < 0 ? 0 : x; i < x + w && i < sw; i++)
            frame[(size_t)j * sw + i] = color;
    }
}

// The throttle: a bar that fills up with + and down with -, the
// afterburner's part above it, and what a damaged engine can't give hatched red
static void draw_throttle(void) {
    int32_t lh = gfx_line_height(surf);
    int32_t w = UI_THROTTLE_W, h = UI_THROTTLE_H, ab = UI_THROTTLE_AB_H;
    int32_t x = ui_pos(UI_THROTTLE_X, w, sw);
    int32_t top = ui_pos(UI_THROTTLE_Y, h + ab, sh);      // Top of the afterburner part
    int32_t y100 = top + ab, bottom = y100 + h;            // 100% line and 0%

    int limit = throttle_limit();
    int shown = plane.throttle < limit ? plane.throttle : limit;
    int32_t fill = h * shown / 100;
    fill_rect(x + 1, bottom - fill, w - 1, fill, C_HUD, 1);
    if (plane.afterburner && afterburner_ok())
        fill_rect(x + 1, top + 1, w - 1, ab - 1, C_WARN, 1);
    if (limit < 100) {
        int32_t dead = h * (100 - limit) / 100;
        fill_rect(x + 1, y100, w - 1, dead, C_WARN, 3);
    }
    if (!afterburner_ok())
        fill_rect(x + 1, top + 1, w - 1, ab - 1, C_WARN, 3);

    draw_rect(x, top, w, h + ab, C_HUD);
    line((float)x, (float)y100, (float)(x + w), (float)y100, C_HUD);
    for (int q = 1; q < 4; q++) {
        float ty = (float)(bottom - h * q / 4);
        line((float)(x + w), ty, (float)(x + w + 3), ty, C_HUD);
    }
    text_at(x + w / 2, top - lh - 2, 0, C_TEXT, "THR");
    if (plane.afterburner)
        text_at(x + w / 2, top - lh * 2 - 2, 0, C_TEXT_ALERT, "AB");
}


// The fuel gauge: a bar that empties as fuel burns, red when low
static void draw_fuel(void) {
    int32_t lh = gfx_line_height(surf);
    int32_t w = UI_FUEL_W, h = UI_FUEL_H;
    int32_t x = ui_pos(UI_FUEL_X, w, sw), top = ui_pos(UI_FUEL_Y, h, sh), bottom = top + h;
    int32_t fill = (int32_t)((float)h * plane.fuel / 100.0f + 0.5f);
    fill_rect(x + 1, bottom - fill, w - 1, fill, plane.fuel < FUEL_LOW ? C_WARN : C_HUD, 1);
    draw_rect(x, top, w, h, C_HUD);
    for (int q = 1; q < 4; q++) {
        float ty = (float)(bottom - h * q / 4);
        line((float)(x + w), ty, (float)(x + w + 3), ty, C_HUD);
    }
    text_at(x + w / 2, top - lh - 2, 0, plane.fuel < FUEL_LOW ? C_TEXT_ALERT : C_TEXT, "FUEL");
}

//
// Radar scope: heading up, the aircraft in the middle. Bases (with their
// runways), SAM sites and missiles in range, friendly green, enemy red.
//

static const float radar_ranges[] = { 10000, 20000, 40000 };
static int radar_range = 1;        // Starts at 20 km

void radar_zoom(void) {
    // Far, middle, close, then far again
    radar_range = radar_range > 0 ? radar_range - 1 : (int)COUNT(radar_ranges) - 1;
}

typedef struct {
    int32_t cx, cy, r;
    float   scale;          // Pixels a meter
    vec3    hf, hr;         // Heading, flat: up and right on the scope
} scope_t;

// Where a ground point is on the scope; false if outside it (or within `margin` of the edge)
static bool scope_point(const scope_t* sc, float wx, float wz, float margin, float* x, float* y) {
    float dx = wx - plane.pos.x, dz = wz - plane.pos.z;
    float rx = (dx * sc->hr.x + dz * sc->hr.z) * sc->scale;
    float ry = (dx * sc->hf.x + dz * sc->hf.z) * sc->scale;
    float lim = (float)sc->r - margin;
    if (rx * rx + ry * ry > lim * lim)
        return false;
    *x = (float)sc->cx + rx;
    *y = (float)sc->cy - ry;
    return true;
}

static void draw_circle(int32_t cx, int32_t cy, int32_t r, uint32_t color, int every) {
    int steps = r * 2;
    for (int k = 0; k < steps; k += every > 1 ? every : 1) {
        float a = (float)k * (2.0f * (float)M_PI) / (float)steps;
        put_pixel(cx + (int32_t)(cosf(a) * (float)r + 0.5f), cy + (int32_t)(sin_f(a) * (float)r + 0.5f), color);
    }
}

static void draw_radar(void) {
    int32_t lh = gfx_line_height(surf);
    scope_t sc;
    sc.r = UI_RADAR_R;
    sc.cx = sw / 2 + UI_RADAR_DX;
    sc.cy = ui_pos(UI_RADAR_Y, sc.r * 2, sh) + sc.r;
    float range = radar_ranges[radar_range];
    sc.scale = (float)sc.r / range;
    sc.hf = v3(plane.fwd.x, 0, plane.fwd.z);
    if (vdot(sc.hf, sc.hf) < 1e-4f)
        sc.hf = plane.fwd.y > 0 ? v3(-plane.up.x, 0, -plane.up.z) : v3(plane.up.x, 0, plane.up.z);
    sc.hf = vnorm(sc.hf);
    sc.hr = v3(sc.hf.z, 0, -sc.hf.x);

    // Darkened disc
    for (int32_t j = -sc.r; j <= sc.r; j++) {
        int32_t y = sc.cy + j;
        if (y < 0 || y >= sh)
            continue;
        int32_t half = (int32_t)sqrtf((float)(sc.r * sc.r - j * j));
        for (int32_t i = -half; i <= half; i++) {
            int32_t x = sc.cx + i;
            if (x >= 0 && x < sw)
                frame[(size_t)y * sw + x] = mix(frame[(size_t)y * sw + x], C_RADAR_BG, UI_RADAR_DIM);
        }
    }
    uint32_t dim = mix(C_HUD, C_RADAR_BG, 150);
    draw_circle(sc.cx, sc.cy, sc.r / 2, dim, 3);
    line((float)sc.cx, (float)(sc.cy - sc.r + 2), (float)sc.cx, (float)(sc.cy + sc.r - 2), dim);
    line((float)(sc.cx - sc.r + 2), (float)sc.cy, (float)(sc.cx + sc.r - 2), (float)sc.cy, dim);

    // North tick on the rim
    float nx = sc.hr.z, ny = sc.hf.z;          // North (0, 0, 1) on the scope
    line((float)sc.cx + nx * (float)(sc.r - 7), (float)sc.cy - ny * (float)(sc.r - 7),
         (float)sc.cx + nx * (float)sc.r, (float)sc.cy - ny * (float)sc.r, C_HUD);
    text_at(sc.cx + (int32_t)(nx * (float)(sc.r - 14)), sc.cy - (int32_t)(ny * (float)(sc.r - 14)) - lh / 2, 0, C_HUD, "N");

    // A map's bounds
    if (map.loaded) {
        float cx[4] = { map.x0, map.x1, map.x1, map.x0 }, cz[4] = { map.z0, map.z0, map.z1, map.z1 };
        for (int k = 0; k < 4; k++) {
            float ax = (float)sc.cx, ay = (float)sc.cy, bx = ax, by = ay;
            float dx0 = cx[k] - plane.pos.x, dz0 = cz[k] - plane.pos.z;
            float dx1 = cx[(k + 1) % 4] - plane.pos.x, dz1 = cz[(k + 1) % 4] - plane.pos.z;
            ax += (dx0 * sc.hr.x + dz0 * sc.hr.z) * sc.scale;
            ay -= (dx0 * sc.hf.x + dz0 * sc.hf.z) * sc.scale;
            bx += (dx1 * sc.hr.x + dz1 * sc.hr.z) * sc.scale;
            by -= (dx1 * sc.hf.x + dz1 * sc.hf.z) * sc.scale;
            // Only inside the scope: step along and keep what is in
            int steps = 64;
            for (int t = 0; t < steps; t++) {
                float px = ax + (bx - ax) * (float)t / (float)steps, py = ay + (by - ay) * (float)t / (float)steps;
                float rx = px - (float)sc.cx, ry = py - (float)sc.cy;
                if (rx * rx + ry * ry < (float)((sc.r - 2) * (sc.r - 2)) && (t & 1))
                    put_pixel((int32_t)px, (int32_t)py, C_WARN);
            }
        }
    }

    // Air bases: a box with the runway through it
    base_t bases[MAX_NEAR];
    int count = bases_near(plane.pos.x, plane.pos.z, range, bases, MAX_NEAR);
    for (int k = 0; k < count; k++) {
        base_t b = bases[k];
        float x, y, ax, ay, ex, ey;
        if (!scope_point(&sc, b.x, b.z, 4, &x, &y))
            continue;
        uint32_t color = b.enemy ? C_ENEMY : C_FRIEND;
        float half = RUNWAY_HALF_LEN * sc.scale;
        if (half < 4)
            half = 4;
        float ux = vdot(b.along, sc.hr), uy = vdot(b.along, sc.hf);
        ax = x + ux * half; ay = y - uy * half;
        ex = x - ux * half; ey = y + uy * half;
        line(ax, ay, ex, ey, color);
        draw_rect((int32_t)x - 2, (int32_t)y - 2, 4, 4, color);
    }

    // SAM sites and radars: diamonds
    sam_site_t sites[MAX_NEAR];
    count = sams_near(plane.pos.x, plane.pos.z, range, sites, MAX_NEAR);
    for (int k = 0; k < count; k++) {
        float x, y;
        if (scope_point(&sc, sites[k].x, sites[k].z, 3, &x, &y))
            draw_diamond(x, y, 3, sites[k].enemy ? C_ENEMY : C_FRIEND);
    }

    // A map's targets: crosses
    for (int k = 0; k < map.target_count; k++) {
        const map_target_t* t = &map.targets[k];
        float x, y;
        if (!scope_point(&sc, t->x, t->z, 3, &x, &y))
            continue;
        uint32_t c = t->enemy ? C_ENEMY : C_FRIEND;
        line(x - 2, y - 2, x + 2, y + 2, c);
        line(x - 2, y + 2, x + 2, y - 2, c);
    }

    // Ships
    for (int k = 0; k < ship_count; k++) {
        const ship_t* sh = &ships[k];
        float x, y;
        if (sh->active && !sh->sunk && scope_point(&sc, sh->pos.x, sh->pos.z, 3, &x, &y))
            ship_mark(x, y, sh->fwd.x * sc.hr.x + sh->fwd.z * sc.hr.z, sh->fwd.x * sc.hf.x + sh->fwd.z * sc.hf.z, sh);
    }

    // Fighters: arrowheads pointing their way
    for (int k = 0; k < MAX_BANDITS; k++) {
        const bandit_t* b = &bandits[k];
        float x, y;
        if (!b->active || b->state == BANDIT_FALLING || !scope_point(&sc, b->pos.x, b->pos.z, 3, &x, &y))
            continue;
        float fx = b->fwd.x * sc.hr.x + b->fwd.z * sc.hr.z, fy = b->fwd.x * sc.hf.x + b->fwd.z * sc.hf.z;
        float l = sqrtf(fx * fx + fy * fy);
        if (l < 1e-3f) { fx = 0; fy = 1; l = 1; }
        fx /= l;
        fy /= l;
        line(x + fx * 4, y - fy * 4, x - fx * 3 - fy * 3, y + fy * 3 - fx * 3, C_ENEMY);
        line(x + fx * 4, y - fy * 4, x - fx * 3 + fy * 3, y + fy * 3 + fx * 3, C_ENEMY);
    }
    for (int k = 0; k < MAX_TRAFFIC; k++) {
        const bandit_t* b = &traffic[k].p;
        float x, y;
        if (!b->active || !scope_point(&sc, b->pos.x, b->pos.z, 3, &x, &y))
            continue;
        float fx = b->fwd.x * sc.hr.x + b->fwd.z * sc.hr.z, fy = b->fwd.x * sc.hf.x + b->fwd.z * sc.hf.z;
        float l = sqrtf(fx * fx + fy * fy);
        if (l < 1e-3f) { fx = 0; fy = 1; l = 1; }
        fx /= l;
        fy /= l;
        line(x + fx * 4, y - fy * 4, x - fx * 3 - fy * 3, y + fy * 3 - fx * 3, C_FRIEND);
        line(x + fx * 4, y - fy * 4, x - fx * 3 + fy * 3, y + fy * 3 + fx * 3, C_FRIEND);
    }

    // Missiles: dots, radar-guided white, heat seekers orange, yours blue
    for (int k = 0; k < MAX_MISSILES; k++) {
        float x, y;
        if (missiles[k].active && scope_point(&sc, missiles[k].pos.x, missiles[k].pos.z, 1, &x, &y))
            fill_rect((int32_t)x - 1, (int32_t)y - 1, 3, 3, missile_color(missiles[k].kind), 1);
    }

    // The aircraft
    line((float)sc.cx, (float)sc.cy - 4, (float)sc.cx - 3, (float)sc.cy + 3, C_MISSILE);
    line((float)sc.cx, (float)sc.cy - 4, (float)sc.cx + 3, (float)sc.cy + 3, C_MISSILE);

    draw_circle(sc.cx, sc.cy, sc.r, C_HUD, 1);
    text_at(sc.cx + sc.r * 7 / 10 + 4, sc.cy - sc.r * 7 / 10 - lh, -1, C_TEXT, "%d km", (int)(range / 1000));
}

//
// The map: north up, right of the radar, showing the ground around you
// much farther out. The ground's colors come from a picture of the whole
// map made once when it loads (hills lighter); bases, SAM sites, targets,
// fighters and missiles go on top like on the radar. Z and X zoom it in
// and out (out to the whole map), N turns it off and on.
//

static const float nav_ranges[] = { 25000, 50000, 100000, 0 };     // Half the width; 0: the whole map
static int nav_range = 1;               // Starts at 50 km
static bool nav_off;

// dir > 0: out (farther, up to the whole map), < 0: in (closer); shows the map if it was off
void nav_map_zoom(int dir) {
    int last = (int)COUNT(nav_ranges) - (map.loaded ? 1 : 2);
    if (nav_off)
        nav_off = false;
    else
        nav_range = nav_range + (dir > 0 ? 1 : -1);
    nav_range = nav_range < 0 ? 0 : nav_range > last ? last : nav_range;
}

void nav_map_toggle(void) {
    nav_off = !nav_off;
}

static uint32_t* nav_img;
static int       nav_w, nav_h;
static float     nav_ppm;               // Its pixels a meter
static uint32_t  nav_serial;

// Fill a map triangle into the picture
static void nav_tri(const map_tri_t* t) {
    float ax = (t->ax - map.x0) * nav_ppm, ay = (map.z1 - t->az) * nav_ppm;
    float bx = (t->bx - map.x0) * nav_ppm, by = (map.z1 - t->bz) * nav_ppm;
    float cx = (t->cx - map.x0) * nav_ppm, cy = (map.z1 - t->cz) * nav_ppm;
    float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (fabsf(area) < 1e-6f)
        return;
    int x0 = (int)((t->x0 - map.x0) * nav_ppm), x1 = (int)((t->x1 - map.x0) * nav_ppm) + 1;
    int y0 = (int)((map.z1 - t->z1) * nav_ppm), y1 = (int)((map.z1 - t->z0) * nav_ppm) + 1;
    x0 = x0 < 0 ? 0 : x0; y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > nav_w ? nav_w : x1; y1 = y1 > nav_h ? nav_h : y1;
    for (int y = y0; y < y1; y++) {
        float py = (float)y + 0.5f;
        for (int x = x0; x < x1; x++) {
            float px = (float)x + 0.5f;
            float w0 = ((bx - px) * (cy - py) - (by - py) * (cx - px)) / area;
            float w1 = ((cx - px) * (ay - py) - (cy - py) * (ax - px)) / area;
            if (w0 >= -0.01f && w1 >= -0.01f && w0 + w1 <= 1.01f)
                nav_img[y * nav_w + x] = t->color;
        }
    }
}

static void nav_build(void) {
    free(nav_img);
    nav_img = NULL;
    nav_serial = map.serial;
    float mw = map.x1 - map.x0, mh = map.z1 - map.z0;
    if (!map.loaded || mw <= 0 || mh <= 0)
        return;
    nav_ppm = (float)NAV_MAP_TEXELS / (mw > mh ? mw : mh);
    nav_w = (int)(mw * nav_ppm) + 1;
    nav_h = (int)(mh * nav_ppm) + 1;
    nav_img = malloc((size_t)nav_w * nav_h * sizeof(uint32_t));
    if (!nav_img)
        return;
    for (int i = 0; i < nav_w * nav_h; i++)
        nav_img[i] = map.ground;
    for (int i = 0; i < map.tri_count; i++)
        nav_tri(&map.tris[i]);
    // Hills: lighter the higher
    for (int y = 0; y < nav_h; y++) {
        for (int x = 0; x < nav_w; x++) {
            float h = terrain_height(map.x0 + ((float)x + 0.5f) / nav_ppm, map.z1 - ((float)y + 0.5f) / nav_ppm);
            if (h > 30) {
                int k = (int)(h * (110.0f / 2000.0f));
                nav_img[y * nav_w + x] = mix(nav_img[y * nav_w + x], GFX_RGB(235, 230, 220), k > 140 ? 140 : k);
            }
        }
    }
}

typedef struct {
    int32_t x0, y0, w, h;
    float   cx, cz;         // The world point in the middle
    float   ppm;            // Pixels a meter
} panel_t;

static bool panel_point(const panel_t* p, float wx, float wz, float margin, float* x, float* y) {
    *x = (float)p->x0 + (float)p->w * 0.5f + (wx - p->cx) * p->ppm;
    *y = (float)p->y0 + (float)p->h * 0.5f - (wz - p->cz) * p->ppm;
    return *x >= (float)p->x0 + margin && *x < (float)(p->x0 + p->w) - margin &&
           *y >= (float)p->y0 + margin && *y < (float)(p->y0 + p->h) - margin;
}

static void draw_nav_map(void) {
    if (nav_off)
        return;
    if (nav_serial != map.serial || (map.loaded && !nav_img))
        nav_build();
    if (!map.loaded && nav_ranges[nav_range] == 0)
        nav_range = 0;
    panel_t p;
    p.w = UI_MAP_W;
    p.h = UI_MAP_H;
    p.x0 = sw / 2 + UI_MAP_DX;
    p.y0 = ui_pos(UI_MAP_Y, p.h, sh);
    float half = nav_ranges[nav_range];
    if (half > 0) {
        p.cx = plane.pos.x;
        p.cz = plane.pos.z;
        p.ppm = (float)p.w * 0.5f / half;
    } else {
        p.cx = (map.x0 + map.x1) * 0.5f;
        p.cz = (map.z0 + map.z1) * 0.5f;
        float sx = (float)p.w / (map.x1 - map.x0), sz = (float)p.h / (map.z1 - map.z0);
        p.ppm = sx < sz ? sx : sz;
    }

    // The ground
    float mpp = 1.0f / p.ppm;
    uint32_t plain = map.loaded ? map.outside : GFX_RGB(96, 120, 70);
    for (int32_t j = 0; j < p.h; j++) {
        int32_t y = p.y0 + j;
        if (y < 0 || y >= sh)
            continue;
        float wz = p.cz + ((float)p.h * 0.5f - (float)j - 0.5f) * mpp;
        int ty = nav_img ? (int)((map.z1 - wz) * nav_ppm) : -1;
        for (int32_t i = 0; i < p.w; i++) {
            int32_t x = p.x0 + i;
            if (x < 0 || x >= sw)
                continue;
            uint32_t c = plain;
            if (ty >= 0 && ty < nav_h) {
                float wx = p.cx + ((float)i + 0.5f - (float)p.w * 0.5f) * mpp;
                int tx = (int)((wx - map.x0) * nav_ppm);
                if (tx >= 0 && tx < nav_w && wx >= map.x0 && wx <= map.x1 && wz >= map.z0 && wz <= map.z1)
                    c = nav_img[ty * nav_w + tx];
            }
            frame[(size_t)y * sw + x] = mix(c, C_RADAR_BG, UI_MAP_DIM);
        }
    }
    float range = half > 0 ? half * 1.5f : (map.x1 - map.x0 + map.z1 - map.z0);

    // Air bases: the runway, and a box
    base_t bases[MAX_NEAR];
    int count = bases_near(p.cx, p.cz, range, bases, MAX_NEAR);
    for (int k = 0; k < count; k++) {
        float x, y;
        if (!panel_point(&p, bases[k].x, bases[k].z, 4, &x, &y))
            continue;
        uint32_t color = bases[k].enemy ? C_ENEMY : C_FRIEND;
        float l = RUNWAY_HALF_LEN * p.ppm;
        l = l < 4 ? 4 : l;
        line(x + bases[k].along.x * l, y - bases[k].along.z * l, x - bases[k].along.x * l, y + bases[k].along.z * l, color);
        draw_rect((int32_t)x - 2, (int32_t)y - 2, 4, 4, color);
    }

    // SAM sites and radars
    sam_site_t sites[MAX_NEAR];
    count = sams_near(p.cx, p.cz, range, sites, MAX_NEAR);
    for (int k = 0; k < count; k++) {
        float x, y;
        if (!sam_destroyed(sites[k].ci, sites[k].cj) && panel_point(&p, sites[k].x, sites[k].z, 3, &x, &y))
            draw_diamond(x, y, sites[k].launchers ? 3 : 2, sites[k].enemy ? C_ENEMY : C_FRIEND);
    }

    // Targets: crosses (grey once destroyed)
    for (int k = 0; k < map.target_count; k++) {
        const map_target_t* t = &map.targets[k];
        float x, y;
        if (!panel_point(&p, t->x, t->z, 3, &x, &y))
            continue;
        uint32_t c = t->destroyed ? GFX_RGB(120, 120, 120) : t->enemy ? C_ENEMY : C_FRIEND;
        line(x - 2, y - 2, x + 2, y + 2, c);
        line(x - 2, y + 2, x + 2, y - 2, c);
    }

    // Ships
    for (int k = 0; k < ship_count; k++) {
        const ship_t* sh = &ships[k];
        float x, y;
        if (sh->active && !sh->sunk && panel_point(&p, sh->pos.x, sh->pos.z, 3, &x, &y))
            ship_mark(x, y, sh->fwd.x, sh->fwd.z, sh);
    }

    // Fighters and missiles
    for (int k = 0; k < MAX_BANDITS; k++) {
        const bandit_t* b = &bandits[k];
        float x, y;
        if (!b->active || b->state == BANDIT_FALLING || !panel_point(&p, b->pos.x, b->pos.z, 4, &x, &y))
            continue;
        float fx = b->fwd.x, fy = b->fwd.z, l = sqrtf(fx * fx + fy * fy);
        if (l < 1e-3f) { fx = 0; fy = 1; l = 1; }
        fx /= l;
        fy /= l;
        line(x + fx * 4, y - fy * 4, x - fx * 3 - fy * 3, y + fy * 3 - fx * 3, C_ENEMY);
        line(x + fx * 4, y - fy * 4, x - fx * 3 + fy * 3, y + fy * 3 + fx * 3, C_ENEMY);
    }
    for (int k = 0; k < MAX_TRAFFIC; k++) {
        const bandit_t* b = &traffic[k].p;
        float x, y;
        if (!b->active || !panel_point(&p, b->pos.x, b->pos.z, 4, &x, &y))
            continue;
        float fx = b->fwd.x, fy = b->fwd.z, l = sqrtf(fx * fx + fy * fy);
        if (l < 1e-3f) { fx = 0; fy = 1; l = 1; }
        fx /= l;
        fy /= l;
        line(x + fx * 4, y - fy * 4, x - fx * 3 - fy * 3, y + fy * 3 - fx * 3, C_FRIEND);
        line(x + fx * 4, y - fy * 4, x - fx * 3 + fy * 3, y + fy * 3 + fx * 3, C_FRIEND);
    }
    for (int k = 0; k < MAX_MISSILES; k++) {
        float x, y;
        if (missiles[k].active && panel_point(&p, missiles[k].pos.x, missiles[k].pos.z, 1, &x, &y))
            fill_rect((int32_t)x - 1, (int32_t)y - 1, 3, 3, missile_color(missiles[k].kind), 1);
    }

    // You: an arrow along your heading
    float x, y;
    if (panel_point(&p, plane.pos.x, plane.pos.z, 3, &x, &y)) {
        float fx = plane.fwd.x, fy = plane.fwd.z, l = sqrtf(fx * fx + fy * fy);
        if (l < 1e-3f) { fx = 0; fy = 1; l = 1; }
        fx /= l;
        fy /= l;
        line(x + fx * 6, y - fy * 6, x - fx * 4 - fy * 4, y + fy * 4 - fx * 4, C_MISSILE);
        line(x + fx * 6, y - fy * 6, x - fx * 4 + fy * 4, y + fy * 4 + fx * 4, C_MISSILE);
        line(x - fx * 4 - fy * 4, y + fy * 4 - fx * 4, x - fx * 2, y + fy * 2, C_MISSILE);
        line(x - fx * 4 + fy * 4, y + fy * 4 + fx * 4, x - fx * 2, y + fy * 2, C_MISSILE);
    }

    draw_rect(p.x0, p.y0, p.w, p.h, C_HUD);
    text_at(p.x0 + p.w / 2, p.y0 + 2, 0, C_HUD, "N");
    if (half > 0)
        text_at(p.x0 + 3, p.y0 + 2, -1, C_HUD, "%d km", (int)(half * 2 / 1000));
    else
        text_at(p.x0 + 3, p.y0 + 2, -1, C_HUD, "MAP");
}

// Fighters in view: brackets, with the type and distance
static void draw_bandit_markers(void) {
    for (int i = 0; i < MAX_BANDITS; i++) {
        const bandit_t* b = &bandits[i];
        float x, y;
        if (!b->active || b->state == BANDIT_FALLING || !project_point(b->pos, &x, &y))
            continue;
        float dist = vlen(vsub(b->pos, cam_pos));
        if (dist > 15000)
            continue;
        float r = 7;
        line(x - r, y - r, x - r + 3, y - r, C_WARN);
        line(x - r, y - r, x - r, y - r + 3, C_WARN);
        line(x + r, y - r, x + r - 3, y - r, C_WARN);
        line(x + r, y - r, x + r, y - r + 3, C_WARN);
        line(x - r, y + r, x - r + 3, y + r, C_WARN);
        line(x - r, y + r, x - r, y + r - 3, C_WARN);
        line(x + r, y + r, x + r - 3, y + r, C_WARN);
        line(x + r, y + r, x + r, y + r - 3, C_WARN);
        text_at((int32_t)x, (int32_t)y + 9, 0, C_TEXT_ALERT, "%s %d.%d", b->name, (int)(dist / 1000), (int)(dist / 100) % 10);
    }
}

// Friendly traffic close by: just its name and distance
static void draw_traffic_labels(void) {
    for (int i = 0; i < MAX_TRAFFIC; i++) {
        const bandit_t* b = &traffic[i].p;
        float x, y;
        if (!b->active || !project_point(b->pos, &x, &y))
            continue;
        float dist = vlen(vsub(b->pos, cam_pos));
        if (dist < 8000)
            text_at((int32_t)x, (int32_t)y + 9, 0, C_TEXT, "%s %d.%d", b->name, (int)(dist / 1000), (int)(dist / 100) % 10);
    }
}

// The armed missile's seeker (a circle for air-to-air, a box for
// air-to-ground), the lock box (blinking while it locks), what is locked,
// and how many of each are left
static void draw_weapons(bool blink) {
    int32_t lh = gfx_line_height(surf), cw = gfx_char_width(surf);
    float cx = (float)sw * 0.5f, cy = (float)sh * 0.5f;
    int type = weapons.type[weapons.armed];
    if (type < 0)
        return;
    const weapon_type_t* wt = &weapon_types[type];

    if (!plane.on_ground && !plane.crashed && weapons.left[weapons.armed] > 0) {
        float r = hud_fl() * tanf_deg(UI_SEEKER_DEG);
        if (wt->air) {
            for (int k = 0; k < 48; k += 2) {
                float a0 = (float)k * (2.0f * (float)M_PI / 48), a1 = (float)(k + 1) * (2.0f * (float)M_PI / 48);
                line(cx + cosf(a0) * r, cy + sin_f(a0) * r, cx + cosf(a1) * r, cy + sin_f(a1) * r, hud_color);
            }
        } else {
            float b = r * 0.8f;
            line(cx - b, cy - b, cx - b + 8, cy - b, hud_color); line(cx - b, cy - b, cx - b, cy - b + 8, hud_color);
            line(cx + b, cy - b, cx + b - 8, cy - b, hud_color); line(cx + b, cy - b, cx + b, cy - b + 8, hud_color);
            line(cx - b, cy + b, cx - b + 8, cy + b, hud_color); line(cx - b, cy + b, cx - b, cy + b - 8, hud_color);
            line(cx + b, cy + b, cx + b - 8, cy + b, hud_color); line(cx + b, cy + b, cx + b, cy + b - 8, hud_color);
        }
    }

    // The lock
    vec3 tp, tv;
    if (weapons.lock.kind != TGT_NONE && target_state(&weapons.lock, &tp, &tv)) {
        float x, y;
        if ((weapons.locked || blink) && project_point(tp, &x, &y)) {
            draw_rect((int32_t)x - 11, (int32_t)y - 11, 22, 22, weapons.locked ? C_WARN : hud_color);
            if (weapons.locked)
                draw_rect((int32_t)x - 12, (int32_t)y - 12, 24, 24, C_WARN);
        }
        char name[64];
        target_name(&weapons.lock, name, sizeof(name));
        float dist = vlen(vsub(tp, plane.pos));
        int32_t wx = ui_pos(UI_WARN_X, cw * 8, sw) + cw * 8, wy = ui_pos(UI_WARN_Y, lh * 2, sh) + lh * 2;
        text_at(wx, wy, 1, weapons.locked ? C_TEXT_ALERT : C_TEXT, "%s", weapons.locked ? "LOCKED" : "LOCKING");
        text_at(wx, wy + lh, 1, C_TEXT, "%s %d.%d", name, (int)(dist / 1000), (int)(dist / 100) % 10);
    }

    // What's left: the armed kind marked; a bomb's reach now
    int32_t wx = ui_pos(UI_WEAPON_X, cw * 8, sw), wy = ui_pos(UI_WEAPON_Y, lh * 2, sh);
    for (int k = 0; k < LOAD_SLOTS; k++)
        if (weapons.type[k] >= 0)
            text_at(wx, wy + lh * k, -1, weapons.armed == k ? C_TEXT_ALERT : C_TEXT, "%s%s %d",
                    weapons.armed == k ? ">" : " ", weapon_types[weapons.type[k]].tag, weapons.left[k]);
    if (wt->bomb && !plane.on_ground && !plane.crashed) {
        // Under the lock: how far a bomb would glide from here
        float reach = weapon_range(type);
        int32_t rx = ui_pos(UI_WARN_X, cw * 8, sw) + cw * 8, ry = ui_pos(UI_WARN_Y, lh * 2, sh) + lh * 4;
        if (reach > 0)
            text_at(rx, ry, 1, C_TEXT, "BOMB REACH %d km", (int)(reach / 1000));
        else
            text_at(rx, ry, 1, C_TEXT_ALERT, "TOO LOW TO BOMB");
    }
}

// Is a heat seeker among what is coming (flares), or only radar missiles (chaff)?
static bool incoming_heat(void) {
    for (int i = 0; i < MAX_MISSILES; i++) {
        const missile_t* m = &missiles[i];
        if (m->active && m->guided && !m->decoy && m->kind == MSL_ENEMY_AAM)
            return true;
    }
    return false;
}

// Bailed out: the radar, the map, where you are and how high, and what is happening
static void draw_chute_hud(bool blink) {
    int32_t cx = sw / 2, cy = sh / 2, lh = gfx_line_height(surf), cw = gfx_char_width(surf);
    draw_radar();
    draw_nav_map();
    int32_t bx = ui_pos(UI_BASE_X, cw * 13, sw), by = ui_pos(UI_BASE_Y, lh * 2, sh);
    float agl = chute.pos.y - terrain_height(chute.pos.x, chute.pos.z);
    text_at(bx, by, -1, C_TEXT, "%d ft above the ground", (int)(agl * 3.281f));
    const char* zone = map.loaded ? map_zone_at(chute.pos.x, chute.pos.z) : NULL;
    if (zone)
        text_at(bx, by + lh, -1, C_TEXT, "Over %s", zone);
    int32_t ky = cy + UI_MESSAGE_DY;
    if (chute.down) {
        text_at(cx, ky, 0, chute.safe ? C_TEXT : C_TEXT_ALERT, "%s", chute.result ? chute.result : "Down");
        text_at(cx, ky + lh, 0, C_TEXT, sortie_score > 0 ? "Enter or Esc: back to the pilots (this flight's points are lost)"
                                                          : "Enter or Esc: back to the pilots");
    } else if (!chute.open) {
        text_at(cx, ky, 0, C_TEXT_ALERT, blink ? "EJECTED" : "");
    } else {
        text_at(cx, ky, 0, C_TEXT, "Under the canopy - Left / Right to steer");
    }
    if (paused)
        text_at(cx, cy + UI_CRASH_DY, 0, C_TEXT, "PAUSED - P to go on");
}

void draw_hud(uint64_t now) {
    int32_t cx = sw / 2, cy = sh / 2;
    int32_t cw = gfx_char_width(surf), lh = gfx_line_height(surf);
    int32_t lw = sw * UI_LADDER_W / 200, lhh = sh * UI_LADDER_H / 200;
    hud_box = (box_t){ cx - lw, cy - lhh, cx + lw, cy + lhh };
    bool blink = (now / 400) & 1;
    float agl = plane.pos.y - terrain_height(plane.pos.x, plane.pos.z);
    static bool pull_up_armed;              // Off from takeoff until first above PULL_UP_ARM
    if (plane.on_ground)
        pull_up_armed = false;
    else if (agl > PULL_UP_ARM)
        pull_up_armed = true;
    bool pull_up = pull_up_armed && !plane.on_ground && !plane.crashed && !plane.gear_down &&
                   plane.speed > PULL_UP_SPEED && agl < PULL_UP_AGL;
    hud_color = pull_up && blink ? C_HUD_PULL_UP : night ? C_HUD : C_HUD_DAY;

    // A hit: the picture flashes red
    if (plane.hit_time > 0 && world_time - plane.hit_time < HIT_FLASH) {
        int t = (int)((1.0f - (world_time - plane.hit_time) / HIT_FLASH) * 140);
        for (size_t i = 0; i < (size_t)sw * sh; i++)
            frame[i] = mix(frame[i], GFX_RGB(255, 40, 20), t);
    }
    if (plane.ejected) {
        draw_chute_hud(blink);
        return;
    }

    draw_ladder();
    draw_sam_markers();
    draw_target_markers();
    draw_bandit_markers();
    draw_traffic_labels();
    draw_ship_markers();
    draw_weapons(blink);
    if (craft->chaff + craft->flares > 0) {
        char cm[32];
        snprintf(cm, sizeof(cm), "CHF %d FLR %d", weapons.chaff, weapons.flares);
        text_at(ui_pos(UI_WEAPON_X, (int32_t)strlen(cm) * cw, sw), ui_pos(UI_WEAPON_Y, lh * 2, sh) - lh, -1, C_TEXT, "%s", cm);
    }
    draw_missile_markers();

    // Boresight: a "W" where the nose points
    line((float)cx - 14, (float)cy, (float)cx - 7, (float)cy, hud_color);
    line((float)cx - 7, (float)cy, (float)cx - 3, (float)cy + 5, hud_color);
    line((float)cx - 3, (float)cy + 5, (float)cx, (float)cy, hud_color);
    line((float)cx, (float)cy, (float)cx + 3, (float)cy + 5, hud_color);
    line((float)cx + 3, (float)cy + 5, (float)cx + 7, (float)cy, hud_color);
    line((float)cx + 7, (float)cy, (float)cx + 14, (float)cy, hud_color);

    // Heading, and the way to the nearest base
    float heading = atan2_f(cam_f.x, cam_f.z) * (180.0f / (float)M_PI);
    if (heading < 0)
        heading += 360;
    float base_dist = 0, base_bearing = 0;
    int runway = 0;
    const char* base_name = NULL;
    bool base_deck = false;
    bool have_base = nearest_base(&base_dist, &base_bearing, &runway, &base_name, &base_deck);
    draw_heading_tape(heading, base_bearing, have_base, ui_pos(UI_HEADING_Y, 20, sh) + lh);
    int32_t zy = ui_pos(UI_BASE_Y, lh * 3, sh) + lh * 2;
    if (have_base) {
        int r1 = runway / 10 ? runway / 10 : 36, r2 = (runway + 180) / 10;
        int32_t bx = ui_pos(UI_BASE_X, cw * 13, sw), by = ui_pos(UI_BASE_Y, lh * 2, sh);
        text_at(bx, by, -1, C_TEXT, "%s %d.%d km", base_name ? base_name : "BASE",
                (int)(base_dist / 1000), (int)(base_dist / 100) % 10);
        if (base_deck)
            text_at(bx, by + lh, -1, C_TEXT, "DECK %02d", (runway + 5) / 10 % 36 ? (runway + 5) / 10 % 36 : 36);
        else
            text_at(bx, by + lh, -1, C_TEXT, "RWY %02d/%02d", r1 < r2 ? r1 : r2, r1 < r2 ? r2 : r1);
        zy = by + lh * 2;
    }

    // Where we are on a map: its zone, or past its edge
    if (map.loaded) {
        int32_t zx = ui_pos(UI_BASE_X, cw * 13, sw);
        const char* zone = map_zone_at(plane.pos.x, plane.pos.z);
        if (!map_inside(plane.pos.x, plane.pos.z))
            text_at(zx, zy, -1, C_TEXT_ALERT, "OFF THE MAP");
        else if (zone)
            text_at(zx, zy, -1, C_TEXT, "Over %s", zone);
        zy += lh;
        if (map.problems && world_time < 20)
            text_at(cx, cy + UI_CRASH_DY - lh * 4, 0, C_TEXT_ALERT, "MAP: %s%s", map.problem,
                    map.problems > 1 ? " (and more)" : "");
    }

    // This flight's score, not the pilot's until it lands
    text_at(ui_pos(UI_BASE_X, cw * 13, sw), zy, -1, sortie_score < 0 ? C_TEXT_ALERT : C_TEXT, "SORTIE %d", sortie_score);

    // Radar warning (and the frame rate above it)
    int32_t wx = ui_pos(UI_WARN_X, cw * 8, sw) + cw * 8, wy = ui_pos(UI_WARN_Y, lh * 2, sh);
    if (show_fps) {
        text_at(wx, wy, 1, C_TEXT, "%u FPS", fps);
        wy += lh;
    }
    if (!sams_enabled)
        text_at(wx, wy, 1, C_TEXT, "ENEMIES OFF");
    else if (sam_threat == THREAT_MISSILE && blink)
        text_at(wx, wy, 1, C_TEXT_ALERT, "%s", incoming_heat() ? "HEAT MISSILE" : "RADAR MISSILE");
    else if (sam_threat == THREAT_LOCK)
        text_at(wx, wy, 1, C_TEXT_ALERT, "LOCK");
    else if (sam_threat == THREAT_RADAR)
        text_at(wx, wy, 1, C_TEXT_ALERT, "RADAR");
    else if (bandit_attacking())
        text_at(wx, wy, 1, C_TEXT_ALERT, "BANDIT");

    // Airspeed (knots) and altitude (feet) boxes, beside the ladder
    int32_t box_w = cw * 6 + 8, box_h = lh + 6, boy = cy + UI_BOXES_DY;
    int32_t lx = cx + UI_SPEED_DX - box_w, rx = cx + UI_ALT_DX;
    if (lx < 4) lx = 4;
    if (rx + box_w > sw - 4) rx = sw - 4 - box_w;
    draw_rect(lx, boy - box_h / 2, box_w, box_h, hud_color);
    draw_rect(rx, boy - box_h / 2, box_w, box_h, hud_color);
    float climb = plane.on_ground ? 0 : plane.fwd.y * sim_v(plane.speed);    // Real climb (SIM_SPEED), to match the altimeter
    text_at(lx + box_w / 2, boy - lh / 2 - lh - 6, 0, C_TEXT, "SPD");
    text_at(rx + box_w / 2, boy - lh / 2 - lh - 6, 0, C_TEXT, "ALT");
    text_at(lx + box_w - 4, boy - lh / 2, 1, C_TEXT, "%d", (int)(plane.speed * 1.944f + 0.5f));
    text_at(rx + box_w - 4, boy - lh / 2, 1, C_TEXT, "%d", (int)((plane.pos.y - (plane.on_ground ? GEAR_H : 0)) * 3.281f));
    text_at(lx + box_w / 2, boy + box_h / 2 + 4, 0, C_TEXT, "M%.2f", (double)(plane.speed / 340.0f));
    text_at(rx + box_w / 2, boy + box_h / 2 + 4, 0, C_TEXT, "%+.1fG", (double)plane.g_load);
    text_at(rx + box_w / 2, boy + box_h / 2 + 4 + lh, 0, C_TEXT, "VS%+d", (int)(climb * 196.85f) / 10 * 10);

    // Throttle, and the status lines beside it (from the bottom up)
    draw_throttle();
    draw_fuel();
    draw_radar();
    draw_nav_map();
    int32_t sx0 = ui_pos(UI_STATUS_X, cw * 10, sw), sy0 = ui_pos(UI_STATUS_Y, lh, sh);
    int n = 0;
    if (plane.fuel <= 0)
        text_at(sx0, sy0 - lh * n++, -1, C_TEXT_ALERT, blink ? "FUEL OUT" : "");
    else if (plane.fuel < FUEL_LOW)
        text_at(sx0, sy0 - lh * n++, -1, C_TEXT_ALERT, "FUEL LOW");
    if (plane.damage >= 1)
        text_at(sx0, sy0 - lh * n++, -1, C_TEXT_ALERT, blink ? "ENGINE OUT" : "");
    else if (plane.damage > 0)
        text_at(sx0, sy0 - lh * n++, -1, C_TEXT_ALERT, "ENG %d%%", throttle_limit());
    bool flash = blink || plane.on_ground;  // Flying with them out: they flash
    if (plane.brake)
        text_at(sx0, sy0 - lh * n++, -1, C_TEXT_ALERT, !flash ? "" : plane.on_ground ? "BRAKES" : "SPD BRK");
    if (plane.gear_down)
        text_at(sx0, sy0 - lh * n++, -1, C_TEXT_ALERT, flash ? "GEAR DOWN" : "");

    char mode[32];
    snprintf(mode, sizeof(mode), "%s%s", ctl.mouse ? (ctl.locked ? "MOUSE" : "MOUSE ABS") : "KEYS", ctl.invert ? " INV" : "");
    text_at(ui_pos(UI_MODE_X, (int32_t)strlen(mode) * cw, sw), ui_pos(UI_MODE_Y, lh, sh), -1, C_TEXT, "%s", mode);

    // Stick position: a small cross inside a square
    int32_t stx = cx + UI_STICK_DX, sty = ui_pos(UI_STICK_Y, 24, sh) + 12;
    draw_rect(stx - 12, sty - 12, 24, 24, hud_color);
    int32_t px = stx + (int32_t)(ctl.stick_x * 10), py = sty + (int32_t)(ctl.stick_y * 10);
    line((float)px - 2, (float)py, (float)px + 2, (float)py, hud_color);
    line((float)px, (float)py - 2, (float)px, (float)py + 2, hud_color);

    // Messages and warnings
    float sink = -climb;
    int32_t my = cy + UI_MESSAGE_DY, ky = cy + UI_CRASH_DY;
    if (plane.crashed) {
        text_at(cx, ky, 0, C_TEXT_ALERT, "CRASHED");
        text_at(cx, ky + lh, 0, C_TEXT_ALERT, "%s", plane.crash_reason ? plane.crash_reason : "");
        text_at(cx, ky + lh * 2, 0, C_TEXT_ALERT, "Enter or Esc: back to the pilots");
        if (sortie_score > 0)
            text_at(cx, ky + lh * 3, 0, C_TEXT_ALERT, "The %d points from this flight are lost", sortie_score);
    } else if (plane.on_ground) {
        base_t gb;
        bool enemy_base = plane.speed < 1 && on_pavement(plane.pos.x, plane.pos.z) &&
                          base_at(plane.pos.x, plane.pos.z, &gb) && gb.enemy;
        if (plane.servicing)
            text_at(cx, my, 0, C_TEXT, "REFUELING %d%%%s", (int)plane.fuel, plane.damage > 0 ? " - REPAIRING" : "");
        else if (enemy_base)
            text_at(cx, my, 0, C_TEXT_ALERT, "ENEMY BASE - no fuel here");
        else if (plane.speed < 1 && plane.fuel >= 100 && on_pavement(plane.pos.x, plane.pos.z) && plane.throttle == 0)
            text_at(cx, my, 0, C_TEXT, "FUEL FULL - ready to go");
        else if (plane.landed && plane.speed < 1) {
            text_at(cx, my, 0, C_TEXT, plane.damage > 0 ? "LANDED - and you made it home" : "LANDED - nice flying");
            if (sortie_score)
                text_at(cx, my - lh - 2, 0, C_TEXT, "Esc: back to the pilots, %+d points", sortie_score);
            else
                text_at(cx, my - lh - 2, 0, C_TEXT, "Esc: back to the pilots");
        }
        else if (plane.landed)
            text_at(cx, my, 0, C_TEXT, "TOUCHDOWN - B for brakes");
        else if (plane.throttle == 0 && plane.speed < 1)
            text_at(cx, my, 0, C_TEXT, "Throttle up with +");
        else if (plane.speed < craft->v_rotate * 0.85f)
            text_at(cx, my, 0, C_TEXT, "Pull back at %d knots", (int)(craft->v_rotate * 1.944f + 5) / 10 * 10);
        else if (blink)
            text_at(cx, my, 0, C_TEXT, "ROTATE");
    } else if (!plane.gear_down && agl < 300 && sink > 1 && plane.speed < craft->v_land_max * 1.3f && blink) {
        text_at(cx, my, 0, C_TEXT_ALERT, "GEAR");
    } else if (!plane.gear_down && sink > 1 && agl / sink < 6 && blink) {
        text_at(cx, my, 0, C_TEXT_ALERT, "PULL UP");
    } else if (plane.speed < craft->v_stall && blink) {
        text_at(cx, my, 0, C_TEXT_ALERT, "STALL");
    } else if (plane.hit_time > 0 && world_time - plane.hit_time < 3) {
        text_at(cx, my, 0, C_TEXT_ALERT, plane.damage >= 1 ? "HIT - ENGINE OUT, GLIDE" : "HIT - ENGINE DAMAGED");
    }
    if (!plane.crashed && weapons.message_time > 0 && world_time - weapons.message_time < 3)
        text_at(cx, my + lh + 2, 0, C_TEXT, "%s", weapons.message);
    if (paused)
        text_at(cx, ky - lh * 2, 0, C_TEXT, "PAUSED - P to go on");

    if (show_help) {
        static const char* help[] = {
            "Arrows  stick (Up pushes the nose down)",
            ", .     rudder        Space  center stick",
            "+ -     throttle      0-9    throttle 100%, 10-90%",
            "L       gear          B      brakes",
            "A       full throttle (afterburner)",
            "M       mouse flying  I      invert pitch",
            "W       other weapon  T      next target",
            "Enter   fire (or click)",
            "S       enemies on/off D     chunky pixels",
            "R       radar range   N      map on/off",
            "Z X     map zoom in / out",
            "V       frame rate    O      night",
            "C       chaff         F      flare",
            "Esc     eject (landed: bank score)",
            "P pause   H help   Ctrl+Q quit",
        };
        int count = COUNT(help);
        int32_t y = ui_pos(UI_HELP_Y, count * lh, sh);
        for (int i = 0; i < count; i++)
            text_at(cx + UI_HELP_DX, y + i * lh, -1, C_TEXT, "%s", help[i]);
    }
}
