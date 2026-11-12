/*
 * sam.c: SAM sites at work.
 *
 * An enemy site's radar sees the aircraft within RADAR_RANGE when it is
 * more than RADAR_FLOOR above the ground and no hill is in the way. After
 * LOCK_TIME of tracking it launches, one missile at a time, LAUNCH_GAP
 * apart, while it has missiles on its launchers (each reloads after
 * RELOAD_TIME). Friendly sites never launch.
 *
 * The missiles are radar guided from the site: while the site still sees
 * the aircraft they steer for where it will be (lead pursuit), but they can
 * only turn so hard (MISSILE_MAX_G), and the faster they go the wider they
 * turn. The motor burns for MISSILE_BOOST, then they coast and slow down.
 * So: break hard across a missile's path just before it arrives and it
 * overshoots; run from it and it runs out of energy; dive below the radar
 * or behind a hill and it flies on blind. Inside MISSILE_FUSE it explodes:
 * the aircraft is hit (aircraft.c says what that costs).
 *
 * Every other missile flies here too, each kind with its own motor and
 * guidance: the fighters' (heat seekers: they follow while the aircraft is
 * ahead of them), and yours (air.c, weapons.c), which steer for what was
 * locked and destroy it.
 *
 * Site state (missiles left, lock, reload) is kept for the sites nearby,
 * keyed by their cell (or map index); the sites themselves come from
 * world.c. A map's radar on its own (no launchers) only watches.
 */

#include <string.h>

#include "flight.h"

missile_t missiles[MAX_MISSILES];
decoy_t   decoys[MAX_DECOYS];
blast_t   blasts[MAX_BLASTS];
int       sam_threat;
bool      sams_enabled = true;

#define MAX_SITE_STATES 32
#define SITE_STEP       0.1f        // Sites think this often

typedef struct {
    bool     used;
    int32_t  ci, cj;
    uint32_t stamp;                 // Last looked at (to reuse the oldest)
    uint8_t  loaded;                // A bit per launcher with a missile on
    uint8_t  full;
    float    lock;                  // Seconds of tracking
    float    cooldown;              // Until it may launch again
    float    reload;                // Until the next launcher is loaded
    float    los_timer;
    bool     los;                   // No hill between radar and aircraft
    bool     tracking;              // Sees the aircraft now
} site_state_t;

static site_state_t states[MAX_SITE_STATES];
static uint32_t     stamp;
static float        site_timer;
static int          site_threat;

static bool enemy_kind(int kind) {
    return kind == MSL_SAM || kind == MSL_ENEMY_AAM;
}

void sam_reset(void) {
    memset(states, 0, sizeof(states));
    memset(missiles, 0, sizeof(missiles));
    memset(blasts, 0, sizeof(blasts));
    memset(decoys, 0, sizeof(decoys));
    sam_threat = site_threat = THREAT_NONE;
    site_timer = 0;
}

static site_state_t* find_state(int32_t ci, int32_t cj) {
    for (int i = 0; i < MAX_SITE_STATES; i++)
        if (states[i].used && states[i].ci == ci && states[i].cj == cj)
            return &states[i];
    return NULL;
}

static site_state_t* get_state(const sam_site_t* s) {
    site_state_t* st = find_state(s->ci, s->cj);
    if (!st) {
        st = &states[0];
        for (int i = 0; i < MAX_SITE_STATES; i++) {
            if (!states[i].used) {
                st = &states[i];
                break;
            }
            if (states[i].stamp < st->stamp)
                st = &states[i];
        }
        memset(st, 0, sizeof(*st));
        st->used   = true;
        st->ci     = s->ci;
        st->cj     = s->cj;
        st->full   = (uint8_t)((1u << s->launchers) - 1);
        st->loaded = st->full;
    }
    st->stamp = ++stamp;
    return st;
}

uint8_t sam_loaded(const sam_site_t* s) {
    site_state_t* st = find_state(s->ci, s->cj);
    return st ? st->loaded : (uint8_t)((1u << s->launchers) - 1);
}

void blast_at(vec3 p) {
    blast_t* b = &blasts[0];
    for (int i = 0; i < MAX_BLASTS; i++) {
        if (!blasts[i].active) {
            b = &blasts[i];
            break;
        }
        if (blasts[i].age > b->age)
            b = &blasts[i];
    }
    b->active = true;
    b->pos = p;
    b->age = 0;
}

static void explode(missile_t* m) {
    m->active = false;
    m->fade = 0;
    blast_at(m->pos);
}

void sam_set_enabled(bool on) {
    sams_enabled = on;
    if (!on) {
        for (int i = 0; i < MAX_MISSILES; i++)
            if (missiles[i].active && enemy_kind(missiles[i].kind))
                explode(&missiles[i]);
        for (int i = 0; i < MAX_SITE_STATES; i++)
            states[i].lock = 0;
    }
}

// No hill between a and b
static bool line_of_sight(vec3 a, vec3 b) {
    for (int i = 1; i < 16; i++) {
        vec3 p = vadd(a, vscale(vsub(b, a), (float)i / 16.0f));
        if (terrain_height(p.x, p.z) > p.y)
            return false;
    }
    return true;
}

static int missiles_from(int32_t ci, int32_t cj) {
    int n = 0;
    for (int i = 0; i < MAX_MISSILES; i++)
        if (missiles[i].active && missiles[i].site_ci == ci && missiles[i].site_cj == cj)
            n++;
    return n;
}

// Each kind of missile: motor, how hard it turns, how slowly its guidance
// follows, how long it flies, its proximity fuse, and the cos of its
// seeker's half angle (heat seekers lose what isn't ahead; -1: no seeker).
// Bombs have no motor: they glide on what they were dropped with
static const struct {
    float boost, thrust, max_g, lag, life, fuse, seeker;
    bool  bomb;
} kinds[MSL_KINDS] = {
    [MSL_SAM]       = { MISSILE_BOOST, MISSILE_THRUST, MISSILE_MAX_G, MISSILE_LAG, MISSILE_LIFE, MISSILE_FUSE, -1, false },
    [MSL_ENEMY_AAM] = { 2.5f, 250, 11, 0.35f, 20, 12, 0.64f, false },
    [MSL_AAM]       = { 2.5f, 300, 26, 0.12f, 18, 15, 0.5f, false },     // Sidewinder
    [MSL_AMRAAM]    = { 4.0f, 280, 20, 0.2f, 35, 15, -1, false },
    [MSL_AGM]       = { 5.0f, 70, 9, 0.2f, 40, 15, -1, false },          // Maverick
    [MSL_HARM]      = { 4.0f, 260, 12, 0.2f, 45, 20, -1, false },
    [MSL_PAVEWAY]   = { 0, 0, 4, 0.3f, 120, 12, -1, true },
    [MSL_JDAM]      = { 0, 0, 3, 0.3f, 120, 15, -1, true },
    [MSL_NUKE]      = { 8.0f, 70, 9, 0.2f, 120, 40, -1, false },     // Maverick-like, longer burn
};

#define BOMB_STEER_V    60.0f       // A bomb slower than this can't steer: it just falls

#define AGM_KILL        60.0f       // An air-to-ground missile hitting the ground this close still counts

int enemy_missiles(void) {
    int n = 0;
    for (int i = 0; i < MAX_MISSILES; i++)
        n += missiles[i].active && enemy_kind(missiles[i].kind);
    return n;
}

//
// Chaff and flares. Chaff is a cloud of foil that radar missiles (SAMs)
// may take for the aircraft; a flare is a burning decoy for heat seekers.
// Each one dropped may fool each missile of the kind it works on that is
// coming for whoever dropped it and is within DECOY_REACH (so too early
// is wasted): chaff a radar missile, a flare a heat seeker (less often
// while the afterburner glows).
//

#define DECOY_REACH     3500.0f
#define CHAFF_CHANCE    40          // Percent, each
#define FLARE_CHANCE    45
#define FLARE_CHANCE_AB 20
#define CHAFF_LIFE      6.0f
#define FLARE_LIFE      4.0f

bool heat_seeker(int kind) {
    return kind == MSL_ENEMY_AAM || kind == MSL_AAM;
}

bool decoy_drop(int kind, vec3 pos, vec3 vel, bool by_player) {
    int d = -1;
    for (int i = 0; i < MAX_DECOYS && d < 0; i++)
        if (!decoys[i].active)
            d = i;
    if (d < 0)
        return false;
    decoys[d] = (decoy_t){ true, kind, pos, vel, 0 };
    int chance = kind == DECOY_CHAFF ? CHAFF_CHANCE : by_player && plane.afterburner ? FLARE_CHANCE_AB : FLARE_CHANCE;
    for (int i = 0; i < MAX_MISSILES; i++) {
        missile_t* m = &missiles[i];
        // Only what is chasing whoever dropped it, and only the kind this fools
        bool at_me = by_player ? enemy_kind(m->kind) : m->kind == MSL_AAM;   // (Fighters' flares: Sidewinders)
        if (!m->active || m->decoy || !at_me || heat_seeker(m->kind) != (kind == DECOY_FLARE))
            continue;
        if (vlen(vsub(m->pos, pos)) > DECOY_REACH)
            continue;
        uint32_t r = hash2(i * 131 + d, (int32_t)(world_time * 977.0f), 41);
        if ((int)(r % 100) < chance)
            m->decoy = d + 1;
    }
    return true;
}

static void update_decoys(float dt) {
    for (int i = 0; i < MAX_DECOYS; i++) {
        decoy_t* d = &decoys[i];
        if (!d->active)
            continue;
        d->age += dt;
        if (d->kind == DECOY_FLARE) {
            d->vel = vscale(d->vel, 1.0f - 1.2f * dt);       // Drags back, and falls
            d->vel.y -= GRAVITY * dt;
        } else {
            d->vel = vscale(d->vel, 1.0f - 3.0f * dt);       // Chaff hangs in the air
            d->vel.y = -3;
        }
        d->pos = vadd(d->pos, vscale(d->vel, dt));
        if (d->age > (d->kind == DECOY_FLARE ? FLARE_LIFE : CHAFF_LIFE) || d->pos.y < terrain_height(d->pos.x, d->pos.z))
            d->active = false;
    }
}

missile_t* missile_new(int kind, vec3 pos, vec3 dir, float speed, target_t target) {
    missile_t* m = NULL;
    for (int i = 0; i < MAX_MISSILES && !m; i++)
        if (!missiles[i].active && (missiles[i].trail_count == 0 || missiles[i].fade >= TRAIL_FADE))
            m = &missiles[i];
    if (!m)
        return NULL;
    memset(m, 0, sizeof(*m));
    m->active = true;
    m->kind   = kind;
    m->target = target;
    m->pos    = pos;
    m->dir    = vnorm(dir);
    m->speed  = speed;
    m->guided = true;
    return m;
}

static void launch(const sam_site_t* s, site_state_t* st) {
    if (enemy_missiles() >= MAX_INBOUND)
        return;
    int n = 0;
    while (n < s->launchers && !(st->loaded & (1u << n)))
        n++;
    if (n >= s->launchers)
        return;

    vec3 pos, out, side, rail;
    sam_launcher(s, n, &pos, &out, &side, &rail);
    missile_t* m = missile_new(MSL_SAM, vadd(vadd(pos, v3(0, 3, 0)), vscale(rail, 4)), rail, 40,
                               (target_t){ .kind = TGT_PLAYER });
    if (!m)
        return;
    m->site_ci = s->ci;
    m->site_cj = s->cj;

    st->loaded &= (uint8_t)~(1u << n);
    st->cooldown = LAUNCH_GAP;
    if (st->reload <= 0)
        st->reload = RELOAD_TIME;
}

static void update_sites(float dt) {
    site_threat = THREAT_NONE;
    bool visible = !plane.crashed && !plane.on_ground;
    float agl = plane.pos.y - terrain_height(plane.pos.x, plane.pos.z);
    sam_site_t near[MAX_SITE_STATES];
    int count = sams_near(plane.pos.x, plane.pos.z, RADAR_RANGE + 2000, near, MAX_SITE_STATES);

    for (int k = 0; k < count; k++) {
        const sam_site_t s = near[k];
        if (sam_destroyed(s.ci, s.cj))
            continue;
        vec3 radar = v3(s.x, 12, s.z);
        float dist = vlen(vsub(plane.pos, radar));
        if (dist > RADAR_RANGE + 2000)
            continue;
        site_state_t* st = get_state(&s);

        // Reloading, one launcher at a time
        if (st->loaded != st->full) {
            st->reload -= dt;
            if (st->reload <= 0) {
                int n = 0;
                while (st->loaded & (1u << n))
                    n++;
                st->loaded |= (uint8_t)(1u << n);
                st->reload = st->loaded != st->full ? RELOAD_TIME : 0;
            }
        }
        if (st->cooldown > 0)
            st->cooldown -= dt;

        bool sees = s.enemy && sams_enabled && visible && dist < seen_range(RADAR_RANGE) && agl > RADAR_FLOOR;
        if (sees) {
            st->los_timer -= dt;
            if (st->los_timer <= 0) {
                st->los = line_of_sight(radar, plane.pos);
                st->los_timer = 0.5f;
            }
            sees = st->los;
        }
        st->tracking = sees;
        if (!sees) {
            st->lock = st->lock > 2 * dt ? st->lock - 2 * dt : 0;
            continue;
        }

        st->lock += dt;
        // A radar on its own only watches
        int threat = st->lock < LOCK_TIME * 0.5f || !s.launchers ? THREAT_RADAR : THREAT_LOCK;
        if (threat > site_threat)
            site_threat = threat;
        if (st->lock >= LOCK_TIME && st->cooldown <= 0 && st->loaded && dist < LAUNCH_RANGE &&
            missiles_from(s.ci, s.cj) < 2)
            launch(&s, st);
    }
}

static void trail_push(missile_t* m) {
    if (m->trail_count < TRAIL_LEN) {
        m->trail[(m->trail_head + m->trail_count++) % TRAIL_LEN] = m->pos;
    } else {
        m->trail[m->trail_head] = m->pos;
        m->trail_head = (m->trail_head + 1) % TRAIL_LEN;
    }
}

// Steer for where the target (at target_p, moving at target_v) will be
static void steer(missile_t* m, vec3 target_p, vec3 target_v, float dt) {
    vec3 to = vsub(target_p, m->pos);
    float dist = vlen(to);
    if (dist < 1)
        return;
    vec3 to_n = vscale(to, 1.0f / dist);
    float closing = sim_v(m->speed) * vdot(m->dir, to_n) - vdot(target_v, to_n);
    float t_go = clampf(dist / (closing > 100 ? closing : 100), 0, 6);
    vec3 want = vnorm(vsub(vadd(target_p, vscale(target_v, t_go)), m->pos));

    // The guidance follows where it should aim only after a while
    if (vdot(m->aim, m->aim) < 0.5f)
        m->aim = want;
    m->aim = vnorm(vadd(m->aim, vscale(vsub(want, m->aim), clampf(dt / kinds[m->kind].lag, 0, 1))));
    vec3 aim = m->aim;

    float c = clampf(vdot(m->dir, aim), -1, 1);
    float angle = acosf(c);
    float max_turn = kinds[m->kind].max_g * GRAVITY / (m->speed > 50 ? m->speed : 50) * dt;
    if (angle <= max_turn) {
        m->dir = aim;
    } else {
        vec3 perp = vnorm(vsub(aim, vscale(m->dir, c)));
        m->dir = vnorm(vadd(vscale(m->dir, cosf(max_turn)), vscale(perp, sin_f(max_turn))));
    }
}

// Closest a missile moving from m0 to m1 in the last dt came to a target
// now at t1 and moving at tv
static float pass_dist(vec3 m0, vec3 m1, vec3 t1, vec3 tv, float dt) {
    vec3 a = vsub(m0, vsub(t1, vscale(tv, dt)));    // Apart at the start of the step ...
    vec3 b = vsub(m1, t1);                          // ... and at its end
    vec3 d = vsub(b, a);
    float dd = vdot(d, d);
    float s = dd > 1e-6f ? clampf(-vdot(a, d) / dd, 0, 1) : 1;
    return vlen(vadd(a, vscale(d, s)));
}

static void update_missiles(float dt) {
    for (int i = 0; i < MAX_MISSILES; i++) {
        missile_t* m = &missiles[i];
        if (!m->active) {
            if (m->trail_count && m->fade < TRAIL_FADE)
                m->fade += dt;
            continue;
        }
        m->age += dt;

        // What it steers for, and whether it still can
        vec3 tp = plane.pos, tv = plane.on_ground ? v3(0, 0, 0) : vscale(plane.fwd, sim_v(plane.speed));
        bool alive = !plane.crashed;
        if (m->decoy) {
            // Fooled: it chases the decoy while it lasts, then flies on blind
            const decoy_t* d = &decoys[m->decoy - 1];
            m->guided = d->active;
            tp = d->pos;
            tv = d->vel;
            alive = false;
        } else if (m->kind == MSL_SAM) {
            if (m->site_cj == SHIP_SITE) {
                m->guided = ship_tracking(m->site_ci) && alive;
            } else {
                site_state_t* st = find_state(m->site_ci, m->site_cj);
                m->guided = st && st->tracking && alive;
            }
        } else {
            if (m->target.kind != TGT_PLAYER)
                alive = target_state(&m->target, &tp, &tv);
            m->guided = alive;
            if (alive && kinds[m->kind].seeker > -1) {
                vec3 to = vsub(tp, m->pos);
                float d = vlen(to);
                m->guided = d < 1 || vdot(to, m->dir) > kinds[m->kind].seeker * d;
            }
        }
        if (kinds[m->kind].bomb && m->speed < BOMB_STEER_V)
            m->guided = false;
        if (m->guided && m->age > (kinds[m->kind].bomb ? 1.0f : 0.5f))     // Clear of the rail (or the jet) first
            steer(m, tp, tv, dt);
        else if (m->age > 0.5f)
            m->dir = vnorm(vadd(m->dir, v3(0, -GRAVITY / m->speed * dt, 0)));   // Blind: it sags

        float thrust = m->age < kinds[m->kind].boost ? kinds[m->kind].thrust : 0;
        m->speed += (thrust - MISSILE_DRAG * m->speed * m->speed - GRAVITY * m->dir.y) * dt;
        if (m->speed < 20)
            m->speed = 20;
        vec3 m_from = m->pos;
        m->pos = vadd(m->pos, vscale(m->dir, sim_dist(m->speed, dt)));

        m->trail_timer += dt;
        if (m->trail_timer >= TRAIL_STEP) {
            m->trail_timer -= TRAIL_STEP;
            if (!kinds[m->kind].bomb)           // No motor, no smoke
                trail_push(m);
        }

        float fuse = kinds[m->kind].fuse;
        bool mine = !enemy_kind(m->kind);
        // Closest it came during the step: fast as things go, it can pass
        // clean through the fuse's reach between two steps
        vec3 pv = plane.on_ground ? v3(0, 0, 0) : vscale(plane.fwd, sim_v(plane.speed));
        float near_tp = pass_dist(m_from, m->pos, tp, tv, dt);
        float near_player = pass_dist(m_from, m->pos, plane.pos, pv, dt);
        if (m->decoy && m->guided && near_tp < fuse) {
            explode(m);                         // On the decoy: harmless
        } else if (!mine && !plane.crashed && near_player < fuse) {
            explode(m);
            take_hit();
        } else if (mine && alive && near_tp < fuse) {
            explode(m);
            target_hit(&m->target, m->pos);
            if (m->kind == MSL_JDAM)
                weapons_area_hit(m->pos, weapon_types[WPN_JDAM].blast);
            if (m->kind == MSL_NUKE)
                nuke_detonate(m->pos);
        } else if (m->pos.y < terrain_height(m->pos.x, m->pos.z)) {
            explode(m);
            bool ground = m->kind == MSL_AGM || m->kind == MSL_HARM || m->kind == MSL_PAVEWAY || m->kind == MSL_JDAM ||
                          m->kind == MSL_NUKE;
            if (mine && alive && ground && vlen(vsub(tp, m->pos)) < AGM_KILL)
                target_hit(&m->target, m->pos);
            if (m->kind == MSL_JDAM)
                weapons_area_hit(m->pos, weapon_types[WPN_JDAM].blast);
            if (m->kind == MSL_NUKE)
                nuke_detonate(m->pos);
        } else if (m->age > kinds[m->kind].life ||
                   (m->age > kinds[m->kind].boost && m->speed < MISSILE_MIN_V && m->kind != MSL_AGM &&
                    m->kind != MSL_NUKE && !kinds[m->kind].bomb)) {
            explode(m);
            if (m->kind == MSL_NUKE)            // Out of time: it goes off anyway
                nuke_detonate(m->pos);
        }
    }

    for (int i = 0; i < MAX_BLASTS; i++) {
        if (blasts[i].active) {
            blasts[i].age += dt;
            if (blasts[i].age > 2.0f)
                blasts[i].active = false;
        }
    }
}

void sam_update(float dt) {
    site_timer += dt;
    while (site_timer >= SITE_STEP) {
        site_timer -= SITE_STEP;
        update_sites(SITE_STEP);
    }
    update_decoys(dt);
    update_missiles(dt);

    sam_threat = site_threat;
    for (int i = 0; i < ship_count; i++)
        if (ship_tracking(i) && sam_threat < THREAT_LOCK)
            sam_threat = THREAT_LOCK;
    for (int i = 0; i < MAX_MISSILES; i++)
        if (missiles[i].active && missiles[i].guided && !missiles[i].decoy && enemy_kind(missiles[i].kind))
            sam_threat = THREAT_MISSILE;
}
