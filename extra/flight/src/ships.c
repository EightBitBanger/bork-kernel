/*
 * ships.c: carriers, warships, missile boats and tankers.
 *
 * A map puts ships at sea ("carrier, USS Nimitz, x, y, heading 30" and
 * so on; a carrier brings its group of escorts, any other ship can bring
 * missile boats). Each sails at its own speed, turning away well before it
 * or its group would run aground or off the map and back toward where it
 * started when it has gone SHIP_LEG; escorts keep station round it.
 *
 * A carrier's deck is real size: a straight deck down the middle and the
 * landing deck angled DECK_ANGLE to port, with four arresting wires across
 * it near the stern. Touch down on or just short of them with the gear
 * (and hook) down, lined up with the landing deck, and a wire catches the
 * aircraft and stops it in ARREST_RUN; land past them and it's a bolter:
 * full power and fly off the end. Stopped on a friendly carrier's deck it
 * is refueled and rearmed like at a base, the deck carries it along as
 * the ship sails, and full power fires the catapult (aircraft.c).
 *
 * Enemy cruisers, destroyers and frigates fire radar SAMs (chaff fools
 * them) when their radar has you; all enemy ships can be sunk with
 * air-to-ground missiles.
 */

#include <stdio.h>
#include <string.h>

#include "flight.h"

#define SHIP_LEG        15000.0f    // Turns back past this far from where it started
#define SHIP_LOOK       1500.0f     // Turns away from land or the map's edge this far ahead
#define SHIP_SAM_GAP    10.0f       // Seconds between a warship's missiles
#define SHIP_SAM_FLOOR  40.0f       // Its radar sees you above this
#define SHIP_SINK_RATE  0.35f       // m/s down, once hit
#define WIRE_LINE_UP    0.35f       // Radians off the landing deck's line the hook still catches

typedef struct {
    const char* key;                // Its keyword in a map
    const char* label;
    float       length, beam, deck; // Meters; deck: its height above the water
    float       speed, turn;        // m/s, rad/s
    float       sam_range;          // An enemy one's radar and missiles (0: none)
} ship_kind_t;

static const ship_kind_t kinds[SHIP_KINDS] = {
    [SHIP_CARRIER]      = { "carrier",     "carrier",      333, 41, 19,  10, 0.020f, 0 },
    [SHIP_CRUISER]      = { "cruiser",     "cruiser",      173, 17,  8,  12, 0.030f, 14000 },
    [SHIP_DESTROYER]    = { "destroyer",   "destroyer",    155, 20,  7,  13, 0.035f, 11000 },
    [SHIP_FRIGATE]      = { "frigate",     "frigate",      135, 14,  6,  12, 0.040f, 8000 },
    [SHIP_MISSILE_BOAT] = { "missileboat", "missile boat",  39,  8,  2.5f, 16, 0.080f, 0 },
    [SHIP_TANKER]       = { "tanker",      "tanker",       330, 58, 10,   7, 0.015f, 0 },
};

// A carrier group's escorts, kind and station (ahead, to the right): the
// first ones picked first, missile boats close in
static const struct { int kind; float a, c; } escorts[] = {
    { SHIP_CRUISER, 1800, -900 }, { SHIP_DESTROYER, -1500, 1200 }, { SHIP_MISSILE_BOAT, 500, -700 },
    { SHIP_MISSILE_BOAT, -400, 800 }, { SHIP_DESTROYER, 600, 1700 }, { SHIP_FRIGATE, -1800, -1400 },
    { SHIP_FRIGATE, 2600, 900 }, { SHIP_DESTROYER, -600, -1900 },
};
// Any other ship's escorts: missile boats
static const struct { float a, c; } boat_stations[] = {
    { -300, 250 }, { -300, -250 }, { 400, 350 }, { 400, -350 }, { -700, 0 }, { 800, 0 },
};
static const char* escort_names[] = {
    "Ticonderoga", "Spruance", "Arleigh Burke", "Oliver Perry", "Belknap", "Kidd", "Leahy", "Knox",
    "Farragut", "Coontz", "Truxtun", "Mitscher",
};
static const char* boat_names[] = { "Pegasus", "Hercules", "Taurus", "Aquila", "Aries", "Gemini" };

ship_t ships[MAX_SHIPS];
int    ship_count;
static int  flagship[MAX_SHIPS];   // map line -> ship
static char names[MAX_SHIPS][40];

int ship_kind_of(const char* key) {
    for (int i = 0; i < SHIP_KINDS; i++)
        if (!strcasecmp(kinds[i].key, key))
            return i;
    return -1;
}

const char* ship_label(int kind) { return kinds[kind].label; }
float ship_length(int kind) { return kinds[kind].length; }
float ship_deck(int kind) { return kinds[kind].deck; }
float ship_beam(int kind) { return kinds[kind].beam; }

static void set_heading(ship_t* s, float h) {
    while (h < 0) h += 6.2831853f;
    while (h >= 6.2831853f) h -= 6.2831853f;
    s->heading = h;
    s->fwd = v3(sin_f(h), 0, cosf(h));
    s->right = v3(cosf(h), 0, -sin_f(h));
}

static ship_t* new_ship(int kind, bool enemy, float x, float z, float heading) {
    if (ship_count >= MAX_SHIPS)
        return NULL;
    ship_t* s = &ships[ship_count];
    memset(s, 0, sizeof(*s));
    s->active = true;
    s->kind = kind;
    s->enemy = enemy;
    s->pos = v3(x, 0, z);
    s->speed = kinds[kind].speed;
    s->leader = -1;
    s->home_x = x;
    s->home_z = z;
    s->name = names[ship_count];
    s->shot_timer = SHIP_SAM_GAP * 0.5f;
    set_heading(s, heading);
    ship_count++;
    return s;
}

void ships_reset(void) {
    ship_count = 0;
    int esc = 0, boats = 0;
    for (int i = 0; i < map.ship_count && i < MAX_SHIPS; i++) {
        const map_ship_t* m = &map.ships[i];
        flagship[i] = -1;
        ship_t* s = new_ship(m->kind, m->enemy, m->x, m->z, m->heading * ((float)M_PI / 180.0f));
        if (!s)
            break;
        int lead = ship_count - 1;
        flagship[i] = lead;
        snprintf(names[lead], sizeof(names[0]), "%s", m->name[0] ? m->name : kinds[m->kind].label);
        bool group = m->kind == SHIP_CARRIER;
        int most = group ? (int)COUNT(escorts) : (int)COUNT(boat_stations);
        for (int k = 0; k < m->escorts && k < most; k++) {
            int kind = group ? escorts[k].kind : SHIP_MISSILE_BOAT;
            ship_t* e = new_ship(kind, m->enemy, m->x, m->z, s->heading);
            if (!e)
                break;
            e->leader = lead;
            e->station_a = group ? escorts[k].a : boat_stations[k].a;
            e->station_c = group ? escorts[k].c : boat_stations[k].c;
            const ship_t* l = &ships[lead];
            e->pos = vadd(l->pos, vadd(vscale(l->fwd, e->station_a), vscale(l->right, e->station_c)));
            float r = sqrtf(e->station_a * e->station_a + e->station_c * e->station_c) + kinds[kind].length;
            if (r > s->spread)
                s->spread = r;
            char* name = names[ship_count - 1];
            if (m->enemy)
                snprintf(name, sizeof(names[0]), "Enemy %s", kinds[kind].label);
            else if (kind == SHIP_MISSILE_BOAT)
                snprintf(name, sizeof(names[0]), "USS %s", boat_names[boats++ % COUNT(boat_names)]);
            else
                snprintf(name, sizeof(names[0]), "USS %s", escort_names[esc++ % COUNT(escort_names)]);
        }
    }
}

int ship_flagship(int map_index) {
    return map_index >= 0 && map_index < map.ship_count && map_index < MAX_SHIPS ? flagship[map_index] : -1;
}

//
// Where the decks are
//

static void to_local(const ship_t* s, float x, float z, float* a, float* c) {
    float dx = x - s->pos.x, dz = z - s->pos.z;
    *a = dx * s->fwd.x + dz * s->fwd.z;
    *c = dx * s->right.x + dz * s->right.z;
}

// The landing deck's own coordinates: from its stern end along it (s) and
// across it (t, + to starboard)
#define ANGLED_A0       -158.0f
#define ANGLED_C0       -6.0f
#define ANGLED_LEN      230.0f
static void to_angled(float a, float c, float* s, float* t) {
    float ua = cosf(DECK_ANGLE), uc = -sin_f(DECK_ANGLE);
    float da = a - ANGLED_A0, dc = c - ANGLED_C0;
    *s = da * ua + dc * uc;
    *t = -da * uc + dc * ua;
}

static bool on_carrier_deck(float a, float c) {
    if (a >= -160 && a <= 165 && c >= -18 && c <= 24)
        return true;
    float s, t;
    to_angled(a, c, &s, &t);
    return s >= 0 && s <= ANGLED_LEN && fabsf(t) <= LANDING_HALF_W;
}

int ship_deck_at(float x, float z, float* deck_y) {
    for (int i = 0; i < ship_count; i++) {
        const ship_t* s = &ships[i];
        if (!s->active || s->sunk || s->kind != SHIP_CARRIER)
            continue;
        if (fabsf(x - s->pos.x) > 200 || fabsf(z - s->pos.z) > 200)
            continue;
        float a, c;
        to_local(s, x, z, &a, &c);
        if (on_carrier_deck(a, c)) {
            *deck_y = s->pos.y + kinds[s->kind].deck;
            return i;
        }
    }
    return -1;
}

bool ship_solid_at(vec3 p, const char** why) {
    for (int i = 0; i < ship_count; i++) {
        const ship_t* s = &ships[i];
        const ship_kind_t* k = &kinds[s->kind];
        if (!s->active || fabsf(p.x - s->pos.x) > k->length || fabsf(p.z - s->pos.z) > k->length)
            continue;
        float a, c, y = p.y - s->pos.y;
        to_local(s, p.x, p.z, &a, &c);
        if (s->kind == SHIP_CARRIER) {
            if (y < k->deck - 0.5f && y > -2 && (on_carrier_deck(a, c) || (a > -160 && a < k->length / 2 && fabsf(c) < k->beam / 2))) {
                *why = "Flew into the carrier";
                return true;
            }
            if (a > 8 && a < 52 && c > 16 && c < 26 && y < k->deck + 23 && y > 0) {
                *why = "Hit the carrier's island";
                return true;
            }
        } else if (fabsf(a) < k->length / 2 && fabsf(c) < k->beam / 2 && y < k->deck * 2 + 4 && y > -2) {
            *why = "Flew into a ship";
            return true;
        }
    }
    return false;
}

bool ship_wire(int i, float x, float z, vec3 dir, int* wire, float* along) {
    const ship_t* sh = &ships[i];
    float a, c, s, t;
    to_local(sh, x, z, &a, &c);
    to_angled(a, c, &s, &t);
    *along = s;
    if (s < WIRE_FIRST - 8 || s > WIRE_LAST + 8 || fabsf(t) > 11)
        return false;
    // Lined up with the landing deck?
    float dh = atan2_f(dir.x, dir.z) - (sh->heading - DECK_ANGLE);
    while (dh > 3.14159265f) dh -= 6.2831853f;
    while (dh < -3.14159265f) dh += 6.2831853f;
    if (fabsf(dh) > WIRE_LINE_UP)
        return false;
    int w = (int)((s - WIRE_FIRST) / 12 + 1.5f);
    *wire = w < 1 ? 1 : w > 4 ? 4 : w;
    return true;
}

void ship_angled_point(int i, float s, float t, vec3* pos) {
    float ua = cosf(DECK_ANGLE), uc = -sin_f(DECK_ANGLE);
    ship_deck_point(i, ANGLED_A0 + s * ua - t * uc, ANGLED_C0 + s * uc + t * ua, pos);
}

void ship_deck_point(int i, float a, float c, vec3* pos) {
    const ship_t* s = &ships[i];
    *pos = vadd(s->pos, vadd(vscale(s->fwd, a), vscale(s->right, c)));
    pos->y = s->pos.y + kinds[s->kind].deck;
}

//
// Sailing
//

static bool open_water(float x, float z) {
    return map.loaded && map_inside(x, z) && map_water_at(x, z) && terrain_height(x, z) <= 0;
}

// Open water along heading h out to look, wide enough for the ship's group
static bool clear_ahead(const ship_t* s, float h, float look) {
    vec3 f = v3(sin_f(h), 0, cosf(h)), r = v3(f.z, 0, -f.x);
    float w = s->spread + ship_beam(s->kind);
    for (int k = 1; k <= 3; k++) {
        vec3 p = vadd(s->pos, vscale(f, look * (float)k / 3));
        if (!open_water(p.x, p.z))
            return false;
        if (s->spread > 0 && (!open_water(p.x + r.x * w, p.z + r.z * w) || !open_water(p.x - r.x * w, p.z - r.z * w)))
            return false;
    }
    return true;
}

static void sail(ship_t* s, float dt) {
    const ship_kind_t* k = &kinds[s->kind];
    if (s->leader >= 0) {
        // On station round the carrier
        const ship_t* l = &ships[s->leader];
        set_heading(s, l->heading);
        s->speed = l->speed;
        s->turn = l->turn;
        vec3 p = vadd(l->pos, vadd(vscale(l->fwd, s->station_a), vscale(l->right, s->station_c)));
        s->pos.x = p.x;
        s->pos.z = p.z;
        return;
    }
    float look = (SHIP_LOOK > k->length * 3 ? SHIP_LOOK : k->length * 3) + s->spread;
    float leg = s->kind == SHIP_MISSILE_BOAT && s->spread == 0 ? SHIP_LEG * 0.4f : SHIP_LEG;
    float want = 0;                 // Turn: -1 left, +1 right
    if (!clear_ahead(s, s->heading, look)) {
        // Land or the edge ahead: turn toward the side with more water, and
        // keep turning that way till it's clear
        if (!s->avoid) {
            float best = 0;
            for (int side = -1; side <= 1; side += 2) {
                float score = (clear_ahead(s, s->heading + (float)side * 0.7f, look) ? 2.0f : 0.0f) +
                              (clear_ahead(s, s->heading + (float)side * 1.4f, look) ? 1.0f : 0.0f);
                if (score > best || (score == best && side > 0 && best == 0)) {
                    best = score;
                    s->avoid = side;
                }
            }
            if (!s->avoid)
                s->avoid = 1;
        }
        want = (float)s->avoid;
    } else {
        // Too far from where it patrols: head back
        s->avoid = 0;
        float dx = s->home_x - s->pos.x, dz = s->home_z - s->pos.z;
        if (dx * dx + dz * dz > leg * leg && dx * s->fwd.x + dz * s->fwd.z < 0) {
            want = dx * s->right.x + dz * s->right.z > 0 ? 1.0f : -1.0f;
            if (!clear_ahead(s, s->heading + want * 0.7f, look))
                want = 0;           // Not toward land: on round the coast first
        }
    }
    s->turn = approach(s->turn, want * k->turn, k->turn * 0.5f * dt);
    set_heading(s, s->heading + s->turn * dt);
    vec3 bow = vadd(s->pos, vscale(s->fwd, k->length * 0.5f + 20));
    if (open_water(bow.x, bow.z) || !map.loaded)
        s->pos = vadd(s->pos, vscale(s->fwd, s->speed * dt));      // (Else it stops, and turns till it can go)
}

// Carry the aircraft on its deck: turned with the ship, and moved with it
static void carry(const ship_t* s, vec3 old_pos, float turned) {
    float c = cosf(turned), sn = sin_f(turned);
    #define TURN_XZ(v) do { float vx = (v).x, vz = (v).z; (v).x = vx * c + vz * sn; (v).z = vz * c - vx * sn; } while (0)
    vec3 off = vsub(plane.pos, old_pos);
    TURN_XZ(off);
    plane.pos.x = s->pos.x + off.x;
    plane.pos.z = s->pos.z + off.z;
    TURN_XZ(plane.ground_fwd);
    TURN_XZ(plane.ground_right);
    TURN_XZ(plane.fwd);
    TURN_XZ(plane.right);
    TURN_XZ(plane.up);
    #undef TURN_XZ
}

//
// Fighting
//

bool ship_tracking(int i) {
    return i >= 0 && i < ship_count && ships[i].active && !ships[i].sunk && ships[i].tracking;
}

static void fight(ship_t* s, int index, float dt) {
    const ship_kind_t* k = &kinds[s->kind];
    s->tracking = false;
    if (!s->enemy || s->sunk || k->sam_range <= 0 || !sams_enabled || plane.crashed || plane.on_ground)
        return;
    vec3 d = vsub(plane.pos, s->pos);
    float dist = vlen(d);
    float ground = terrain_height(plane.pos.x, plane.pos.z);
    s->tracking = dist < seen_range(k->sam_range) && plane.pos.y - ground > SHIP_SAM_FLOOR;
    if (s->shot_timer > 0)
        s->shot_timer -= dt;
    if (!s->tracking || s->shot_timer > 0 || enemy_missiles() >= MAX_INBOUND)
        return;
    vec3 from = vadd(s->pos, v3(0, k->deck + 4, 0));
    missile_t* m = missile_new(MSL_SAM, from, vnorm(v3(d.x * 0.3f, dist * 0.6f, d.z * 0.3f)), 40,
                               (target_t){ .kind = TGT_PLAYER });
    if (m) {
        m->site_ci = index;
        m->site_cj = SHIP_SITE;
        s->shot_timer = SHIP_SAM_GAP;
    }
}

void ship_hit(int i) {
    ship_t* s = &ships[i];
    if (!s->active || s->sunk)
        return;
    s->sunk = true;
    s->sink_time = 0;
    s->speed *= 0.3f;
    blast_at(vadd(s->pos, v3(0, kinds[s->kind].deck, 0)));
}

void ships_update(float dt) {
    for (int i = 0; i < ship_count; i++) {
        ship_t* s = &ships[i];
        if (!s->active)
            continue;
        vec3 old_pos = s->pos;
        float old_heading = s->heading;
        if (s->sunk) {
            // Burning and settling, then gone
            s->sink_time += dt;
            s->speed = approach(s->speed, 0, 0.5f * dt);
            s->pos = vadd(s->pos, vscale(s->fwd, s->speed * dt));
            s->pos.y = -s->sink_time * SHIP_SINK_RATE * (kinds[s->kind].length > 100 ? 1.0f : 2.0f);
            if (s->sink_time < 12 && (hash2(i, (int32_t)(s->sink_time * 4), 91) & 7) == 0 &&
                (int)(s->sink_time * 4) != (int)((s->sink_time - dt) * 4)) {
                float a = ((float)(hash2(i, (int32_t)(s->sink_time * 10), 92) & 255) / 255.0f - 0.5f) * kinds[s->kind].length;
                blast_at(vadd(s->pos, vadd(vscale(s->fwd, a), v3(0, kinds[s->kind].deck, 0))));
            }
            if (s->pos.y < -(kinds[s->kind].deck * 2 + 10))
                s->active = false;
        } else {
            sail(s, dt);
            fight(s, i, dt);
        }
        if (plane.deck == i + 1 && plane.on_ground && !plane.crashed) {
            float turned = s->heading - old_heading;
            while (turned > 3.14159265f) turned -= 6.2831853f;
            while (turned < -3.14159265f) turned += 6.2831853f;
            carry(s, old_pos, turned);
        }
    }
    // A ship sunk under you
    if (plane.deck && plane.on_ground && (plane.deck > ship_count || ships[plane.deck - 1].sunk)) {
        plane.on_ground = false;
        plane.deck = 0;
    }
}
