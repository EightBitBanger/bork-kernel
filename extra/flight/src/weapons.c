/*
 * weapons.c: your weapons.
 *
 * Two kinds at a time, picked in the menu with the aircraft (a few of
 * each: weapon_types below, times the aircraft's load):
 *
 *   AIM-9 Sidewinder    heat-seeking air-to-air, short range, agile
 *   AIM-120 AMRAAM      radar air-to-air, long range (flares don't fool it)
 *   AGM-65 Maverick     air-to-ground missile: targets, ships, SAM sites
 *   AGM-88 HARM         anti-radiation: only radars (SAM and radar sites,
 *                       warships), from far off, fast
 *   GBU-12 Paveway II   laser-guided bomb: no motor, it glides on the
 *                       speed and height you drop it with
 *   GBU-31 JDAM         GPS-guided 2000 lb bomb: glides farther, and
 *                       wrecks everything round where it hits
 *   Tactical nuke       a Maverick-like missile that locks from as far
 *                       as anything does (SEARCH_RANGE), and wipes out
 *                       everything for kilometres round where it goes
 *                       off: stay well clear (NUKE_PLAYER)
 *
 * W arms the other kind. The armed kind looks for something to lock within
 * its cone and range (a bomb's range grows with height and speed; its cone
 * is round your heading, as it falls ahead), and locks on after LOCK_DELAY
 * with it there; T moves on to the next. Enter (or a click) fires at what
 * is locked. What is hit is destroyed for the rest of the flight. Landing
 * at a friendly base or carrier loads a full set again, chaff and flares
 * too: C drops chaff (it fools radar-guided SAMs), F a flare (it fools
 * heat seekers).
 *
 * Everything destroyed scores for the sortie (sortie_add, SCORE_* in
 * flight.h): enemy fighters, ships, SAM and radar sites and a map's targets;
 * a friendly one (a blast that catches it) costs as much.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "flight.h"

#define LOCK_DELAY      1.0f        // Seconds in the cone to lock
#define MAX_CANDIDATES  32
#define MAX_DEAD_SITES  128
#define BOMB_FLOOR      150.0f      // A bomb needs this much height to glide anywhere
#define SEARCH_RANGE    30000.0f    // SAM sites are looked for this far
#define NUKE_BLAST      3000.0f     // A nuke destroys every target, site and ship this close ...
#define NUKE_AIR        4500.0f     // ... downs every aircraft this close ...
#define NUKE_PLAYER     3500.0f     // ... and you, this close

const weapon_type_t weapon_types[WPN_TYPES] = {
    //  name, tag, about
    //  missile, count, air, bomb, range, cone, blast, call
    [WPN_AIM9]     = { "AIM-9 Sidewinder", "AIM-9", "Heat seeker, 8 km",
                       MSL_AAM, 6, true, false, 8000, 0.766f, 0, "FOX 2" },
    [WPN_AIM120]   = { "AIM-120 AMRAAM", "AIM-120", "Radar, 18 km, beats flares",
                       MSL_AMRAAM, 4, true, false, 18000, 0.866f, 0, "FOX 3" },
    [WPN_MAVERICK] = { "AGM-65 Maverick", "AGM-65", "Ground missile, 12 km",
                       MSL_AGM, 4, false, false, 12000, 0.82f, 0, "RIFLE" },
    [WPN_HARM]     = { "AGM-88 HARM", "HARM", "Hunts radars, 25 km",
                       MSL_HARM, 4, false, false, 25000, 0.707f, 0, "MAGNUM" },
    [WPN_PAVEWAY]  = { "GBU-12 Paveway II", "GBU-12", "Glide bomb: drop high, fast",
                       MSL_PAVEWAY, 4, false, true, 18000, 0.866f, 0, "PICKLE" },
    [WPN_JDAM]     = { "GBU-31 JDAM", "GBU-31", "Big glide bomb, wide blast",
                       MSL_JDAM, 4, false, true, 24000, 0.866f, 180, "PICKLE" },
    [WPN_NUKE]     = { "Tactical nuke", "NUKE", "Missile, 30 km, vast blast",
                       MSL_NUKE, 2, false, false, SEARCH_RANGE, 0.707f, NUKE_BLAST, "NUKE AWAY" },
};

int loadout[LOAD_SLOTS] = { WPN_AIM9, WPN_MAVERICK };

weapons_t weapons;
nuke_t    nuke;

static struct { int32_t ci, cj; } dead_sites[MAX_DEAD_SITES];
static int dead_site_count;

void weapons_say(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(weapons.message, sizeof(weapons.message), fmt, ap);
    va_end(ap);
    weapons.message_time = world_time > 0 ? world_time : 0.001f;
}

bool player_missile(int kind) {
    return kind != MSL_SAM && kind != MSL_ENEMY_AAM;
}

int weapon_capacity(int type) {
    return type < 0 ? 0 : weapon_types[type].count * (craft->load > 0 ? craft->load : 1);
}

float weapon_range(int type) {
    const weapon_type_t* w = &weapon_types[type];
    if (!w->bomb)
        return w->range;
    float agl = plane.pos.y - terrain_height(plane.pos.x, plane.pos.z);
    if (agl < BOMB_FLOOR)
        return 0;
    float glide = type == WPN_JDAM ? 5.0f : 4.5f, carry = type == WPN_JDAM ? 15.0f : 12.0f;
    float r = agl * glide + sim_v(plane.speed) * carry;
    return r < w->range ? r : w->range;
}

static void fill(void) {
    for (int k = 0; k < LOAD_SLOTS; k++) {
        weapons.type[k] = loadout[k];
        weapons.left[k] = weapon_capacity(loadout[k]);
    }
    weapons.chaff = craft->chaff;
    weapons.flares = craft->flares;
}

void weapons_reset(void) {
    memset(&weapons, 0, sizeof(weapons));
    if (loadout[0] < 0) {                       // Never empty-handed
        loadout[0] = loadout[1] >= 0 ? loadout[1] : WPN_AIM9;
        loadout[1] = -1;
    }
    if (loadout[1] == loadout[0])
        loadout[1] = -1;
    fill();
    weapons.armed = 0;
    nuke.active = false;
    dead_site_count = 0;
    for (int i = 0; i < map.target_count; i++)
        map.targets[i].destroyed = false;
}

bool weapons_rearm(void) {
    bool full = weapons.chaff == craft->chaff && weapons.flares == craft->flares;
    for (int k = 0; k < LOAD_SLOTS; k++)
        full = full && weapons.left[k] == weapon_capacity(weapons.type[k]);
    if (full)
        return false;
    fill();
    return true;
}

bool sam_destroyed(int32_t ci, int32_t cj) {
    for (int i = 0; i < dead_site_count; i++)
        if (dead_sites[i].ci == ci && dead_sites[i].cj == cj)
            return true;
    return false;
}

//
// Targets
//

bool target_state(const target_t* t, vec3* pos, vec3* vel) {
    *vel = v3(0, 0, 0);
    switch (t->kind) {
        case TGT_PLAYER:
            *pos = plane.pos;
            *vel = vscale(plane.fwd, sim_v(plane.speed));     // Over the ground (SIM_SPEED)
            return !plane.crashed;
        case TGT_BANDIT: {
            const bandit_t* b = &bandits[t->index];
            if (!b->active || b->id != t->id || b->state == BANDIT_FALLING)
                return false;
            *pos = b->pos;
            *vel = vscale(b->fwd, sim_v(b->speed));
            return true;
        }
        case TGT_MAP: {
            if (t->index < 0 || t->index >= map.target_count || map.targets[t->index].destroyed)
                return false;
            *pos = v3(map.targets[t->index].x, 6, map.targets[t->index].z);
            return true;
        }
        case TGT_SHIP: {
            const ship_t* s = &ships[t->index];
            if (t->index < 0 || t->index >= ship_count || !s->active || s->sunk)
                return false;
            *pos = vadd(s->pos, v3(0, ship_deck(s->kind) * 0.7f, 0));
            *vel = vscale(s->fwd, sim_v(s->speed));
            return true;
        }
        case TGT_SAM: {
            sam_site_t s[64];
            int n = sams_near(plane.pos.x, plane.pos.z, SEARCH_RANGE * 2, s, 64);
            for (int i = 0; i < n; i++) {
                if (s[i].ci == t->ci && s[i].cj == t->cj) {
                    *pos = v3(s[i].x, 6, s[i].z);
                    return !sam_destroyed(t->ci, t->cj);
                }
            }
            return false;
        }
    }
    return false;
}

void target_name(const target_t* t, char* out, size_t size) {
    switch (t->kind) {
        case TGT_BANDIT:
            snprintf(out, size, "%s", bandits[t->index].name ? bandits[t->index].name : "Fighter");
            return;
        case TGT_MAP:
            snprintf(out, size, "%s", map.targets[t->index].name);
            return;
        case TGT_SHIP:
            snprintf(out, size, "%s", ships[t->index].name);
            return;
        case TGT_SAM: {
            sam_site_t s[64];
            int n = sams_near(plane.pos.x, plane.pos.z, SEARCH_RANGE * 2, s, 64);
            for (int i = 0; i < n; i++) {
                if (s[i].ci == t->ci && s[i].cj == t->cj) {
                    snprintf(out, size, "%s %s", s[i].launchers ? "SAM" : "Radar", s[i].name ? s[i].name : "site");
                    return;
                }
            }
            snprintf(out, size, "SAM site");
            return;
        }
    }
    snprintf(out, size, "?");
}

static void kill_site(int32_t ci, int32_t cj);

// The points for a SAM or radar site there (enemy: +, friendly: -), 0 if gone
static int site_points(int32_t ci, int32_t cj) {
    if (sam_destroyed(ci, cj))
        return 0;
    sam_site_t s[64];
    int n = sams_near(plane.pos.x, plane.pos.z, SEARCH_RANGE * 2, s, 64);
    for (int i = 0; i < n; i++)
        if (s[i].ci == ci && s[i].cj == cj) {
            int p = s[i].launchers ? SCORE_SAM : SCORE_RADAR;
            return s[i].enemy ? p : -p;
        }
    return SCORE_SAM;
}

void target_hit(const target_t* t, vec3 where) {
    char name[64];
    target_name(t, name, sizeof(name));
    int pts = 0;
    switch (t->kind) {
        case TGT_BANDIT: {
            const bandit_t* b = &bandits[t->index];
            if (b->active && b->state != BANDIT_FALLING)
                pts = SCORE_FIGHTER;
            bandit_hit(t->index);
            weapons_say("SPLASH - %s down  +%d", name, pts);
            break;
        }
        case TGT_MAP: {
            map_target_t* g = &map.targets[t->index];
            if (!g->destroyed)
                pts = g->enemy ? SCORE_TARGET : -SCORE_TARGET;
            g->destroyed = true;
            weapons_say("%s destroyed  %+d", name, pts);
            break;
        }
        case TGT_SHIP: {
            const ship_t* sh = &ships[t->index];
            if (sh->active && !sh->sunk)
                pts = sh->enemy ? SCORE_SHIP : -SCORE_SHIP;
            ship_hit(t->index);
            weapons_say("%s hit - sinking  %+d", name, pts);
            break;
        }
        case TGT_SAM:
            pts = site_points(t->ci, t->cj);
            kill_site(t->ci, t->cj);
            weapons_say("%s destroyed  %+d", name, pts);
            break;
        default:
            break;
    }
    sortie_add(pts);
    blast_at(where);
}

static void kill_site(int32_t ci, int32_t cj) {
    if (dead_site_count < MAX_DEAD_SITES && !sam_destroyed(ci, cj)) {
        dead_sites[dead_site_count].ci = ci;
        dead_sites[dead_site_count++].cj = cj;
    }
}

void weapons_area_hit(vec3 where, float radius) {
    int n = 0, pts = 0;
    for (int i = 0; i < map.target_count; i++) {
        map_target_t* t = &map.targets[i];
        float dx = t->x - where.x, dz = t->z - where.z, r = radius + t->radius;
        if (!t->destroyed && dx * dx + dz * dz < r * r) {
            t->destroyed = true;
            pts += t->enemy ? SCORE_TARGET : -SCORE_TARGET;
            n++;
        }
    }
    sam_site_t s[64];
    int count = sams_near(where.x, where.z, radius + SAM_RING, s, 64);
    for (int i = 0; i < count; i++) {
        float dx = s[i].x - where.x, dz = s[i].z - where.z, r = radius + SAM_RING;
        if (!sam_destroyed(s[i].ci, s[i].cj) && dx * dx + dz * dz < r * r) {
            int p = s[i].launchers ? SCORE_SAM : SCORE_RADAR;
            pts += s[i].enemy ? p : -p;
            kill_site(s[i].ci, s[i].cj);
            n++;
        }
    }
    for (int i = 0; i < ship_count; i++) {
        const ship_t* sh = &ships[i];
        float dx = sh->pos.x - where.x, dz = sh->pos.z - where.z, r = radius + ship_length(sh->kind) * 0.5f;
        if (sh->active && !sh->sunk && dx * dx + dz * dz < r * r) {
            pts += sh->enemy ? SCORE_SHIP : -SCORE_SHIP;
            ship_hit(i);
            n++;
        }
    }
    // A ring of fire round it
    for (int k = 0; k < 6; k++) {
        float a = (float)k * 1.047f;
        blast_at(vadd(where, v3(cosf(a) * radius * 0.5f, 4, sin_f(a) * radius * 0.5f)));
    }
    sortie_add(pts);
    if (n > 1)
        weapons_say("%d targets destroyed  %+d", n, pts);
}

void nuke_detonate(vec3 where) {
    nuke.active = true;
    nuke.pos = where;
    nuke.age = 0;
    int downed = 0;
    for (int i = 0; i < MAX_BANDITS; i++) {
        const bandit_t* b = &bandits[i];
        if (b->active && b->state != BANDIT_FALLING && vlen(vsub(b->pos, where)) < NUKE_AIR) {
            bandit_hit(i);
            downed++;
        }
    }
    sortie_add(downed * SCORE_FIGHTER);
    weapons_area_hit(where, NUKE_BLAST);
    if (downed)
        weapons_say("NUCLEAR DETONATION - %d aircraft down", downed);
    else
        weapons_say("NUCLEAR DETONATION");
    if (!plane.crashed && vlen(vsub(plane.pos, where)) < NUKE_PLAYER)
        crash("Caught in the nuclear blast");
}

//
// Locking on
//

typedef struct {
    target_t t;
    float    cos;                   // Of its angle off the nose
} candidate_t;

static candidate_t candidates[MAX_CANDIDATES];
static int         candidate_count;

// Within range and the cone: off the nose, or (flat) off your heading
static void consider(target_t t, vec3 pos, float range, float min_cos, bool flat) {
    vec3 d = vsub(pos, plane.pos);
    float dist = vlen(d);
    if (dist > range || dist < 50 || candidate_count >= MAX_CANDIDATES)
        return;
    float c;
    if (flat) {
        float hd = sqrtf(d.x * d.x + d.z * d.z), hf = sqrtf(plane.fwd.x * plane.fwd.x + plane.fwd.z * plane.fwd.z);
        c = hd < 1 || hf < 1e-3f ? 1 : (d.x * plane.fwd.x + d.z * plane.fwd.z) / (hd * hf);
    } else {
        c = vdot(d, plane.fwd) / dist;
    }
    if (c < min_cos)
        return;
    candidates[candidate_count].t = t;
    candidates[candidate_count++].cos = c;
}

static bool same_target(const target_t* a, const target_t* b) {
    if (a->kind != b->kind)
        return false;
    switch (a->kind) {
        case TGT_BANDIT: return a->index == b->index && a->id == b->id;
        case TGT_MAP:    return a->index == b->index;
        case TGT_SHIP:   return a->index == b->index;
        case TGT_SAM:    return a->ci == b->ci && a->cj == b->cj;
    }
    return true;
}

// What the armed kind could lock now, nearest the nose first
static void find_candidates(void) {
    candidate_count = 0;
    if (plane.crashed)
        return;
    int type = weapons.type[weapons.armed];
    if (type < 0)
        return;
    const weapon_type_t* w = &weapon_types[type];
    float range = weapon_range(type);
    if (w->air) {
        for (int i = 0; i < MAX_BANDITS; i++) {
            const bandit_t* b = &bandits[i];
            if (b->active && b->state != BANDIT_FALLING)
                consider((target_t){ .kind = TGT_BANDIT, .index = i, .id = b->id }, b->pos, range, w->cone, false);
        }
    } else if (range > 0) {
        bool harm = type == WPN_HARM;
        for (int i = 0; i < map.target_count && !harm; i++) {
            const map_target_t* g = &map.targets[i];
            if (g->enemy && !g->destroyed)
                consider((target_t){ .kind = TGT_MAP, .index = i }, v3(g->x, 6, g->z), range, w->cone, w->bomb);
        }
        for (int i = 0; i < ship_count; i++) {
            const ship_t* sh = &ships[i];
            bool radar = sh->kind == SHIP_CRUISER || sh->kind == SHIP_DESTROYER || sh->kind == SHIP_FRIGATE;
            if (sh->active && sh->enemy && !sh->sunk && (radar || !harm))
                consider((target_t){ .kind = TGT_SHIP, .index = i }, vadd(sh->pos, v3(0, ship_deck(sh->kind) * 0.7f, 0)),
                         range, w->cone, w->bomb);
        }
        sam_site_t s[64];
        int n = sams_near(plane.pos.x, plane.pos.z, range, s, 64);
        for (int i = 0; i < n; i++)
            if (s[i].enemy && !sam_destroyed(s[i].ci, s[i].cj))
                consider((target_t){ .kind = TGT_SAM, .ci = s[i].ci, .cj = s[i].cj }, v3(s[i].x, 6, s[i].z),
                         range, w->cone, w->bomb);
    }
    // Sort by angle off the nose (few: insertion sort)
    for (int i = 1; i < candidate_count; i++) {
        candidate_t c = candidates[i];
        int k = i;
        while (k > 0 && candidates[k - 1].cos < c.cos) {
            candidates[k] = candidates[k - 1];
            k--;
        }
        candidates[k] = c;
    }
}

static int candidate_index(const target_t* t) {
    for (int i = 0; i < candidate_count; i++)
        if (same_target(&candidates[i].t, t))
            return i;
    return -1;
}

void weapons_update(float dt) {
    if (nuke.active && (nuke.age += dt) > NUKE_LIFE)
        nuke.active = false;
    find_candidates();
    if (weapons.lock.kind != TGT_NONE && candidate_index(&weapons.lock) >= 0) {
        weapons.lock_time += dt;
    } else if (candidate_count && weapons.left[weapons.armed] > 0) {
        weapons.lock = candidates[0].t;
        weapons.lock_time = 0;
    } else {
        weapons.lock.kind = TGT_NONE;
        weapons.lock_time = 0;
    }
    weapons.locked = weapons.lock.kind != TGT_NONE && weapons.lock_time >= LOCK_DELAY;
}

void weapons_select(void) {
    if (weapons.type[1 - weapons.armed] >= 0)
        weapons.armed = 1 - weapons.armed;
    weapons.lock.kind = TGT_NONE;
    weapons.lock_time = 0;
    weapons.locked = false;
}

void weapons_next_target(void) {
    find_candidates();
    if (!candidate_count)
        return;
    int i = candidate_index(&weapons.lock);
    weapons.lock = candidates[(i + 1) % candidate_count].t;
    weapons.lock_time = 0;
    weapons.locked = false;
}

void weapons_fire(void) {
    if (plane.crashed || plane.on_ground)
        return;
    int type = weapons.type[weapons.armed];
    if (type < 0)
        return;
    const weapon_type_t* w = &weapon_types[type];
    if (weapons.left[weapons.armed] <= 0) {
        weapons_say("No %s left", w->tag);
        return;
    }
    if (!weapons.locked) {
        weapons_say(w->bomb && weapon_range(type) <= 0 ? "Too low to drop" : "No lock");
        return;
    }
    // Off the rail under the aircraft, along the nose; a bomb just lets go
    vec3 pos = vadd(vadd(plane.pos, vscale(plane.up, w->bomb ? -2.0f : -2.5f)), vscale(plane.fwd, w->bomb ? 0 : 4));
    vec3 dir = w->air ? plane.fwd : vnorm(vadd(plane.fwd, vscale(plane.up, -0.05f)));
    if (!missile_new(w->missile, pos, dir, plane.speed, weapons.lock))
        return;
    weapons.left[weapons.armed]--;
    weapons_say("%s", w->call);
}

//
// Countermeasures: chaff for radar missiles, flares for heat seekers
// (sam.c decides what each fools)
//

static void drop(int kind, int* left, const char* name) {
    if (plane.crashed || plane.on_ground)
        return;
    if (*left <= 0) {
        weapons_say("No %s left", name);
        return;
    }
    vec3 pos = vsub(plane.pos, vscale(plane.fwd, 10));
    vec3 vel = vadd(vscale(plane.fwd, plane.speed * 0.6f), vscale(plane.up, -15));
    if (decoy_drop(kind, pos, vel, true))
        (*left)--;
}

void weapons_chaff(void) {
    drop(DECOY_CHAFF, &weapons.chaff, "chaff");
}

void weapons_flare(void) {
    drop(DECOY_FLARE, &weapons.flares, "flares");
}
