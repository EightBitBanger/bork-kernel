/*
 * air.c: enemy fighters.
 *
 * An enemy air base scrambles fighters when you fly within BANDIT_WAKE of
 * it: one, then another BANDIT_GAP later, and no more than BANDITS_PER_BASE
 * ever (the bases keep count), so the sky doesn't fill up. A fighter takes
 * off down the runway, climbs, and orbits its base in a big circle until it
 * spots you within BANDIT_SEE. Then it comes after you, steering for where
 * you will be, and fires heat seekers (sam.c flies them) when you are ahead
 * of it and in range. It breaks hard when one of your missiles gets close
 * (sometimes dropping a flare that may fool it),
 * gives up and goes home when you get far enough away, and falls burning
 * when hit.
 *
 * Fighters fly simply: they turn toward where they want to go, as hard as
 * their g allows, at their own speed.
 */

#include <string.h>

#include "flight.h"

#define BANDITS_PER_BASE 2
#define BANDIT_WAKE     30000.0f    // A base scrambles when you are this close (and flying)
#define BANDIT_WAKE_MIN 6000.0f     // ... even for a stealthy one (seen from the ground)
#define BANDIT_FLARE_PCT 40         // Chance a fighter drops a flare when breaking from your missile
#define BANDIT_GAP      40.0f       // Seconds between its fighters
#define BANDIT_SEE      14000.0f    // They spot you this far
#define BANDIT_LOSE     30000.0f    // ... and give up past this
#define BANDIT_ORBIT_R  5000.0f
#define BANDIT_ORBIT_ALT 3000.0f
#define BANDIT_SHOT_MAX 6000.0f     // They fire from this close ...
#define BANDIT_SHOT_MIN 700.0f      // ... but not closer
#define BANDIT_SHOT_COS 0.94f       // ... with you within 20 degrees of the nose
#define BANDIT_SHOT_GAP 12.0f
#define BANDIT_MISSILES 2
#define BANDIT_FLOOR    250.0f      // Keep this high above the ground (except taking off)
#define MAX_RECORDS     64

typedef struct {
    const char* name;
    float       speed;              // Cruising (attacking a little faster)
    float       max_g;
} bandit_type_t;

static const bandit_type_t types[] = {
    { "MiG-29", 260, 7.0f },
    { "MiG-21", 230, 6.0f },
    { "Su-27",  270, 7.5f },
    { "MiG-17", 190, 5.5f },
    { "MiG-19", 210, 6.0f },
    { "MiG-23", 250, 6.0f },
    { "Su-22",  230, 5.5f },
};
#define DEFAULT_TYPES 3             // A map without a "fighters" line: the first ones

// Bases that have scrambled: how many, and when the next may go
typedef struct {
    float x, z;
    int   launched;
    float next;
} base_record_t;

bandit_t             bandits[MAX_BANDITS];
static base_record_t records[MAX_RECORDS];
static int           record_count;
static uint32_t      next_id;

int fighter_type(const char* name) {
    for (int i = 0; i < (int)COUNT(types); i++)
        if (!strcasecmp(types[i].name, name))
            return i;
    return -1;
}

void air_reset(void) {
    memset(bandits, 0, sizeof(bandits));
    record_count = 0;
}

bool bandit_attacking(void) {
    for (int i = 0; i < MAX_BANDITS; i++)
        if (bandits[i].active && bandits[i].state == BANDIT_ATTACK)
            return true;
    return false;
}

void bandit_hit(int index) {
    bandit_t* b = &bandits[index];
    if (!b->active || b->state == BANDIT_FALLING)
        return;
    b->state = BANDIT_FALLING;
    b->state_time = 0;
    blast_at(b->pos);
}

static base_record_t* record_for(const base_t* base) {
    for (int i = 0; i < record_count; i++)
        if (fabsf(records[i].x - base->x) < 1 && fabsf(records[i].z - base->z) < 1)
            return &records[i];
    if (record_count >= MAX_RECORDS)
        return NULL;
    base_record_t* r = &records[record_count++];
    r->x = base->x;
    r->z = base->z;
    r->launched = 0;
    r->next = world_time;
    return r;
}

static void scramble(const base_t* base, base_record_t* r) {
    bandit_t* b = NULL;
    int index = 0;
    for (int i = 0; i < MAX_BANDITS && !b; i++)
        if (!bandits[i].active) {
            b = &bandits[i];
            index = i;
        }
    if (!b)
        return;
    memset(b, 0, sizeof(*b));
    b->active = true;
    b->id = ++next_id;
    uint32_t pick = hash2((int32_t)base->x, (int32_t)base->z + r->launched, 31);
    b->type = map.fighter_count ? map.fighters[pick % (uint32_t)map.fighter_count] : (int)(pick % DEFAULT_TYPES);
    b->name = types[b->type].name;
    b->state = BANDIT_CLIMB;
    // From the near end of the runway, down it, just off the ground
    float start = -(RUNWAY_HALF_LEN - 200);
    b->pos = v3(base->x + base->along.x * start, 40, base->z + base->along.z * start);
    b->fwd = vnorm(v3(base->along.x, 0.15f, base->along.z));
    b->up = v3(0, 1, 0);
    b->speed = 120;
    b->home_x = base->x;
    b->home_z = base->z;
    b->missiles = BANDIT_MISSILES;
    b->shot_timer = BANDIT_SHOT_GAP * 0.5f;
    r->launched++;
    r->next = world_time + BANDIT_GAP;
    (void)index;
}

// Enemy bases near you send up their fighters
static void wake_bases(void) {
    if (plane.on_ground || plane.crashed || !sams_enabled)
        return;
    base_t near[16];
    int n = bases_near(plane.pos.x, plane.pos.z, BANDIT_WAKE, near, 16);
    for (int i = 0; i < n; i++) {
        if (!near[i].enemy)
            continue;
        float dx = near[i].x - plane.pos.x, dz = near[i].z - plane.pos.z;
        float wake = seen_range(BANDIT_WAKE) > BANDIT_WAKE_MIN ? seen_range(BANDIT_WAKE) : BANDIT_WAKE_MIN;
        if (dx * dx + dz * dz > wake * wake)
            continue;
        base_record_t* r = record_for(&near[i]);
        if (r && r->launched < BANDITS_PER_BASE && world_time >= r->next)
            scramble(&near[i], r);
    }
}

// Turn fwd toward want, at most max_turn radians; how far it turned to the right
float turn_toward(vec3* fwd, vec3 want, float max_turn) {
    float c = clampf(vdot(*fwd, want), -1, 1);
    float angle = acosf(c);
    vec3 right = vcross(v3(0, 1, 0), *fwd);
    float side = vdot(want, right) >= 0 ? 1.0f : -1.0f;
    if (angle <= max_turn) {
        *fwd = want;
        return angle * side;
    }
    vec3 perp = vsub(want, vscale(*fwd, c));
    if (vdot(perp, perp) < 1e-8f)
        perp = right;               // Straight behind: turn right
    perp = vnorm(perp);
    *fwd = vnorm(vadd(vscale(*fwd, cosf(max_turn)), vscale(perp, sin_f(max_turn))));
    return max_turn * side;
}

// A player missile closing on this fighter
static const missile_t* threat_to(const bandit_t* b, int index) {
    for (int i = 0; i < MAX_MISSILES; i++) {
        const missile_t* m = &missiles[i];
        if (m->active && (m->kind == MSL_AAM || m->kind == MSL_AMRAAM) && m->target.kind == TGT_BANDIT && m->target.index == index &&
            vlen(vsub(m->pos, b->pos)) < 1800)
            return m;
    }
    return NULL;
}

static void fire(bandit_t* b) {
    if (enemy_missiles() >= MAX_INBOUND)
        return;
    if (missile_new(MSL_ENEMY_AAM, vadd(b->pos, vscale(b->fwd, 12)), b->fwd, b->speed + 20,
                    (target_t){ .kind = TGT_PLAYER })) {
        b->missiles--;
        b->shot_timer = BANDIT_SHOT_GAP;
    }
}

static void fly(bandit_t* b, int index, float dt) {
    const bandit_type_t* t = &types[b->type];
    b->state_time += dt;
    if (b->shot_timer > 0)
        b->shot_timer -= dt;
    if (b->evade_timer > 0)
        b->evade_timer -= dt;

    vec3 to_player = vsub(plane.pos, b->pos);
    float dist = vlen(to_player);
    bool player_up = !plane.on_ground && !plane.crashed && sams_enabled;
    float ground = terrain_height(b->pos.x, b->pos.z);

    if (b->state == BANDIT_FALLING) {
        // Out of control: nose down, rolling, until it hits
        b->fwd = vnorm(vadd(b->fwd, v3(0, -0.6f * dt, 0)));
        b->turn = 2.5f;
        b->pos = vadd(b->pos, vscale(b->fwd, sim_dist(b->speed, dt)));
        if (b->pos.y <= ground + 2) {
            blast_at(v3(b->pos.x, ground + 5, b->pos.z));
            b->active = false;
        }
        return;
    }

    // Where it wants to go
    vec3 want = b->fwd;
    float speed = t->speed;
    switch (b->state) {
        case BANDIT_CLIMB:
            want = vnorm(v3(b->fwd.x, 0.25f, b->fwd.z));
            speed = t->speed * 0.8f;
            if (b->pos.y > 1500)
                b->state = BANDIT_PATROL;
            break;
        case BANDIT_PATROL: {
            // Round the base, steering onto the circle
            float dx = b->pos.x - b->home_x, dz = b->pos.z - b->home_z;
            float r = sqrtf(dx * dx + dz * dz);
            vec3 out = r > 1 ? v3(dx / r, 0, dz / r) : v3(1, 0, 0);
            vec3 tangent = v3(-out.z, 0, out.x);
            float pull = clampf((r - BANDIT_ORBIT_R) / BANDIT_ORBIT_R, -1, 1);
            want = vnorm(vsub(tangent, vscale(out, pull)));
            want.y = clampf((BANDIT_ORBIT_ALT - b->pos.y) / 2000.0f, -0.2f, 0.2f);
            want = vnorm(want);
            if (player_up && dist < seen_range(BANDIT_SEE))
                b->state = BANDIT_ATTACK;
            break;
        }
        case BANDIT_ATTACK: {
            if (!player_up || dist > seen_range(BANDIT_LOSE)) {
                b->state = BANDIT_PATROL;
                break;
            }
            vec3 pv = vscale(plane.fwd, sim_v(plane.speed));
            float t_go = clampf(dist / (sim_v(t->speed) + 100), 0, 4);
            want = vnorm(vsub(vadd(plane.pos, vscale(pv, t_go * 0.6f)), b->pos));
            speed = t->speed * 1.15f;
            if (b->missiles > 0 && b->shot_timer <= 0 && dist < BANDIT_SHOT_MAX && dist > BANDIT_SHOT_MIN &&
                vdot(to_player, b->fwd) > BANDIT_SHOT_COS * dist)
                fire(b);
            break;
        }
    }

    // A missile coming: break hard across its path
    const missile_t* m = threat_to(b, index);
    if (m && b->evade_timer <= 0) {
        b->evade_timer = 3;
        if ((int)(hash2(index, (int32_t)(world_time * 100.0f), 43) % 100) < BANDIT_FLARE_PCT)
            decoy_drop(DECOY_FLARE, vsub(b->pos, vscale(b->fwd, 8)), vscale(b->fwd, b->speed * 0.7f), false);
    }
    if (b->evade_timer > 0 && m) {
        vec3 across = vcross(m->dir, v3(0, 1, 0));
        if (vdot(across, b->fwd) < 0)
            across = vscale(across, -1);
        want = vnorm(vadd(across, v3(0, -0.1f, 0)));
        speed = t->speed * 1.25f;
    }

    // Not into the ground
    if (b->state != BANDIT_CLIMB && b->pos.y < ground + BANDIT_FLOOR && want.y < 0.2f)
        want = vnorm(v3(want.x, 0.35f, want.z));

    float max_turn = t->max_g * GRAVITY / (b->speed > 60 ? b->speed : 60) * dt;
    b->turn = turn_toward(&b->fwd, want, max_turn) / dt;
    b->speed = approach(b->speed, speed, 15 * dt);
    b->pos = vadd(b->pos, vscale(b->fwd, sim_dist(b->speed, dt)));
    if (b->pos.y < ground + 20)
        b->pos.y = ground + 20;
}

// Bank for the turn it is making: from how fast its heading changes (not
// pitch), eased in and out at a steady roll rate so it doesn't twitch
#define BANK_ROLL_RATE  1.2f        // rad/s
#define BANK_EASE       3.0f        // 1/s toward the turn's bank

// Its up from its heading and bank
static void bank_up(bandit_t* b) {
    vec3 right = vcross(v3(0, 1, 0), b->fwd);
    right = vdot(right, right) < 1e-6f ? v3(1, 0, 0) : vnorm(right);
    vec3 up = vcross(b->fwd, right);
    b->up = vadd(vscale(up, cosf(b->bank)), vscale(right, sin_f(b->bank)));
}

void bank_with_turn(bandit_t* b, vec3 old_fwd, float dt, float limit) {
    float h0 = atan2_f(old_fwd.x, old_fwd.z), h1 = atan2_f(b->fwd.x, b->fwd.z);
    float dh = h1 - h0;
    while (dh > 3.14159265f) dh -= 6.2831853f;
    while (dh < -3.14159265f) dh += 6.2831853f;
    float want = clampf(atan2_f(dh / dt * b->speed, GRAVITY), -limit, limit);
    float step = clampf((want - b->bank) * BANK_EASE * dt, -BANK_ROLL_RATE * dt, BANK_ROLL_RATE * dt);
    b->bank += step;
    bank_up(b);
}

void air_update(float dt) {
    if (!sams_enabled) {
        for (int i = 0; i < MAX_BANDITS; i++)
            if (bandits[i].active && bandits[i].state != BANDIT_FALLING)
                bandits[i].active = false;      // Enemies off: gone
    }
    wake_bases();
    for (int i = 0; i < MAX_BANDITS; i++) {
        bandit_t* b = &bandits[i];
        if (!b->active)
            continue;
        vec3 old_fwd = b->fwd;
        fly(b, i, dt);
        if (!b->active)
            continue;

        // Bank with the turn (for the picture); falling, it spins
        if (b->state == BANDIT_FALLING) {
            b->bank = b->state_time * 4.0f;
            bank_up(b);
        } else {
            bank_with_turn(b, old_fwd, dt, 1.3f);
        }
    }
}
