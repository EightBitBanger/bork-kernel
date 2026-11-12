/*
 * traffic.c: friendly fighters and airliners going about their business.
 *
 * Every TRAFFIC_GAP seconds, while fewer than MAX_TRAFFIC are about, one
 * more appears: off a friendly base's runway or already on its way, bound
 * for another friendly base near you (or, with only one about, out of the
 * area). It climbs to its cruising height, flies to a fix TRAFFIC_FINAL_FIX
 * out from the far base's runway, follows a three degree glide path down
 * to it, lands, rolls out and is gone. Anything that wanders farther than
 * TRAFFIC_GONE from you is gone too. They are background: nobody shoots
 * at them and your missiles won't lock them.
 */

#include <string.h>

#include "flight.h"

#define TRAFFIC_GAP       15.0f     // Seconds between new ones
#define TRAFFIC_RANGE     60000.0f  // Bases this near you send and take traffic
#define TRAFFIC_GONE      90000.0f  // Farther than this from you: gone
#define TRAFFIC_MIN_LEG   8000.0f   // Bases closer than this don't trade
#define TRAFFIC_FINAL_FIX 8000.0f   // Final approach starts this far out
#define TRAFFIC_GLIDE     0.0524f   // Three degrees
#define TRAFFIC_FLOOR     400.0f    // Above the ground, cruising
#define TRAFFIC_ROLL_ACCEL 2.5f     // m/s^2 down the runway, both ways

typedef struct {
    const char* name;
    int         look;
    float       size;               // Airliners: 1 is a 747
    uint32_t    color;
    float       speed, land_speed;  // Cruising, and over the runway
    float       max_g;
    float       alt_min, alt_max;   // Cruising height
} traffic_type_t;

static const traffic_type_t types[] = {
    { "Boeing 747",     LOOK_AIRLINER, 1.00f, GFX_RGB(236, 238, 240), 240, 80, 1.4f, 3500, 5500 },
    { "Boeing 737",     LOOK_AIRLINER, 0.50f, GFX_RGB(230, 232, 236), 220, 72, 1.6f, 3000, 4500 },
    { "Airbus A320",    LOOK_AIRLINER, 0.53f, GFX_RGB(240, 240, 244), 220, 72, 1.6f, 3000, 4500 },
    { "KC-135",         LOOK_AIRLINER, 0.66f, GFX_RGB(150, 156, 160), 210, 78, 1.5f, 3000, 5000 },
    { "C-17",           LOOK_AIRLINER, 0.80f, GFX_RGB(104, 112, 116), 200, 62, 1.6f, 2500, 4000 },
    { "F-15C",          LOOK_FIGHTER,  1.00f, GFX_RGB(150, 162, 176), 250, 75, 5.0f, 2000, 3500 },
    { "F-16C",          LOOK_FIGHTER,  1.00f, GFX_RGB(140, 148, 156), 240, 72, 5.0f, 2000, 3500 },
};

traffic_t    traffic[MAX_TRAFFIC];
static float next_time;
static uint32_t spawned;

void traffic_reset(void) {
    memset(traffic, 0, sizeof(traffic));
    next_time = world_time + 3;
}

static float gear_height(const traffic_t* t) {
    return t->p.look == LOOK_AIRLINER ? 1.5f + 5.0f * t->p.size : GEAR_H;
}

// The runway end to come in over, and the way to land: from the side it is on
static void plan_landing(traffic_t* t) {
    const base_t* b = &t->dest;
    float s = (t->p.pos.x - b->x) * b->along.x + (t->p.pos.z - b->z) * b->along.z;
    t->land_dir = s > 0 ? vscale(b->along, -1) : b->along;
}

static vec3 threshold(const traffic_t* t) {
    float back = RUNWAY_HALF_LEN - 300;
    return v3(t->dest.x - t->land_dir.x * back, 0, t->dest.z - t->land_dir.z * back);
}

static void spawn(void) {
    traffic_t* t = NULL;
    for (int i = 0; i < MAX_TRAFFIC && !t; i++)
        if (!traffic[i].p.active)
            t = &traffic[i];
    if (!t)
        return;
    base_t near[16], ok[16];
    int n = bases_near(plane.pos.x, plane.pos.z, TRAFFIC_RANGE, near, 16), m = 0;
    for (int i = 0; i < n; i++)
        if (!near[i].enemy)
            ok[m++] = near[i];
    if (!m)
        return;

    uint32_t r = hash2((int32_t)spawned, (int32_t)(world_time * 10), 77);
    spawned++;
    const traffic_type_t* tt = &types[r % COUNT(types)];
    const base_t* from = &ok[(r >> 4) % (uint32_t)m];
    memset(t, 0, sizeof(*t));
    t->has_dest = false;
    for (int k = 0; k < m; k++) {          // Another base far enough, picked by r
        const base_t* d = &ok[(k + (r >> 8)) % (uint32_t)m];
        float dx = d->x - from->x, dz = d->z - from->z;
        if (dx * dx + dz * dz > TRAFFIC_MIN_LEG * TRAFFIC_MIN_LEG) {
            t->dest = *d;
            t->has_dest = true;
            break;
        }
    }
    bandit_t* p = &t->p;
    p->active = true;
    p->name = tt->name;
    p->type = (int)(tt - types);
    p->look = tt->look;
    p->size = tt->size;
    p->color = tt->color;
    p->up = v3(0, 1, 0);
    t->cruise_alt = tt->alt_min + (float)((r >> 12) % 1000) / 1000.0f * (tt->alt_max - tt->alt_min);
    t->land_speed = tt->land_speed;

    // Half of them are already on their way: part way along, cruising
    bool airborne = t->has_dest && ((r >> 20) & 1);
    if (airborne) {
        float f = 0.2f + (float)((r >> 22) % 40) / 100.0f;
        p->pos = v3(from->x + (t->dest.x - from->x) * f, t->cruise_alt, from->z + (t->dest.z - from->z) * f);
        p->fwd = vnorm(v3(t->dest.x - from->x, 0, t->dest.z - from->z));
        p->speed = tt->speed;
        p->state = TRAFFIC_CRUISE;
    } else {
        // Not down the runway you are sitting on
        if (plane.on_ground && fabsf(plane.pos.x - from->x) < RUNWAY_HALF_LEN + 500 &&
            fabsf(plane.pos.z - from->z) < RUNWAY_HALF_LEN + 500)
            return;
        vec3 dir = (r >> 21) & 1 ? from->along : vscale(from->along, -1);
        float start = RUNWAY_HALF_LEN - 150;
        p->pos = v3(from->x - dir.x * start, 0, from->z - dir.z * start);
        p->pos.y = gear_height(t);
        p->fwd = dir;
        p->speed = 0;
        p->state = TRAFFIC_ROLL;
    }
    if (t->has_dest)
        plan_landing(t);
}

static void fly(traffic_t* t, float dt) {
    bandit_t* p = &t->p;
    const traffic_type_t* tt = &types[p->type];
    float ground = terrain_height(p->pos.x, p->pos.z);
    vec3 ahead = vadd(p->pos, vscale(p->fwd, 1500));
    float ground_ahead = terrain_height(ahead.x, ahead.z) > ground ? terrain_height(ahead.x, ahead.z) : ground;
    p->state_time += dt;

    if (p->state == TRAFFIC_ROLL || p->state == TRAFFIC_ROLLOUT) {
        // On the wheels, straight down the runway
        if (p->state == TRAFFIC_ROLL) {
            p->speed += TRAFFIC_ROLL_ACCEL * 1.4f * dt;
            if (p->speed >= t->land_speed)
                p->state = TRAFFIC_CLIMB;
        } else {
            p->speed -= TRAFFIC_ROLL_ACCEL * dt;
            if (p->speed < 12) {
                p->active = false;      // Turned off onto the taxiway
                return;
            }
        }
        p->turn = 0;
        p->pos = vadd(p->pos, vscale(p->fwd, sim_dist(p->speed, dt)));
        p->pos.y = gear_height(t);
        return;
    }

    vec3 want = p->fwd;
    float speed = tt->speed;
    switch (p->state) {
        case TRAFFIC_CLIMB:
            want = vnorm(v3(p->fwd.x, 0.12f, p->fwd.z));
            speed = tt->speed * 0.7f;
            if (p->pos.y > ground + 600)
                p->state = TRAFFIC_CRUISE;
            break;
        case TRAFFIC_CRUISE: {
            float alt = t->cruise_alt;
            if (t->has_dest) {
                vec3 th = threshold(t);
                vec3 fix = vsub(th, vscale(t->land_dir, TRAFFIC_FINAL_FIX));
                float dx = fix.x - p->pos.x, dz = fix.z - p->pos.z, d = sqrtf(dx * dx + dz * dz);
                float fix_alt = TRAFFIC_FINAL_FIX * TRAFFIC_GLIDE;
                if (d < 20000)              // Down toward the fix's height on the way in
                    alt = fix_alt + (t->cruise_alt - fix_alt) * clampf((d - 3000) / 17000, 0, 1);
                if (d < 30000)
                    speed = tt->land_speed + (tt->speed - tt->land_speed) * clampf(d / 30000, 0, 1);
                want = d > 1 ? v3(dx / d, 0, dz / d) : p->fwd;
                float floor = ground_ahead + TRAFFIC_FLOOR;
                if (d < 1200 && (p->pos.y < fix_alt + 300 || p->pos.y < floor + 100))
                    p->state = TRAFFIC_FINAL;   // (Still high: it circles the fix on the way down)
                if (alt < floor)
                    alt = floor;
            } else if (alt < ground_ahead + TRAFFIC_FLOOR) {
                alt = ground_ahead + TRAFFIC_FLOOR;
            }
            want.y = clampf((alt - p->pos.y) / (p->pos.y < alt ? 800.0f : 2000.0f), -0.15f, p->pos.y < ground_ahead + TRAFFIC_FLOOR / 2 ? 0.35f : 0.12f);
            want = vnorm(want);
            break;
        }
        case TRAFFIC_FINAL: {
            // Onto the centerline a little ahead, and down the glide path
            vec3 th = threshold(t);
            float along = (p->pos.x - th.x) * t->land_dir.x + (p->pos.z - th.z) * t->land_dir.z;
            float ahead = along + 1500 < 0 ? along + 1500 : 0;
            vec3 aim = vadd(th, vscale(t->land_dir, ahead));
            aim.y = gear_height(t) - ahead * TRAFFIC_GLIDE;
            want = vnorm(vsub(aim, p->pos));
            if (want.y < -0.12f)
                want = vnorm(v3(want.x, -0.12f, want.z));
            speed = tt->land_speed;
            if (along > -1 && p->pos.y < gear_height(t) + 3) {
                p->state = TRAFFIC_ROLLOUT;
                p->fwd = t->land_dir;
                return;
            }
            if (along > RUNWAY_HALF_LEN) {  // Missed it somehow: around again
                p->state = TRAFFIC_CRUISE;
                plan_landing(t);
            }
            break;
        }
    }

    float max_turn = tt->max_g * GRAVITY / (p->speed > 60 ? p->speed : 60) * dt;
    p->turn = turn_toward(&p->fwd, want, max_turn) / dt;
    p->speed = approach(p->speed, speed, 6 * dt);
    p->pos = vadd(p->pos, vscale(p->fwd, sim_dist(p->speed, dt)));
    if (p->pos.y < ground + 60 && (p->state != TRAFFIC_FINAL || ground > 0))
        p->pos.y = ground + 60;     // Never into a hill (runways are at 0)
}

void traffic_update(float dt) {
    if (world_time >= next_time) {
        next_time = world_time + TRAFFIC_GAP;
        spawn();
    }
    for (int i = 0; i < MAX_TRAFFIC; i++) {
        traffic_t* t = &traffic[i];
        bandit_t* p = &t->p;
        if (!p->active)
            continue;
        vec3 old_fwd = p->fwd;
        fly(t, dt);
        float dx = p->pos.x - plane.pos.x, dz = p->pos.z - plane.pos.z;
        if (dx * dx + dz * dz > TRAFFIC_GONE * TRAFFIC_GONE)
            p->active = false;
        if (!p->active)
            continue;

        // Bank with the turn (gently, for an airliner; level on the wheels)
        bool rolling = p->state == TRAFFIC_ROLL || p->state == TRAFFIC_ROLLOUT;
        bank_with_turn(p, old_fwd, dt, rolling ? 0 : p->look == LOOK_AIRLINER ? 0.45f : 1.2f);
    }
}
