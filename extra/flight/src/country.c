/*
 * country.c: villages and farms out in the country.
 *
 * The country is cut into COUNTRY_CELL squares, and each may hold one
 * village (a cluster of houses on its own grid, often with a tower) or one
 * farm (a farmhouse, a barn, a shed, maybe a silo, and fields round it).
 * Which, where and how big come from the cell's hash, so the same place
 * always looks the same. Nothing goes on water, on a hill, outside a map's
 * bounds, or near an air base, a SAM site, a target or a map's city.
 * Checking that costs a little, so each cell's answer is kept in a small
 * cache (cleared when another map loads). A map may thin them out
 * ("country" lines: deserts are mostly empty).
 */

#include <string.h>

#include "flight.h"

#define VILLAGE_PCT     18          // Chance a cell has a village ...
#define FARM_PCT        32          // ... or a farm
#define SITE_CLEAR      450.0f      // Flat, dry ground this far round it
#define SITE_BASE_CLEAR 2500.0f     // This far from a runway's middle
#define SITE_SAM_CLEAR  1500.0f
#define SITE_TARGET_CLEAR 1500.0f
#define SITE_CITY_CLEAR 600.0f      // Past a map city's edge
#define HOUSE_GRID      24.0f       // Village plots
#define CACHE_SIZE      1024

static const uint32_t walls[] = {
    GFX_RGB(232, 226, 210), GFX_RGB(214, 200, 170), GFX_RGB(200, 178, 140), GFX_RGB(176, 100, 74),
    GFX_RGB(222, 222, 218), GFX_RGB(190, 168, 128), GFX_RGB(160, 150, 136),
};
static const uint32_t barns[] = { GFX_RGB(150, 52, 40), GFX_RGB(120, 92, 64), GFX_RGB(150, 152, 150) };
static const uint32_t crops[] = {
    GFX_RGB(96, 140, 62), GFX_RGB(198, 178, 92), GFX_RGB(128, 98, 68), GFX_RGB(132, 166, 82), GFX_RGB(170, 160, 70),
};

typedef struct {
    int32_t        i, j;
    uint32_t       serial;          // Which world it was worked out for (0: not yet)
    bool           has;
    country_site_t site;
} cache_t;

static cache_t cache[CACHE_SIZE];

static float unit(uint32_t h) { return (float)(h & 1023) / 1023.0f; }

// Room for a village or farm at x, z?
static bool site_ok(float x, float z) {
    if (!map_inside(x - SITE_CLEAR, z - SITE_CLEAR) || !map_inside(x + SITE_CLEAR, z + SITE_CLEAR))
        return false;
    for (int k = 0; k < 9; k++) {
        float px = x + (float)(k % 3 - 1) * SITE_CLEAR, pz = z + (float)(k / 3 - 1) * SITE_CLEAR;
        if (terrain_height(px, pz) > 0 || map_water_at(px, pz))
            return false;
    }
    base_t b[4];
    if (bases_near(x, z, SITE_BASE_CLEAR, b, 4))
        return false;
    sam_site_t s[4];
    if (sams_near(x, z, SITE_SAM_CLEAR, s, 4))
        return false;
    if (map.loaded) {
        if (map_city_near(x, z, SITE_CITY_CLEAR))
            return false;
        for (int t = 0; t < map.target_count; t++) {
            float dx = map.targets[t].x - x, dz = map.targets[t].z - z;
            if (dx * dx + dz * dz < SITE_TARGET_CLEAR * SITE_TARGET_CLEAR)
                return false;
        }
    }
    return true;
}

static bool site_in_cell(int32_t i, int32_t j, country_site_t* out) {
    uint32_t serial = map.loaded ? map.serial * 2 + 2 : 1;
    cache_t* c = &cache[(uint32_t)(i * 73856093 ^ j * 19349663) % CACHE_SIZE];
    if (c->serial == serial && c->i == i && c->j == j) {
        *out = c->site;
        return c->has;
    }
    c->i = i;
    c->j = j;
    c->serial = serial;
    c->has = false;

    uint32_t h = hash2(i, j, 61), k = hash2(i, j, 62);
    int roll = (int)(h % 100);
    if (roll < VILLAGE_PCT + FARM_PCT) {
        country_site_t s;
        s.kind = roll < VILLAGE_PCT ? COUNTRY_VILLAGE : COUNTRY_FARM;
        s.seed = k;
        float margin = SITE_CLEAR / COUNTRY_CELL;
        s.x = ((float)i + margin + unit(h >> 8) * (1 - 2 * margin)) * COUNTRY_CELL;
        s.z = ((float)j + margin + unit(h >> 18) * (1 - 2 * margin)) * COUNTRY_CELL;
        float a = unit(k) * 1.57f;
        s.ax = v3(cosf(a), 0, -sin_f(a));
        s.az = v3(sin_f(a), 0, cosf(a));
        if ((int)((k >> 20) % 100) < map_country_pct(s.x, s.z) && site_ok(s.x, s.z)) {
            c->site = s;
            c->has = true;
        }
    }
    *out = c->site;
    return c->has;
}

int country_sites_near(float x, float z, float range, country_site_t* out, int max) {
    int n = 0, span = (int)(range / COUNTRY_CELL) + 1;
    int32_t ci = cell_of(x, COUNTRY_CELL), cj = cell_of(z, COUNTRY_CELL);
    for (int32_t j = cj - span; j <= cj + span; j++)
        for (int32_t i = ci - span; i <= ci + span; i++)
            if (n < max && site_in_cell(i, j, &out[n]) && fabsf(out[n].x - x) <= range && fabsf(out[n].z - z) <= range)
                n++;
    return n;
}

static void put(const country_site_t* s, map_building_t* b, float a, float c, float w, float d, float h, uint32_t color) {
    b->x = s->x + s->ax.x * a + s->az.x * c;
    b->z = s->z + s->ax.z * a + s->az.z * c;
    b->ax = s->ax;
    b->az = s->az;
    b->half_w = w * 0.5f;
    b->half_d = d * 0.5f;
    b->height = h;
    b->color = color;
    b->target = -1;
    b->round = false;
}

int country_buildings(const country_site_t* s, map_building_t* out, int max) {
    int n = 0;
    uint32_t seed = s->seed;
    if (s->kind == COUNTRY_FARM) {
        uint32_t g = hash2((int32_t)seed, 1, 63);
        if (n < max) put(s, &out[n++], 0, 0, 12, 9, 6.5f, walls[g % COUNT(walls)]);                  // House
        if (n < max) put(s, &out[n++], 34, 8, 22, 14, 10, barns[(g >> 4) % COUNT(barns)]);            // Barn
        if (n < max) put(s, &out[n++], 18, -22, 10, 8, 4, GFX_RGB(140, 132, 118));                    // Shed
        if ((g >> 8) % 3 && n < max) {                                                                // Silo
            put(s, &out[n], 50, -6, 6, 6, 15, GFX_RGB(196, 198, 196));
            out[n++].round = true;
        }
        return n;
    }

    // A village: houses on plots round the middle, maybe a tower
    int houses = 8 + (int)(seed % 15);
    float r = 70 + (float)houses * 9;
    int32_t used[48][2];
    int used_n = 0;
    for (int k = 0; k < houses * 3 && n < houses && n < max; k++) {
        uint32_t g = hash2((int32_t)seed, k, 64);
        float d = r * sqrtf(unit(g)), t = unit(g >> 10) * 6.2831853f;
        int32_t pa = (int32_t)floorf(cosf(t) * d / HOUSE_GRID + 0.5f), pc = (int32_t)floorf(sin_f(t) * d / HOUSE_GRID + 0.5f);
        bool taken = false;
        for (int u = 0; u < used_n && !taken; u++)
            taken = used[u][0] == pa && used[u][1] == pc;
        if (taken || (pa == 0 && pc == 0))
            continue;
        if (used_n < 48) {
            used[used_n][0] = pa;
            used[used_n][1] = pc;
            used_n++;
        }
        float w = 8 + (float)(g >> 20 & 7), dep = 7 + (float)(g >> 23 & 5), ht = 4 + (float)(g >> 26 & 3);
        put(s, &out[n++], (float)pa * HOUSE_GRID, (float)pc * HOUSE_GRID, w, dep, ht, walls[(g >> 28) % COUNT(walls)]);
    }
    if (seed % 10 < 6 && n < max)
        put(s, &out[n++], 0, 0, 6, 6, 18, GFX_RGB(236, 232, 220));      // Church, temple or mosque tower
    return n;
}

int country_fields(const country_site_t* s, country_field_t* out, int max) {
    if (s->kind != COUNTRY_FARM)
        return 0;
    int n = 0;
    uint32_t g = hash2((int32_t)s->seed, 2, 65);
    int fields = 2 + (int)(g % 3);
    static const float side[4][2] = { { 1, 1 }, { -1, 1 }, { -1, -1 }, { 1, -1 } };
    for (int k = 0; k < fields && n < max; k++) {
        uint32_t f = hash2((int32_t)s->seed, k, 66);
        float w = 150 + unit(f) * 200, d = 150 + unit(f >> 10) * 150;
        float a0 = side[k][0] * 70, c0 = side[k][1] * 60;
        float a1 = a0 + side[k][0] * w, c1 = c0 + side[k][1] * d;
        float pa[4] = { a0, a1, a1, a0 }, pc[4] = { c0, c0, c1, c1 };
        country_field_t* o = &out[n];
        bool ok = true;
        for (int p = 0; p < 4; p++) {
            o->corner[p] = v3(s->x + s->ax.x * pa[p] + s->az.x * pc[p], 0, s->z + s->ax.z * pa[p] + s->az.z * pc[p]);
            if (map_water_at(o->corner[p].x, o->corner[p].z) || terrain_height(o->corner[p].x, o->corner[p].z) > 0)
                ok = false;
        }
        if (!ok)
            continue;
        o->color = crops[(f >> 20) % COUNT(crops)];
        n++;
    }
    return n;
}
