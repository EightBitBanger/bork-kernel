/*
 * world.c: the world. Without a map it is endless: fields, hills, air
 * bases and SAM sites all come from a hash of their grid cell, so nothing
 * is stored and the same place always looks the same. With a map (map.c)
 * they come from the map instead, and there are no made-up fields.
 */

#include <stdlib.h>

#include "flight.h"

const uint32_t tile_colors[5] = {
    0,                              // No field: plain ground
    GFX_RGB(62, 104, 44),           // Dark green
    GFX_RGB(132, 152, 72),          // Light field
    GFX_RGB(128, 108, 70),          // Plowed
    GFX_RGB(52, 92, 150),           // Lake
};

static const uint32_t hill_colors[] = {
    GFX_RGB(128, 110, 82),
    GFX_RGB(104, 112, 78),
    GFX_RGB(146, 136, 114),
    GFX_RGB(96, 98, 88),
};

// Paved parts of a base, in its coordinates (along a0 .. a1, across c0 .. c1)
const base_rect_t base_pavement[6] = {
    { -RUNWAY_HALF_LEN, RUNWAY_HALF_LEN, -RUNWAY_HALF_W, RUNWAY_HALF_W, C_RUNWAY },
    { -1220, -1180, RUNWAY_HALF_W, 150, C_TAXIWAY },        // Connectors
    { 1180, 1220, RUNWAY_HALF_W, 150, C_TAXIWAY },
    { -20, 20, RUNWAY_HALF_W, 150, C_TAXIWAY },
    { -1220, 1220, 130, 150, C_TAXIWAY },                   // Parallel taxiway
    { -320, 320, 150, 330, C_APRON },
};

const building_t base_buildings[7] = {
    { -220, 385, 42, 50, 20, GFX_RGB(150, 152, 140) },     // Hangars
    { 0, 385, 42, 50, 20, GFX_RGB(150, 152, 140) },
    { 220, 385, 42, 50, 20, GFX_RGB(150, 152, 140) },
    { 420, 230, 3.5f, 3.5f, 32, GFX_RGB(206, 200, 186) },   // Control tower's shaft (its cab: render.c)
    { 420, 270, 25, 15, 8, GFX_RGB(176, 168, 150) },        // Operations
    { -420, 210, 12, 12, 10, GFX_RGB(200, 200, 200) },      // Fuel tanks
    { -420, 245, 12, 12, 10, GFX_RGB(200, 200, 200) },
};

float atan2_f(float y, float x) {
    float r = sqrtf(x * x + y * y);
    if (r < 1e-6f)
        return 0;
    float a = acosf(clampf(x / r, -1, 1));
    return y < 0 ? -a : a;
}

uint32_t hash2(int32_t i, int32_t j, uint32_t seed) {
    uint32_t h = (uint32_t)i * 0x8DA6B343u ^ (uint32_t)j * 0xD8163841u ^ seed * 0xCB1AB31Fu;
    h ^= h >> 16; h *= 0x7FEB352Du;
    h ^= h >> 15; h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

int32_t cell_of(float v, float size) {
    return (int32_t)floorf(v / size);
}

//
// Air bases
//

int32_t base_cell_of(float v) {
    return cell_of(v + BASE_CELL * 0.5f, BASE_CELL);    // Cells are centered on multiples of BASE_CELL
}

// The base in base cell (i, j), if it has one. Asked for often (fields,
// hills and SAM sites keep clear of bases), so the last few are kept.
bool base_in_cell(int32_t i, int32_t j, base_t* out) {
    static struct { int32_t i, j; bool valid, ok; base_t base; } cache[16];
    unsigned slot = ((unsigned)i * 7u + (unsigned)j * 13u) & 15u;
    if (cache[slot].valid && cache[slot].i == i && cache[slot].j == j) {
        *out = cache[slot].base;
        return cache[slot].ok;
    }

    base_t b;
    uint32_t h = hash2(i, j, 4), k = hash2(i, j, 5);
    bool ok = (i == 0 && j == 0) || (h & 255) < 150;        // About 60% of cells
    if (ok) {
        if (i == 0 && j == 0) {
            b.x = b.z = 0;
            b.heading = 0;
        } else {
            b.x = (float)i * BASE_CELL + ((float)(h >> 8 & 1023) / 511.5f - 1.0f) * BASE_SPREAD;
            b.z = (float)j * BASE_CELL + ((float)(k & 1023) / 511.5f - 1.0f) * BASE_SPREAD;
            b.heading = (int)((k >> 12) % 18) * 10;
        }
        float a = (float)b.heading * ((float)M_PI / 180.0f), c = cosf(a), s = sin_f(a);
        b.along  = v3(s, 0, c);
        b.across = v3(c, 0, -s);
        b.name   = NULL;
        b.enemy  = (int)((h >> 20) % 100) < BASE_ENEMY_PCT && sqrtf(b.x * b.x + b.z * b.z) > BASE_HOME_CLEAR;
    } else {
        b.enemy = false;
        b.name = NULL;
        b.x = b.z = 0;
        b.heading = 0;
        b.along = b.across = v3(0, 0, 0);
    }
    cache[slot].i = i;
    cache[slot].j = j;
    cache[slot].valid = true;
    cache[slot].ok = ok;
    cache[slot].base = b;
    *out = b;
    return ok;
}

bool base_at(float x, float z, base_t* out) {
    if (map.loaded) {
        // The nearest within reach of its pavement
        base_t near[8];
        int n = map_bases(x, z, RUNWAY_HALF_LEN + 500, near, 8);
        float best = -1;
        for (int i = 0; i < n; i++) {
            float d = (near[i].x - x) * (near[i].x - x) + (near[i].z - z) * (near[i].z - z);
            if (best < 0 || d < best) {
                best = d;
                *out = near[i];
            }
        }
        return best >= 0;
    }
    return base_in_cell(base_cell_of(x), base_cell_of(z), out);
}

int bases_near(float x, float z, float range, base_t* out, int max) {
    if (map.loaded)
        return map_bases(x, z, range, out, max);
    int n = 0, span = (int)(range / BASE_CELL) + 1;
    int32_t ci = base_cell_of(x), cj = base_cell_of(z);
    for (int32_t j = cj - span; j <= cj + span; j++)
        for (int32_t i = ci - span; i <= ci + span; i++)
            if (n < max && base_in_cell(i, j, &out[n]) && fabsf(out[n].x - x) <= range && fabsf(out[n].z - z) <= range)
                n++;
    return n;
}

static bool near_base(float x, float z, float dist) {
    base_t b;
    return base_at(x, z, &b) && fabsf(x - b.x) < dist && fabsf(z - b.z) < dist;
}

void base_local(const base_t* b, float x, float z, float* along, float* across) {
    vec3 d = v3(x - b->x, 0, z - b->z);
    *along = vdot(d, b->along);
    *across = vdot(d, b->across);
}

vec3 base_point(const base_t* b, float along, float across, float y) {
    return v3(b->x + b->along.x * along + b->across.x * across, y,
              b->z + b->along.z * along + b->across.z * across);
}

bool on_runway(float x, float z) {
    base_t b;
    float a, c;
    if (!base_at(x, z, &b))
        return false;
    base_local(&b, x, z, &a, &c);
    return fabsf(a) <= RUNWAY_HALF_LEN && fabsf(c) <= RUNWAY_HALF_W + 3;
}

bool on_pavement(float x, float z) {
    base_t b;
    float a, c;
    if (!base_at(x, z, &b))
        return false;
    base_local(&b, x, z, &a, &c);
    for (size_t i = 0; i < COUNT(base_pavement); i++) {
        const base_rect_t* r = &base_pavement[i];
        if (a >= r->a0 - 3 && a <= r->a1 + 3 && c >= r->c0 - 3 && c <= r->c1 + 3)
            return true;
    }
    return false;
}

//
// Fields and hills
//

int tile_type(int32_t i, int32_t j) {
    if (map.loaded)
        return 0;
    uint32_t h = hash2(i, j, 1) % 100;
    if (h < 50 || near_base(((float)i + 0.5f) * TILE, ((float)j + 0.5f) * TILE, BASE_GRASS))
        return 0;
    return h < 72 ? 1 : h < 87 ? 2 : h < 97 ? 3 : 4;
}

// The hill in cell (i, j), if it has one. Hills stay inside their cell and
// away from air bases.
bool hill_in_cell(int32_t i, int32_t j, hill_t* out) {
    uint32_t h = hash2(i, j, 2), k = hash2(i, j, 3);
    if ((h & 255) >= 110)           // About 43% of cells
        return false;
    out->half   = 450.0f + (float)(h >> 8 & 1023) * (950.0f / 1023.0f);
    out->height = out->half * (0.55f + (float)(k & 255) * (0.55f / 255.0f));
    float room  = HILL_CELL * 0.5f - out->half;
    out->x      = ((float)i + 0.5f) * HILL_CELL + ((float)(h >> 18 & 255) / 127.5f - 1.0f) * room;
    out->z      = ((float)j + 0.5f) * HILL_CELL + ((float)(k >> 8 & 255) / 127.5f - 1.0f) * room;
    out->color  = hill_colors[(k >> 16) % COUNT(hill_colors)];
    return !near_base(out->x, out->z, BASE_CLEAR);
}

int hills_near(float x, float z, float range, hill_t* out, int max) {
    if (map.loaded)
        return map_hills(x, z, range, out, max);
    int n = 0, span = (int)(range / HILL_CELL) + 1;
    int32_t ci = cell_of(x, HILL_CELL), cj = cell_of(z, HILL_CELL);
    for (int32_t j = cj - span; j <= cj + span; j++)
        for (int32_t i = ci - span; i <= ci + span; i++)
            if (n < max && hill_in_cell(i, j, &out[n]))
                n++;
    return n;
}

float terrain_height(float x, float z) {
    if (map.loaded)
        return map_terrain_height(x, z);
    int32_t ci = cell_of(x, HILL_CELL), cj = cell_of(z, HILL_CELL);
    hill_t hill;
    if (!hill_in_cell(ci, cj, &hill))
        return 0;
    float dx = fabsf(x - hill.x), dz = fabsf(z - hill.z);
    float d = dx > dz ? dx : dz;
    return d < hill.half ? hill.height * (1.0f - d / hill.half) : 0;
}

//
// SAM sites
//

// The site in SAM cell (i, j), if it has one: on flat ground, away from
// every base
bool sam_in_cell(int32_t i, int32_t j, sam_site_t* out) {
    uint32_t h = hash2(i, j, 6), k = hash2(i, j, 7);
    if ((h & 255) >= 100)           // About 40% of cells
        return false;
    out->ci = i;
    out->cj = j;
    out->x = ((float)i + 0.5f) * SAM_CELL + ((float)(h >> 8 & 255) / 127.5f - 1.0f) * SAM_SPREAD;
    out->z = ((float)j + 0.5f) * SAM_CELL + ((float)(k & 255) / 127.5f - 1.0f) * SAM_SPREAD;

    int32_t bi = base_cell_of(out->x), bj = base_cell_of(out->z);
    for (int32_t y = bj - 1; y <= bj + 1; y++) {
        for (int32_t x = bi - 1; x <= bi + 1; x++) {
            base_t b;
            if (base_in_cell(x, y, &b) && fabsf(out->x - b.x) < SAM_BASE_CLEAR && fabsf(out->z - b.z) < SAM_BASE_CLEAR)
                return false;
        }
    }
    float r = SAM_RING + 40;
    if (terrain_height(out->x, out->z) > 0 || terrain_height(out->x - r, out->z - r) > 0 ||
        terrain_height(out->x + r, out->z - r) > 0 || terrain_height(out->x - r, out->z + r) > 0 ||
        terrain_height(out->x + r, out->z + r) > 0)
        return false;

    float home = sqrtf(out->x * out->x + out->z * out->z);
    out->enemy     = (k >> 8 & 255) < 180 && home > SAM_HOME_CLEAR;   // About 70% of those far enough out
    out->launchers = 4 + (int)((k >> 16) % 3);
    out->name      = NULL;
    out->angle     = (float)(k >> 20 & 255) * (2.0f * (float)M_PI / 256.0f);
    return true;
}

int sams_near(float x, float z, float range, sam_site_t* out, int max) {
    if (map.loaded)
        return map_sams(x, z, range, out, max);
    int n = 0, span = (int)(range / SAM_CELL) + 1;
    int32_t ci = cell_of(x, SAM_CELL), cj = cell_of(z, SAM_CELL);
    for (int32_t j = cj - span; j <= cj + span; j++)
        for (int32_t i = ci - span; i <= ci + span; i++)
            if (n < max && sam_in_cell(i, j, &out[n]) && fabsf(out[n].x - x) <= range && fabsf(out[n].z - z) <= range)
                n++;
    return n;
}

void sam_launcher(const sam_site_t* s, int n, vec3* pos, vec3* out, vec3* side, vec3* rail) {
    float a = s->angle + (float)n * (2.0f * (float)M_PI) / (float)s->launchers;
    float c = cosf(a), si = sin_f(a);
    *out  = v3(c, 0, si);
    *side = v3(-si, 0, c);
    *pos  = v3(s->x + c * SAM_RING, 0, s->z + si * SAM_RING);
    *rail = vnorm(v3(c * 0.8f, 0.6f, si * 0.8f));      // About 37 degrees up, facing out
}
