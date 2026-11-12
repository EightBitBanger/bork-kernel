/*
 * map.c: theater maps. A map is a text file, one thing per line, fields
 * split by commas; maps/README.txt describes every line. In short:
 *
 *   map, Persian Gulf               its name
 *   units, km                       positions in km (sizes are always meters)
 *   bounds, -120, -100, 120, 100    west, south, east, north edges
 *   ground, sand                    color inside, and outside the bounds
 *   outside, 196 178 140
 *   color, sea, 40 96 150           name a color (r g b)
 *   side, enemy                     what follows is enemy (or friendly)
 *   water, sea, x, y, x, y, ...     ground overlays, drawn in file order:
 *   area, salt, x, y, x, y, ...     water (no city buildings on it) or not
 *   zone, Basra, farm, x, y, ...    an overlay with a name
 *   river, river, 300, x, y, ...    a strip this many meters wide (water)
 *   path, road, 30, x, y, ...       ... or not
 *   city, Kuwait City, x, y, 4000   a named zone with buildings, radius in m
 *   airbase, Name, x, y, heading 150
 *   sam, Name, x, y, launchers 6
 *   radar, Name, x, y
 *   mountain, x, y, width, height
 *   ridge, x, y, x, y, count, width, height
 *   building, x, y, width, depth, height, heading 30
 *   refinery, Name, x, y            and factory, depot, port, powerplant,
 *                                   bunker, bridge, tanks (an oil tank farm)
 *   carrier, USS Nimitz, x, y, heading 30, escorts 4
 *                                   and cruiser, destroyer, frigate,
 *                                   missileboat, tanker: ships at sea
 *   country, 10, farm, palms        villages and farms: 10% of the usual,
 *                                   all of them on areas of these colors
 *   start, Name of an airbase       or a carrier, or: start, x, y, heading, air
 *                                   (only with no friendly base or carrier:
 *                                   flights start at a random one)
 *
 * x is east, y north. A line ending in a comma goes on on the next line.
 * '#' starts a comment. Nothing is placed outside the bounds: past them is
 * plain ground to the horizon.
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flight.h"

map_t map;

typedef struct { base_t b; char name[48]; } map_base_t;
typedef struct { sam_site_t s; char name[48]; } map_sam_t;

typedef struct {
    char     name[48];              // "" for a plain overlay
    uint32_t color;
    int      first, count;          // In points
    float    x0, z0, x1, z1;
    bool     zone;                  // Has a name: map_zone_at finds it
    bool     water;                 // No city buildings on it
} map_poly_t;

typedef struct { float x, z, r; int count; } map_city_t;

static map_base_t* bases;
static int         base_count;
static map_sam_t*  sams;
static int         sam_count;
static hill_t*     hills;
static int         hill_count;
static float       hill_half_max;
static map_poly_t* polys;
static int         poly_count;
static vec2*       points;
static int         point_count;
static map_city_t* cities;
static int         city_count;

// Grow an array to hold one more; false if out of memory
static bool grow(void* array_ptr, int count, size_t size) {
    void** array = (void**)array_ptr;
    if (count >= 8 && (count & (count - 1)))
        return true;                // Room left (capacity doubles at powers of two)
    int cap = count < 8 ? 8 : count * 2;
    void* p = realloc(*array, (size_t)cap * size);
    if (!p)
        return false;
    *array = p;
    return true;
}

//
// Problems: counted, the first kept for the HUD, all printed
//

static const char* file_name;
static int         line_no;

static void problem(const char* fmt, ...) {
    char msg[80];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (line_no) {
        fprintf(stderr, "%s:%d: %s\n", file_name, line_no, msg);
        if (!map.problems++)
            snprintf(map.problem, sizeof(map.problem), "line %d: %s", line_no, msg);
    } else {
        fprintf(stderr, "%s: %s\n", file_name, msg);
        if (!map.problems++)
            snprintf(map.problem, sizeof(map.problem), "%s", msg);
    }
}

//
// Colors: names, or "r g b"
//

typedef struct { char name[24]; uint32_t color; } named_color_t;

static named_color_t colors[64];
static int           color_count;

static const named_color_t default_colors[] = {
    { "sand",   GFX_RGB(198, 178, 130) },
    { "desert", GFX_RGB(210, 186, 136) },
    { "dune",   GFX_RGB(222, 198, 146) },
    { "rock",   GFX_RGB(150, 128, 100) },
    { "salt",   GFX_RGB(226, 222, 206) },
    { "grass",  GFX_RGB(86, 128, 58) },
    { "farm",   GFX_RGB(104, 136, 66) },
    { "palms",  GFX_RGB(70, 106, 52) },
    { "marsh",  GFX_RGB(92, 116, 82) },
    { "water",  GFX_RGB(52, 92, 150) },
    { "sea",    GFX_RGB(44, 92, 146) },
    { "shallows", GFX_RGB(70, 132, 162) },
    { "river",  GFX_RGB(60, 104, 150) },
    { "city",   GFX_RGB(168, 160, 144) },
    { "road",   GFX_RGB(96, 94, 90) },
    { "concrete", GFX_RGB(150, 148, 140) },
    { "white",  GFX_RGB(236, 236, 230) },
    { "grey",   GFX_RGB(128, 128, 124) },
    { "red",    GFX_RGB(170, 60, 50) },
    { "brown",  GFX_RGB(120, 92, 64) },
};

static bool parse_color(const char* s, uint32_t* out) {
    for (int i = color_count - 1; i >= 0; i--) {
        if (!strcasecmp(colors[i].name, s)) {
            *out = colors[i].color;
            return true;
        }
    }
    char* end;
    long v[3];
    const char* p = s;
    for (int k = 0; k < 3; k++) {
        v[k] = strtol(p, &end, 10);
        if (end == p || v[k] < 0 || v[k] > 255)
            return false;
        p = end;
    }
    while (isspace((unsigned char)*p))
        p++;
    if (*p)
        return false;
    *out = GFX_RGB((int)v[0], (int)v[1], (int)v[2]);
    return true;
}

//
// Fields of a line
//

#define MAX_FIELDS      2048

static char* fields[MAX_FIELDS];
static int   field_count;
static float units = 1;             // Meters per position unit
static int   default_side = -1;     // -1: each kind's own; 0 friendly; 1 enemy

static bool number(const char* s, float* out) {
    char* end;
    *out = strtof(s, &end);
    if (end == s)
        return false;
    while (isspace((unsigned char)*end))
        end++;
    return *end == 0;
}

static bool field_number(int i, const char* what, float* out) {
    if (i >= field_count || !number(fields[i], out)) {
        problem("%s: expected a number for the %s", fields[0], what);
        return false;
    }
    return true;
}

// A position: two fields, in map units
static bool field_pos(int i, float* x, float* z) {
    if (!field_number(i, "x (east)", x) || !field_number(i + 1, "y (north)", z))
        return false;
    *x *= units;
    *z *= units;
    return true;
}

typedef struct {
    float    heading;
    bool     has_heading;
    int      side;                  // -1 not given
    int      launchers, buildings, escorts;
    uint32_t color;
    bool     has_color;
} options_t;

// Options after the fields a line must have: "heading 150", "enemy",
// "friendly", "launchers 6", "buildings 80", "escorts 4", "color rock"
static bool parse_options(int from, options_t* o) {
    memset(o, 0, sizeof(*o));
    o->side = -1;
    o->launchers = -1;
    o->buildings = -1;
    o->escorts = -1;
    for (int i = from; i < field_count; i++) {
        char* f = fields[i];
        if (!*f)
            continue;
        char* arg = f;
        while (*arg && !isspace((unsigned char)*arg))
            arg++;
        size_t len = (size_t)(arg - f);
        while (isspace((unsigned char)*arg))
            arg++;
        float v;
        if (!strncasecmp(f, "enemy", len) && len == 5) {
            o->side = 1;
        } else if (!strncasecmp(f, "friendly", len) && len == 8) {
            o->side = 0;
        } else if (len == 7 && !strncasecmp(f, "heading", len) && number(arg, &v)) {
            o->heading = v;
            o->has_heading = true;
        } else if (len == 9 && !strncasecmp(f, "launchers", len) && number(arg, &v) && v >= 0 && v <= 8) {
            o->launchers = (int)v;
        } else if (len == 9 && !strncasecmp(f, "buildings", len) && number(arg, &v) && v >= 0) {
            o->buildings = (int)v;
        } else if (len == 7 && !strncasecmp(f, "escorts", len) && number(arg, &v) && v >= 0 && v <= 6) {
            o->escorts = (int)v;
        } else if (len == 5 && !strncasecmp(f, "color", len) && parse_color(arg, &o->color)) {
            o->has_color = true;
        } else {
            problem("%s: don't know \"%s\"", fields[0], f);
            return false;
        }
    }
    return true;
}

static bool side_of(const options_t* o, bool kind_enemy) {
    if (o->side >= 0)
        return o->side;
    if (default_side >= 0)
        return default_side;
    return kind_enemy;
}

static void copy_name(char* dst, size_t size, const char* src) {
    snprintf(dst, size, "%s", src);
}

//
// Things on the map
//

static void add_building(float x, float z, float heading_deg, float w, float d, float h, uint32_t color) {
    if (!grow(&map.buildings, map.building_count, sizeof(map_building_t)))
        return;
    float a = heading_deg * ((float)M_PI / 180.0f), c = cosf(a), s = sin_f(a);
    map_building_t* b = &map.buildings[map.building_count++];
    b->x = x;
    b->z = z;
    b->az = v3(s, 0, c);            // Depth along the heading
    b->ax = v3(c, 0, -s);
    b->half_w = w * 0.5f;
    b->half_d = d * 0.5f;
    b->height = h;
    b->color = color;
    b->target = -1;
    b->round = false;
}

static void add_hill(float x, float z, float width, float height, uint32_t color) {
    if (!grow(&hills, hill_count, sizeof(hill_t)))
        return;
    hill_t* h = &hills[hill_count++];
    h->x = x;
    h->z = z;
    h->half = width * 0.5f;
    h->height = height;
    h->color = color;
    if (h->half > hill_half_max)
        hill_half_max = h->half;
}

static const uint32_t mountain_colors[] = {
    GFX_RGB(150, 126, 96), GFX_RGB(134, 116, 90), GFX_RGB(160, 140, 110), GFX_RGB(122, 108, 88),
};

// Start a polygon; points follow with poly_point
static map_poly_t* poly_begin(const char* name, uint32_t color) {
    if (!grow(&polys, poly_count, sizeof(map_poly_t)))
        return NULL;
    map_poly_t* p = &polys[poly_count++];
    copy_name(p->name, sizeof(p->name), name ? name : "");
    p->zone = name && *name;
    p->water = false;
    p->color = color;
    p->first = point_count;
    p->count = 0;
    p->x0 = p->z0 = 1e30f;
    p->x1 = p->z1 = -1e30f;
    return p;
}

static void poly_point(map_poly_t* p, float x, float z) {
    if (!grow(&points, point_count, sizeof(vec2)))
        return;
    points[point_count++] = (vec2){ x, z };
    p->count++;
    if (x < p->x0) p->x0 = x;
    if (x > p->x1) p->x1 = x;
    if (z < p->z0) p->z0 = z;
    if (z > p->z1) p->z1 = z;
}

// Points from fields[from ..]: pairs of numbers
static bool poly_fields(map_poly_t* p, int from) {
    if ((field_count - from) % 2 || field_count - from < 6) {
        problem("%s: needs at least 3 points (x, y pairs)", fields[0]);
        return false;
    }
    for (int i = from; i < field_count; i += 2) {
        float x, z;
        if (!field_pos(i, &x, &z))
            return false;
        poly_point(p, x, z);
    }
    return true;
}

// A strip `width` wide along points in fields[from ..]: a quad per segment,
// mitered at the joints so bends have no gaps
static void path_fields(uint32_t color, float width, int from, bool water) {
    int n = (field_count - from) / 2;
    if ((field_count - from) % 2 || n < 2) {
        problem("path: needs at least 2 points (x, y pairs)");
        return;
    }
    vec2* p = malloc(sizeof(vec2) * (size_t)n * 3);
    if (!p)
        return;
    vec2* left = p + n;
    vec2* right = p + n * 2;
    for (int i = 0; i < n; i++)
        if (!field_pos(from + i * 2, &p[i].x, &p[i].y)) {
            free(p);
            return;
        }
    float half = width * 0.5f;
    for (int i = 0; i < n; i++) {
        vec2 nrm = { 0, 0 };
        int segs = 0;
        vec2 seg_n[2];
        if (i > 0) {
            float dx = p[i].x - p[i - 1].x, dz = p[i].y - p[i - 1].y, l = sqrtf(dx * dx + dz * dz);
            if (l > 0) seg_n[segs++] = (vec2){ -dz / l, dx / l };
        }
        if (i < n - 1) {
            float dx = p[i + 1].x - p[i].x, dz = p[i + 1].y - p[i].y, l = sqrtf(dx * dx + dz * dz);
            if (l > 0) seg_n[segs++] = (vec2){ -dz / l, dx / l };
        }
        for (int k = 0; k < segs; k++) {
            nrm.x += seg_n[k].x;
            nrm.y += seg_n[k].y;
        }
        float l = sqrtf(nrm.x * nrm.x + nrm.y * nrm.y);
        float scale = half;
        if (l > 1e-4f) {
            nrm.x /= l;
            nrm.y /= l;
            float c = segs ? nrm.x * seg_n[0].x + nrm.y * seg_n[0].y : 1;
            scale = half / (c > 0.35f ? c : 0.35f);
        }
        left[i] = (vec2){ p[i].x + nrm.x * scale, p[i].y + nrm.y * scale };
        right[i] = (vec2){ p[i].x - nrm.x * scale, p[i].y - nrm.y * scale };
    }
    for (int i = 0; i < n - 1; i++) {
        map_poly_t* q = poly_begin(NULL, color);
        if (!q)
            break;
        q->water = water;
        poly_point(q, left[i].x, left[i].y);
        poly_point(q, left[i + 1].x, left[i + 1].y);
        poly_point(q, right[i + 1].x, right[i + 1].y);
        poly_point(q, right[i].x, right[i].y);
    }
    free(p);
}

// A ring of points: a city's zone
static void circle_poly(const char* name, uint32_t color, float x, float z, float r) {
    map_poly_t* p = poly_begin(name, color);
    if (!p)
        return;
    for (int k = 0; k < 20; k++) {
        float a = (float)k * (2.0f * (float)M_PI / 20.0f);
        float wobble = 0.85f + 0.3f * (float)(hash2((int32_t)x, k, 21) & 255) / 255.0f;
        poly_point(p, x + cosf(a) * r * wobble, z + sin_f(a) * r * wobble);
    }
}

static const uint32_t city_colors[] = {
    GFX_RGB(222, 214, 196), GFX_RGB(204, 196, 178), GFX_RGB(236, 232, 222),
    GFX_RGB(184, 176, 160), GFX_RGB(196, 186, 160), GFX_RGB(170, 168, 162),
};

// Buildings scattered over a city, taller toward the middle, on a grid
// turned the city's own way
static bool on_water(float x, float z);

// On or by one of the map's runways (no city buildings there)
static bool on_airfield(float x, float z) {
    for (int i = 0; i < base_count; i++) {
        float along, across;
        base_local(&bases[i].b, x, z, &along, &across);
        if (fabsf(along) < RUNWAY_HALF_LEN + 400 && fabsf(across) < 600)
            return true;
    }
    return false;
}

static void city_buildings(float x, float z, float r, int count) {
    uint32_t seed = hash2((int32_t)x, (int32_t)z, 22);
    float grid = (float)(seed % 90);
    for (int k = 0; k < count; k++) {
        uint32_t h = hash2((int32_t)seed, k, 23), g = hash2((int32_t)seed, k, 24);
        float u = (float)(h & 1023) / 1023.0f, a = (float)(h >> 10 & 1023) * (2.0f * (float)M_PI / 1024.0f);
        float d = r * 0.9f * sqrtf(u);
        float near_mid = 1.0f - d / r;
        float w = 14 + (float)(g & 31), dep = 14 + (float)(g >> 5 & 31);
        float ht = 5 + (float)(g >> 10 & 15) + near_mid * near_mid * (float)(g >> 14 & 63);
        if (on_water(x + cosf(a) * d, z + sin_f(a) * d) || on_airfield(x + cosf(a) * d, z + sin_f(a) * d))
            continue;
        add_building(x + cosf(a) * d, z + sin_f(a) * d, grid, w, dep, ht, city_colors[(g >> 20) % COUNT(city_colors)]);
    }
}

//
// Targets: a few buildings each, on a pad
//

typedef struct {
    const char*       key;          // Its keyword in the file
    const char*       label;        // For made-up names
    const building_t* parts;        // a: along its heading, c: across
    int               count;
    int               round;        // The first this many parts are round tanks (half_a: radius)
    float             pad;          // Half the pad's side (0: none)
    uint32_t          pad_color;
} target_type_t;

static const building_t factory_parts[] = {
    { 0, 0, 30, 15, 14, GFX_RGB(176, 168, 150) },
    { -45, 10, 20, 12, 10, GFX_RGB(160, 154, 140) },
    { 30, -30, 2, 2, 40, GFX_RGB(140, 90, 70) },
    { -40, -30, 8, 6, 8, GFX_RGB(200, 196, 186) },
};
static const building_t refinery_parts[] = {
    { -40, -30, 7, 7, 10, GFX_RGB(220, 220, 214) }, { -20, -30, 7, 7, 10, GFX_RGB(220, 220, 214) },
    { 0, -30, 7, 7, 10, GFX_RGB(220, 220, 214) },   { -40, -10, 7, 7, 10, GFX_RGB(220, 220, 214) },
    { -20, -10, 7, 7, 10, GFX_RGB(220, 220, 214) }, { 0, -10, 7, 7, 10, GFX_RGB(220, 220, 214) },
    { 30, 15, 15, 10, 12, GFX_RGB(150, 146, 136) },
    { 25, -20, 2, 2, 35, GFX_RGB(120, 120, 116) },  { 35, -20, 2, 2, 30, GFX_RGB(120, 120, 116) },
    { 50, 30, 1, 1, 50, GFX_RGB(110, 100, 90) },
};
static const building_t depot_parts[] = {
    { -30, -20, 10, 5, 5, GFX_RGB(118, 124, 96) }, { 0, -20, 10, 5, 5, GFX_RGB(118, 124, 96) },
    { 30, -20, 10, 5, 5, GFX_RGB(118, 124, 96) },  { -30, 0, 10, 5, 5, GFX_RGB(118, 124, 96) },
    { 0, 0, 10, 5, 5, GFX_RGB(118, 124, 96) },     { 30, 0, 10, 5, 5, GFX_RGB(118, 124, 96) },
    { -30, 20, 10, 5, 5, GFX_RGB(118, 124, 96) },  { 0, 20, 10, 5, 5, GFX_RGB(118, 124, 96) },
    { 30, 20, 10, 5, 5, GFX_RGB(118, 124, 96) },
};
static const building_t port_parts[] = {
    { 0, 60, 110, 10, 2, GFX_RGB(140, 138, 130) },
    { -40, -10, 25, 12, 10, GFX_RGB(176, 150, 120) }, { 20, -10, 25, 12, 10, GFX_RGB(150, 160, 170) },
    { -60, 50, 2, 2, 30, GFX_RGB(210, 150, 40) }, { 0, 50, 2, 2, 30, GFX_RGB(210, 150, 40) },
    { 60, 50, 2, 2, 30, GFX_RGB(210, 150, 40) },
};
static const building_t power_parts[] = {
    { 0, 0, 25, 15, 20, GFX_RGB(186, 180, 168) },
    { -12, 25, 3, 3, 60, GFX_RGB(200, 196, 190) }, { 12, 25, 3, 3, 60, GFX_RGB(200, 196, 190) },
    { 40, -10, 10, 8, 6, GFX_RGB(150, 150, 146) },
};
static const building_t bunker_parts[] = {
    { 0, 0, 15, 15, 5, GFX_RGB(140, 136, 120) },
    { 20, 20, 1, 1, 30, GFX_RGB(110, 110, 110) },
    { -25, 5, 6, 4, 4, GFX_RGB(150, 146, 130) },
};
static const building_t bridge_parts[] = {
    { 0, 0, 150, 6, 6, GFX_RGB(150, 146, 136) },
};

#define TANK(a, c, r, h) { a, c, r, r, h, GFX_RGB(226, 224, 216) }
static const building_t tank_parts[] = {
    TANK(-54, -42, 15, 13), TANK(-18, -42, 15, 13), TANK(18, -42, 15, 13), TANK(54, -42, 15, 13),
    TANK(-54, -6, 15, 13),  TANK(-18, -6, 15, 13),  TANK(18, -6, 15, 13),  TANK(54, -6, 15, 13),
    TANK(-45, 30, 11, 11),  TANK(-20, 30, 11, 11),  TANK(5, 30, 11, 11),
    { 40, 34, 10, 7, 5, GFX_RGB(160, 156, 146) },           // Pump house
    { 66, 34, 1.5f, 1.5f, 22, GFX_RGB(120, 118, 112) },     // Flare stack
};
#undef TANK

static const target_type_t target_types[TARGET_TYPES] = {
    [TARGET_FACTORY]  = { "factory",    "factory",       factory_parts,  COUNT(factory_parts), 0,  70, GFX_RGB(140, 136, 124) },
    [TARGET_REFINERY] = { "refinery",   "refinery",      refinery_parts, COUNT(refinery_parts), 0, 70, GFX_RGB(130, 126, 116) },
    [TARGET_DEPOT]    = { "depot",      "depot",         depot_parts,    COUNT(depot_parts), 0,    50, GFX_RGB(150, 136, 104) },
    [TARGET_PORT]     = { "port",       "port",          port_parts,     COUNT(port_parts), 0,     80, GFX_RGB(136, 134, 128) },
    [TARGET_POWER]    = { "powerplant", "power plant",   power_parts,    COUNT(power_parts), 0,    55, GFX_RGB(138, 136, 128) },
    [TARGET_BUNKER]   = { "bunker",     "command post",  bunker_parts,   COUNT(bunker_parts), 0,   35, GFX_RGB(150, 138, 108) },
    [TARGET_BRIDGE]   = { "bridge",     "bridge",        bridge_parts,   COUNT(bridge_parts), 0,   0,  0 },
    [TARGET_TANKS]    = { "tanks",      "tank farm",     tank_parts,     COUNT(tank_parts), 11,     85, GFX_RGB(132, 128, 118) },
};

static int target_type_of(const char* key) {
    for (int i = 0; i < TARGET_TYPES; i++)
        if (!strcasecmp(target_types[i].key, key))
            return i;
    return -1;
}

uint32_t map_target_pad_color(int type) {
    return target_types[type].pad_color;
}

static void add_target(int type, const char* name, float x, float z, float heading, bool enemy) {
    if (!grow(&map.targets, map.target_count, sizeof(map_target_t)))
        return;
    const target_type_t* t = &target_types[type];
    map_target_t* g = &map.targets[map.target_count++];
    copy_name(g->name, sizeof(g->name), name);
    g->type = type;
    g->x = x;
    g->z = z;
    g->radius = t->pad;
    g->enemy = enemy;
    g->destroyed = false;
    float a = heading * ((float)M_PI / 180.0f), c = cosf(a), s = sin_f(a);
    g->az = v3(s, 0, c);            // Along its heading
    g->ax = v3(c, 0, -s);
    for (int i = 0; i < t->count; i++) {
        const building_t* p = &t->parts[i];
        add_building(x + g->az.x * p->a + g->ax.x * p->c, z + g->az.z * p->a + g->ax.z * p->c,
                     heading, p->half_c * 2, p->half_a * 2, p->height, p->color);
        if (map.building_count) {
            map.buildings[map.building_count - 1].target = map.target_count - 1;
            map.buildings[map.building_count - 1].round = i < t->round;
        }
    }
}

//
// One line
//

static char start_base[48];

static void parse_line(void) {
    const char* key = fields[0];
    options_t o;
    float x, z, v;

    if (!strcasecmp(key, "map")) {
        if (field_count >= 2)
            copy_name(map.name, sizeof(map.name), fields[1]);
    } else if (!strcasecmp(key, "units")) {
        if (field_count >= 2 && (!strcasecmp(fields[1], "km") || !strcasecmp(fields[1], "kilometers")))
            units = 1000;
        else if (field_count >= 2 && (!strcasecmp(fields[1], "m") || !strcasecmp(fields[1], "meters")))
            units = 1;
        else
            problem("units: km or m");
    } else if (!strcasecmp(key, "bounds")) {
        float x0, z0, x1, z1;
        if (field_pos(1, &x0, &z0) && field_pos(3, &x1, &z1)) {
            map.x0 = x0 < x1 ? x0 : x1;
            map.x1 = x0 < x1 ? x1 : x0;
            map.z0 = z0 < z1 ? z0 : z1;
            map.z1 = z0 < z1 ? z1 : z0;
        }
    } else if (!strcasecmp(key, "ground") || !strcasecmp(key, "outside")) {
        uint32_t c;
        if (field_count < 2 || !parse_color(fields[1], &c)) {
            problem("%s: expected a color (a name, or r g b)", key);
            return;
        }
        if (!strcasecmp(key, "ground"))
            map.ground = c;
        else
            map.outside = c;
    } else if (!strcasecmp(key, "color")) {
        uint32_t c;
        if (field_count < 3 || !parse_color(fields[2], &c)) {
            problem("color: expected a name and r g b");
            return;
        }
        if (color_count < (int)COUNT(colors)) {
            copy_name(colors[color_count].name, sizeof(colors[0].name), fields[1]);
            colors[color_count++].color = c;
        }
    } else if (!strcasecmp(key, "side")) {
        if (field_count >= 2 && !strcasecmp(fields[1], "enemy"))
            default_side = 1;
        else if (field_count >= 2 && !strcasecmp(fields[1], "friendly"))
            default_side = 0;
        else
            problem("side: enemy or friendly");
    } else if (!strcasecmp(key, "start")) {
        if (field_count == 2) {
            copy_name(start_base, sizeof(start_base), fields[1]);
        } else if (field_pos(1, &x, &z)) {
            map.start_x = x;
            map.start_z = z;
            map.start_heading = field_count > 3 && number(fields[3], &v) ? v : 0;
            map.start_air = true;   // Not on a runway: has to be flying
            start_base[0] = 0;
        }
    } else if (!strcasecmp(key, "country")) {
        float pct;
        if (!field_number(1, "percent", &pct))
            return;
        map.country_pct = (int)clampf(pct, 0, 100);
        for (int i = 2; i < field_count && map.country_full_count < (int)COUNT(map.country_full); i++) {
            uint32_t c;
            if (parse_color(fields[i], &c))
                map.country_full[map.country_full_count++] = c;
            else
                problem("country: don't know the color \"%s\"", fields[i]);
        }
    } else if (!strcasecmp(key, "fighters")) {
        for (int i = 1; i < field_count; i++) {
            int t = fighter_type(fields[i]);
            if (t < 0)
                problem("fighters: don't know \"%s\"", fields[i]);
            else if (map.fighter_count < (int)COUNT(map.fighters))
                map.fighters[map.fighter_count++] = t;
        }
    } else if (!strcasecmp(key, "airbase")) {
        if (field_count < 4 || !field_pos(2, &x, &z) || !parse_options(4, &o))
            return;
        if (!grow(&bases, base_count, sizeof(map_base_t)))
            return;
        map_base_t* mb = &bases[base_count++];
        copy_name(mb->name, sizeof(mb->name), fields[1]);
        int heading = ((int)floorf(o.heading + 0.5f) % 180 + 180) % 180;
        float a = (float)heading * ((float)M_PI / 180.0f), c = cosf(a), s = sin_f(a);
        mb->b.x = x;
        mb->b.z = z;
        mb->b.heading = heading;
        mb->b.along = v3(s, 0, c);
        mb->b.across = v3(c, 0, -s);
        mb->b.enemy = side_of(&o, false);
        mb->b.name = NULL;
    } else if (!strcasecmp(key, "sam") || !strcasecmp(key, "radar")) {
        if (field_count < 4 || !field_pos(2, &x, &z) || !parse_options(4, &o))
            return;
        if (!grow(&sams, sam_count, sizeof(map_sam_t)))
            return;
        map_sam_t* ms = &sams[sam_count];
        copy_name(ms->name, sizeof(ms->name), fields[1]);
        ms->s.ci = sam_count++;
        ms->s.cj = MAP_SITE;
        ms->s.x = x;
        ms->s.z = z;
        ms->s.enemy = side_of(&o, true);
        ms->s.launchers = !strcasecmp(key, "radar") ? 0 : o.launchers >= 0 ? o.launchers : 5;
        ms->s.angle = (float)(hash2((int32_t)x, (int32_t)z, 25) & 255) * (2.0f * (float)M_PI / 256.0f);
        ms->s.name = NULL;
    } else if (!strcasecmp(key, "mountain")) {
        float w, h;
        if (!field_pos(1, &x, &z) || !field_number(3, "width", &w) || !field_number(4, "height", &h) ||
            !parse_options(5, &o))
            return;
        add_hill(x, z, w, h, o.has_color ? o.color : mountain_colors[hash2((int32_t)x, (int32_t)z, 26) % 4]);
    } else if (!strcasecmp(key, "ridge")) {
        float x1, z1, n, w, h;
        if (!field_pos(1, &x, &z) || !field_pos(3, &x1, &z1) || !field_number(5, "count", &n) ||
            !field_number(6, "width", &w) || !field_number(7, "height", &h) || !parse_options(8, &o))
            return;
        int count = (int)n < 1 ? 1 : (int)n;
        float dx = x1 - x, dz = z1 - z, l = sqrtf(dx * dx + dz * dz);
        float px = l > 0 ? -dz / l : 0, pz = l > 0 ? dx / l : 0;
        for (int k = 0; k < count; k++) {
            uint32_t r = hash2(line_no, k, 27);
            float t = count > 1 ? (float)k / (float)(count - 1) : 0.5f;
            float side = ((float)(r & 255) / 127.5f - 1.0f) * w * 0.35f;
            float ws = w * (0.7f + 0.6f * (float)(r >> 8 & 255) / 255.0f);
            float hs = h * (0.6f + 0.6f * (float)(r >> 16 & 255) / 255.0f);
            add_hill(x + dx * t + px * side, z + dz * t + pz * side, ws, hs,
                     o.has_color ? o.color : mountain_colors[(r >> 24) % 4]);
        }
    } else if (!strcasecmp(key, "building")) {
        float w, d, h;
        if (!field_pos(1, &x, &z) || !field_number(3, "width", &w) || !field_number(4, "depth", &d) ||
            !field_number(5, "height", &h) || !parse_options(6, &o))
            return;
        add_building(x, z, o.heading, w, d, h, o.has_color ? o.color : GFX_RGB(196, 188, 170));
    } else if (!strcasecmp(key, "city")) {
        float r;
        if (field_count < 5 || !field_pos(2, &x, &z) || !field_number(4, "radius", &r) || !parse_options(5, &o))
            return;
        uint32_t c;
        parse_color("city", &c);
        circle_poly(fields[1], o.has_color ? o.color : c, x, z, r);
        if (!grow(&cities, city_count, sizeof(map_city_t)))
            return;
        int n = o.buildings >= 0 ? o.buildings : (int)(r / 40);
        cities[city_count++] = (map_city_t){ x, z, r, n > 400 ? 400 : n };     // Built once all water is known
    } else if (!strcasecmp(key, "zone") || !strcasecmp(key, "area") || !strcasecmp(key, "water")) {
        bool zone = !strcasecmp(key, "zone");
        int ci = zone ? 2 : 1;
        uint32_t c;
        if (field_count <= ci || !parse_color(fields[ci], &c)) {
            problem("%s: expected %sa color", key, zone ? "a name, then " : "");
            return;
        }
        map_poly_t* p = poly_begin(zone ? fields[1] : NULL, c);
        if (p)
            p->water = !strcasecmp(key, "water");
        if (p && !poly_fields(p, ci + 1)) {
            point_count = p->first;
            poly_count--;
        }
    } else if (!strcasecmp(key, "path") || !strcasecmp(key, "river")) {
        uint32_t c;
        float w;
        if (field_count < 3 || !parse_color(fields[1], &c)) {
            problem("%s: expected a color", key);
            return;
        }
        if (!field_number(2, "width", &w))
            return;
        path_fields(c, w, 3, !strcasecmp(key, "river"));
    } else if (ship_kind_of(key) >= 0) {
        if (field_count < 4 || !field_pos(2, &x, &z) || !parse_options(4, &o))
            return;
        if (!grow(&map.ships, map.ship_count, sizeof(map_ship_t)))
            return;
        map_ship_t* ms = &map.ships[map.ship_count++];
        ms->kind = ship_kind_of(key);
        copy_name(ms->name, sizeof(ms->name), fields[1]);
        ms->x = x;
        ms->z = z;
        ms->heading = o.heading;
        ms->enemy = side_of(&o, ms->kind != SHIP_TANKER);
        ms->escorts = o.escorts >= 0 ? o.escorts : ms->kind == SHIP_CARRIER ? 6 : 0;
    } else {
        int type = target_type_of(key);
        if (type < 0) {
            problem("don't know \"%s\"", key);
            return;
        }
        if (field_count < 4 || !field_pos(2, &x, &z) || !parse_options(4, &o))
            return;
        add_target(type, fields[1], x, z, o.heading, side_of(&o, true));
    }
}

//
// Overlays to triangles: clipped to the bounds, then ear clipping
//

static float min3(float a, float b, float c) { return a < b ? (a < c ? a : c) : (b < c ? b : c); }
static float max3(float a, float b, float c) { return a > b ? (a > c ? a : c) : (b > c ? b : c); }

static void add_tri(vec2 a, vec2 b, vec2 c, uint32_t color) {
    if (!grow(&map.tris, map.tri_count, sizeof(map_tri_t)))
        return;
    map_tri_t* t = &map.tris[map.tri_count++];
    t->ax = a.x; t->az = a.y;
    t->bx = b.x; t->bz = b.y;
    t->cx = c.x; t->cz = c.y;
    t->x0 = min3(a.x, b.x, c.x);
    t->x1 = max3(a.x, b.x, c.x);
    t->z0 = min3(a.y, b.y, c.y);
    t->z1 = max3(a.y, b.y, c.y);
    t->color = color;
}

static float cross3(vec2 a, vec2 b, vec2 c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

static int clip_side(const vec2* in, int n, vec2* out, int axis, float limit, bool keep_below) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        vec2 a = in[i], b = in[(i + 1) % n];
        float va = axis ? a.y : a.x, vb = axis ? b.y : b.x;
        bool ina = keep_below ? va <= limit : va >= limit;
        bool inb = keep_below ? vb <= limit : vb >= limit;
        if (ina)
            out[m++] = a;
        if (ina != inb) {
            float t = (limit - va) / (vb - va);
            out[m++] = (vec2){ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
        }
    }
    return m;
}

static void triangulate(const map_poly_t* poly) {
    int n = poly->count;
    vec2* buf = malloc(sizeof(vec2) * (size_t)(n * 2 + 8) * 2);
    int* idx = malloc(sizeof(int) * (size_t)(n * 2 + 8));
    if (!buf || !idx) {
        free(buf);
        free(idx);
        return;
    }
    vec2* a = buf;
    vec2* b = buf + n * 2 + 8;
    memcpy(a, points + poly->first, sizeof(vec2) * (size_t)n);
    n = clip_side(a, n, b, 0, map.x0, false);
    n = clip_side(b, n, a, 0, map.x1, true);
    n = clip_side(a, n, b, 1, map.z0, false);
    n = clip_side(b, n, a, 1, map.z1, true);

    // Counterclockwise (x east, y north)
    float area = 0;
    for (int i = 0; i < n; i++)
        area += a[i].x * a[(i + 1) % n].y - a[(i + 1) % n].x * a[i].y;
    for (int i = 0; i < n; i++)
        idx[i] = area >= 0 ? i : n - 1 - i;

    int guard = 0;
    int i = 0;
    while (n > 3 && guard < n) {
        int ip = (i + n - 1) % n, in = (i + 1) % n;
        vec2 pa = a[idx[ip]], pb = a[idx[i]], pc = a[idx[in]];
        float c = cross3(pa, pb, pc);
        bool ear = false, drop = false;
        if (fabsf(c) < 1e-3f) {
            drop = true;            // On a line with its neighbors: no triangle
        } else if (c > 0) {
            ear = true;
            for (int k = 0; k < n && ear; k++) {
                if (k == ip || k == i || k == in)
                    continue;
                vec2 q = a[idx[k]];
                if ((q.x == pa.x && q.y == pa.y) || (q.x == pb.x && q.y == pb.y) || (q.x == pc.x && q.y == pc.y))
                    continue;
                if (cross3(pa, pb, q) >= 0 && cross3(pb, pc, q) >= 0 && cross3(pc, pa, q) >= 0)
                    ear = false;
            }
        }
        if (ear || drop) {
            if (ear)
                add_tri(pa, pb, pc, poly->color);
            memmove(idx + i, idx + i + 1, sizeof(int) * (size_t)(n - i - 1));
            n--;
            if (i >= n)
                i = 0;
            guard = 0;
        } else {
            i = (i + 1) % n;
            guard++;
        }
    }
    if (n > 3) {
        // Not a simple polygon (it crosses itself): fan out the rest
        for (int k = 1; k < n - 1; k++)
            add_tri(a[idx[0]], a[idx[k]], a[idx[k + 1]], poly->color);
    } else if (n == 3 && fabsf(cross3(a[idx[0]], a[idx[1]], a[idx[2]])) >= 1e-3f) {
        add_tri(a[idx[0]], a[idx[1]], a[idx[2]], poly->color);
    }
    free(buf);
    free(idx);
}

//
// After the file: bounds, names, start
//

bool map_inside(float x, float z) {
    return !map.loaded || (x >= map.x0 && x <= map.x1 && z >= map.z0 && z <= map.z1);
}

static bool in_poly(const map_poly_t* p, float x, float z) {
    if (x < p->x0 || x > p->x1 || z < p->z0 || z > p->z1)
        return false;
    bool in = false;
    const vec2* q = points + p->first;
    for (int i = 0, j = p->count - 1; i < p->count; j = i++) {
        if ((q[i].y > z) != (q[j].y > z) &&
            x < (q[j].x - q[i].x) * (z - q[i].y) / (q[j].y - q[i].y) + q[i].x)
            in = !in;
    }
    return in;
}

// The topmost area there is water (an island drawn over the sea is not)
static bool on_water(float x, float z) {
    for (int i = poly_count - 1; i >= 0; i--)
        if (in_poly(&polys[i], x, z))
            return polys[i].water;
    return false;
}

int map_country_pct(float x, float z) {
    if (!map.loaded)
        return 100;
    if (map.country_full_count)
        for (int i = poly_count - 1; i >= 0; i--)
            if (in_poly(&polys[i], x, z)) {
                for (int k = 0; k < map.country_full_count; k++)
                    if (polys[i].color == map.country_full[k])
                        return 100;
                break;
            }
    return map.country_pct;
}

bool map_city_near(float x, float z, float margin) {
    for (int i = 0; i < city_count; i++) {
        float dx = x - cities[i].x, dz = z - cities[i].z, r = cities[i].r * 1.15f + margin;
        if (dx * dx + dz * dz < r * r)
            return true;
    }
    return false;
}

bool map_water_at(float x, float z) {
    return map.loaded && on_water(x, z);
}

// The zone seen there: the topmost one, unless water is drawn over it
const char* map_zone_at(float x, float z) {
    if (!map.loaded)
        return NULL;
    for (int i = poly_count - 1; i >= 0; i--) {
        if ((!polys[i].zone && !polys[i].water) || !in_poly(&polys[i], x, z))
            continue;
        return polys[i].zone ? polys[i].name : NULL;
    }
    return NULL;
}

// Grow a box (x0, z0, x1, z1) to hold a point
static void take(float* box, float x, float z) {
    if (x < box[0]) box[0] = x;
    if (z < box[1]) box[1] = z;
    if (x > box[2]) box[2] = x;
    if (z > box[3]) box[3] = z;
}

// Flat ground (no hill) at x, z and all round it to r
static bool flat_at(float x, float z, float r) {
    if (map_terrain_height(x, z) > 0)
        return false;
    for (int k = 0; k < 8; k++) {
        float a = (float)k * ((float)M_PI / 4);
        if (map_terrain_height(x + cosf(a) * r, z + sin_f(a) * r) > 0)
            return false;
    }
    return true;
}

// No water at x, z or round it to r
static bool dry_at(float x, float z, float r) {
    if (on_water(x, z))
        return false;
    for (int k = 0; k < 8; k++) {
        float a = (float)k * ((float)M_PI / 4);
        if (on_water(x + cosf(a) * r, z + sin_f(a) * r))
            return false;
    }
    return true;
}

// What a spot needs: flat ground round it to r (and dry, if dry), or for
// a base, its runway and apron clear of hills and water
typedef struct {
    float         r;
    bool          dry;
    const base_t* base;
} spot_t;

static bool spot_ok(const spot_t* s, float x, float z) {
    if (!map_inside(x, z))
        return false;
    if (!s->base)
        return flat_at(x, z, s->r) && (!s->dry || dry_at(x, z, s->r));
    for (float a = -RUNWAY_HALF_LEN - 200; a <= RUNWAY_HALF_LEN + 200; a += 100)
        for (float c = -300; c <= 300; c += 150) {
            float px = x + s->base->along.x * a + s->base->across.x * c;
            float pz = z + s->base->along.z * a + s->base->across.z * c;
            if (map_terrain_height(px, pz) > 0 || on_water(px, pz))
                return false;
        }
    return true;
}

// The nearest spot that will do, within MOVE_MAX of x, z; false if none
#define MOVE_MAX        8000.0f
static bool spot_near(const spot_t* s, float x, float z, float* fx, float* fz) {
    float step = s->base ? 250 : 150;
    for (float d = step; d <= MOVE_MAX; d += step) {
        int steps = 8 + (int)(d / step);
        for (int k = 0; k < steps; k++) {
            float a = (float)k * (2.0f * (float)M_PI / (float)steps);
            float nx = x + cosf(a) * d, nz = z + sin_f(a) * d;
            if (spot_ok(s, nx, nz)) {
                *fx = nx;
                *fz = nz;
                return true;
            }
        }
    }
    return false;
}

// Runways clear of hills and water: a base that isn't moves to the nearest
// place that is (before the cities are built round them)
static void fix_bases(void) {
    for (int i = 0; i < base_count; i++) {
        base_t* b = &bases[i].b;
        spot_t s = { 0, false, b };
        float nx, nz;
        if (!spot_ok(&s, b->x, b->z) && spot_near(&s, b->x, b->z, &nx, &nz)) {
            b->x = nx;
            b->z = nz;
        }
    }
}

// Nothing built into a hillside or standing in water: targets (but ports
// and bridges may be by or over water) and SAM sites move to the nearest
// place that will do (a target with all its buildings), and city buildings
// on a hill are left out
static void fix_sites(void) {
    for (int i = 0; i < map.target_count; i++) {
        map_target_t* t = &map.targets[i];
        bool wet_ok = t->type == TARGET_PORT || t->type == TARGET_BRIDGE;
        spot_t s = { t->radius + 20, !wet_ok, NULL };
        float nx, nz;
        if (spot_ok(&s, t->x, t->z) || !spot_near(&s, t->x, t->z, &nx, &nz))
            continue;
        float dx = nx - t->x, dz = nz - t->z;
        t->x = nx;
        t->z = nz;
        for (int k = 0; k < map.building_count; k++)
            if (map.buildings[k].target == i) {
                map.buildings[k].x += dx;
                map.buildings[k].z += dz;
            }
    }
    for (int i = 0; i < sam_count; i++) {
        sam_site_t* site = &sams[i].s;
        spot_t s = { SAM_RING + 60, true, NULL };
        float nx, nz;
        if (!spot_ok(&s, site->x, site->z) && spot_near(&s, site->x, site->z, &nx, &nz)) {
            site->x = nx;
            site->z = nz;
        }
    }
    int n = 0;
    for (int i = 0; i < map.building_count; i++) {
        const map_building_t* b = &map.buildings[i];
        float r = b->half_w > b->half_d ? b->half_w : b->half_d;
        if (b->target >= 0 || flat_at(b->x, b->z, r))
            map.buildings[n++] = *b;
    }
    map.building_count = n;
}

static void finish(void) {
    // Bounds: given, or around everything with room to spare
    if (map.x0 >= map.x1 || map.z0 >= map.z1) {
        float box[4] = { 1e30f, 1e30f, -1e30f, -1e30f };
        for (int i = 0; i < base_count; i++) take(box, bases[i].b.x, bases[i].b.z);
        for (int i = 0; i < sam_count; i++) take(box, sams[i].s.x, sams[i].s.z);
        for (int i = 0; i < hill_count; i++) take(box, hills[i].x, hills[i].z);
        for (int i = 0; i < map.target_count; i++) take(box, map.targets[i].x, map.targets[i].z);
        for (int i = 0; i < point_count; i++) take(box, points[i].x, points[i].y);
        float x0 = box[0], z0 = box[1], x1 = box[2], z1 = box[3];
        if (x0 > x1) { x0 = z0 = -20000; x1 = z1 = 20000; }
        map.x0 = x0 - 10000; map.z0 = z0 - 10000;
        map.x1 = x1 + 10000; map.z1 = z1 + 10000;
    }

    fix_bases();
    for (int i = 0; i < city_count; i++)
        city_buildings(cities[i].x, cities[i].z, cities[i].r, cities[i].count);

    // Nothing outside them
    line_no = 0;
    int dropped = 0, n = 0;
    for (int i = 0; i < base_count; i++)
        if (map_inside(bases[i].b.x, bases[i].b.z)) bases[n++] = bases[i]; else dropped++;
    base_count = n;
    n = 0;
    for (int i = 0; i < sam_count; i++)
        if (map_inside(sams[i].s.x, sams[i].s.z)) { sams[n] = sams[i]; sams[n].s.ci = n; n++; } else dropped++;
    sam_count = n;
    n = 0;
    for (int i = 0; i < hill_count; i++)
        if (map_inside(hills[i].x, hills[i].z)) hills[n++] = hills[i]; else dropped++;
    hill_count = n;
    // Targets, then buildings (pointing to their targets' new places)
    int* moved = malloc(sizeof(int) * (size_t)(map.target_count + 1));
    n = 0;
    for (int i = 0; i < map.target_count; i++) {
        bool in = map_inside(map.targets[i].x, map.targets[i].z);
        if (moved)
            moved[i] = in ? n : -1;
        if (in)
            map.targets[n++] = map.targets[i];
        else
            dropped++;
    }
    map.target_count = n;
    n = 0;
    for (int i = 0; i < map.building_count; i++) {
        map_building_t* b = &map.buildings[i];
        if (b->target >= 0)
            b->target = moved ? moved[b->target] : -1;
        if (map_inside(b->x, b->z))
            map.buildings[n++] = *b;
    }
    map.building_count = n;
    free(moved);
    if (dropped)
        problem("%d things are outside the bounds (left out)", dropped);

    for (int i = 0; i < poly_count; i++)
        triangulate(&polys[i]);
    fix_sites();

    // Targets without a name are named for where they are
    for (int i = 0; i < map.target_count; i++) {
        map_target_t* t = &map.targets[i];
        if (t->name[0])
            continue;
        const char* zone = map_zone_at(t->x, t->z);
        if (zone) {
            snprintf(t->name, sizeof(t->name), "%s %s", zone, target_types[t->type].label);
        } else {
            copy_name(t->name, sizeof(t->name), target_types[t->type].label);
            t->name[0] = (char)toupper((unsigned char)t->name[0]);
        }
    }

    // Where a flight starts: the base named, else the first friendly one
    int sb = -1;
    for (int i = 0; i < base_count && sb < 0; i++)
        if (start_base[0] ? !strcasecmp(bases[i].name, start_base) : !bases[i].b.enemy)
            sb = i;
    map.start_ship = -1;
    for (int i = 0; i < map.ship_count && start_base[0] && sb < 0 && map.start_ship < 0; i++)
        if (!strcasecmp(map.ships[i].name, start_base) && map.ships[i].kind == SHIP_CARRIER)
            map.start_ship = i;
    if (start_base[0] && sb < 0 && map.start_ship < 0)
        problem("start: no airbase or carrier called \"%s\"", start_base);
    if (map.start_ship >= 0) {
        map.start_x = map.ships[map.start_ship].x;
        map.start_z = map.ships[map.start_ship].z;
        map.start_heading = map.ships[map.start_ship].heading;
        map.start_air = false;
    } else if (sb >= 0) {
        map.start_x = bases[sb].b.x;
        map.start_z = bases[sb].b.z;
        map.start_heading = (float)bases[sb].b.heading;
        map.start_air = false;
    } else if (!map.start_air) {
        map.start_x = (map.x0 + map.x1) * 0.5f;
        map.start_z = (map.z0 + map.z1) * 0.5f;
        map.start_air = true;
    }
}

// Back to no map: the endless world
void map_random_start(uint32_t seed) {
    int n = 0;
    for (int i = 0; i < base_count; i++)
        n += !bases[i].b.enemy;
    for (int i = 0; i < map.ship_count; i++)
        n += map.ships[i].kind == SHIP_CARRIER && !map.ships[i].enemy;
    if (!map.loaded || n == 0)
        return;                         // Nowhere friendly: the map's own start
    int pick = (int)(seed % (uint32_t)n);
    map.start_ship = -1;
    map.start_air = false;
    for (int i = 0; i < base_count; i++)
        if (!bases[i].b.enemy && pick-- == 0) {
            map.start_x = bases[i].b.x;
            map.start_z = bases[i].b.z;
            map.start_heading = (float)bases[i].b.heading;
            return;
        }
    for (int i = 0; i < map.ship_count; i++)
        if (map.ships[i].kind == SHIP_CARRIER && !map.ships[i].enemy && pick-- == 0) {
            map.start_ship = i;
            map.start_x = map.ships[i].x;
            map.start_z = map.ships[i].z;
            map.start_heading = map.ships[i].heading;
            return;
        }
}

void map_unload(void) {
    free(map.tris);
    free(map.buildings);
    free(map.targets);
    free(bases);
    free(sams);
    free(hills);
    free(polys);
    free(points);
    free(cities);
    free(map.ships);
    bases = NULL; sams = NULL; hills = NULL; polys = NULL; points = NULL; cities = NULL;
    base_count = sam_count = hill_count = poly_count = point_count = city_count = 0;
    hill_half_max = 0;
    memset(&map, 0, sizeof(map));
    map.start_ship = -1;
    map.country_pct = 100;
}

// A map file's name (its "map" line), without reading the rest; false if
// it can't be opened
bool map_peek_name(const char* path, char* out, size_t size) {
    FILE* f = fopen(path, "r");
    if (!f)
        return false;
    char buf[256];
    out[0] = 0;
    for (int n = 0; n < 200 && fgets(buf, sizeof(buf), f); n++) {
        char* p = buf;
        while (isspace((unsigned char)*p))
            p++;
        if (strncasecmp(p, "map", 3) || (p[3] != ',' && !isspace((unsigned char)p[3])))
            continue;
        p = strchr(p, ',');
        if (!p)
            continue;
        p++;
        while (isspace((unsigned char)*p))
            p++;
        char* e = p + strcspn(p, "#\r\n");
        while (e > p && isspace((unsigned char)e[-1]))
            e--;
        snprintf(out, size, "%.*s", (int)(e - p), p);
        break;
    }
    fclose(f);
    return true;
}

bool map_load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f)
        return false;
    static uint32_t serial;
    map_unload();
    map.serial = ++serial;
    file_name = path;
    line_no = 0;
    units = 1;
    default_side = -1;
    start_base[0] = 0;
    color_count = (int)COUNT(default_colors);
    memcpy(colors, default_colors, sizeof(default_colors));
    map.ground = map.outside = 0xFFFFFFFFu;     // Not given yet

    static char line[65536];
    char part[4096];
    size_t len = 0;
    int first_line = 1;
    while (fgets(part, sizeof(part), f)) {
        line_no++;
        if (!len)
            first_line = line_no;
        char* hash = strchr(part, '#');
        if (hash)
            *hash = 0;
        size_t pl = strlen(part);
        while (pl && isspace((unsigned char)part[pl - 1]))
            part[--pl] = 0;
        if (len + pl + 1 >= sizeof(line)) {
            problem("line too long");
            len = 0;
            continue;
        }
        memcpy(line + len, part, pl + 1);
        len += pl;
        if (len && line[len - 1] == ',')
            continue;               // Goes on on the next line
        if (!len)
            continue;

        // Split at commas, trim each field
        int keep_line = line_no;
        line_no = first_line;
        field_count = 0;
        char* p = line;
        for (;;) {
            char* comma = strchr(p, ',');
            if (comma)
                *comma = 0;
            while (isspace((unsigned char)*p))
                p++;
            char* e = p + strlen(p);
            while (e > p && isspace((unsigned char)e[-1]))
                *--e = 0;
            if (field_count < MAX_FIELDS)
                fields[field_count++] = p;
            if (!comma)
                break;
            p = comma + 1;
        }
        if (field_count && fields[0][0])
            parse_line();
        line_no = keep_line;
        len = 0;
    }
    fclose(f);

    if (map.ground == 0xFFFFFFFFu)
        map.ground = C_GROUND;
    if (map.outside == 0xFFFFFFFFu)
        map.outside = map.ground;
    if (!map.name[0])
        copy_name(map.name, sizeof(map.name), path);
    map.loaded = true;
    finish();
    return true;
}

//
// Asked for by the world
//

int map_bases(float x, float z, float range, base_t* out, int max) {
    int n = 0;
    for (int i = 0; i < base_count && n < max; i++) {
        if (fabsf(bases[i].b.x - x) > range || fabsf(bases[i].b.z - z) > range)
            continue;
        out[n] = bases[i].b;
        out[n++].name = bases[i].name;
    }
    return n;
}

int map_sams(float x, float z, float range, sam_site_t* out, int max) {
    int n = 0;
    for (int i = 0; i < sam_count && n < max; i++) {
        if (fabsf(sams[i].s.x - x) > range || fabsf(sams[i].s.z - z) > range)
            continue;
        out[n] = sams[i].s;
        out[n++].name = sams[i].name;
    }
    return n;
}

int map_hills(float x, float z, float range, hill_t* out, int max) {
    int n = 0;
    for (int i = 0; i < hill_count && n < max; i++) {
        float r = range + hills[i].half;
        if (fabsf(hills[i].x - x) <= r && fabsf(hills[i].z - z) <= r)
            out[n++] = hills[i];
    }
    return n;
}

float map_terrain_height(float x, float z) {
    float best = 0;
    for (int i = 0; i < hill_count; i++) {
        const hill_t* h = &hills[i];
        float dx = fabsf(x - h->x), dz = fabsf(z - h->z);
        if (dx >= h->half || dz >= h->half)
            continue;
        float d = dx > dz ? dx : dz;
        float y = h->height * (1.0f - d / h->half);
        if (y > best)
            best = y;
    }
    return best;
}
