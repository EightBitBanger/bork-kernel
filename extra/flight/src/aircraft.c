/*
 * aircraft.c: the aircraft and its controls.
 *
 * In the air: arcade, but with real forces. The aircraft flies where the
 * nose points; lift (up to 1 g, less when slow) and gravity bend the flight
 * path, thrust and drag set the speed, climbing costs speed and diving gains
 * it. Bank and the nose falls toward the low wing (a turn); pull to tighten
 * it. Below about 110 knots there isn't enough lift and the nose drops.
 *
 * On the ground: wheels. Steer with the rudder or the roll stick, pull to
 * raise the nose once past rotate speed, and it flies when the nose is up
 * and the speed is there. Coming down is a landing only on a runway, gear
 * down, slow enough, gently and wings level; anything else is a crash.
 *
 * How fast, how hard it turns and how long its fuel lasts depend on the
 * type, from the table below (picked in the menu). There is no hard top
 * speed: thrust and drag settle on one, and where the air is thin (way up
 * high) a dive keeps gaining speed until thicker air slows it.
 */

#include <string.h>

#include "flight.h"

plane_t    plane;
controls_t ctl;

const aircraft_t aircraft_types[] = {
    //  name, about
    //  cruise thr_mil thr_ab thin_air  v_lift stall rotate land_max  pitch roll  yaw   max_g induced fuel ab_burn
    //  load  chaff flares  stealth
    { "F-15E Strike Eagle", "The all-rounder: fast, strong, 9 g",
        300, 14, 26, 0,       75, 58, 72, 154,    0.65f, 1.8f, 0.18f, 9.0f, 0.30f, 30, 4.0f,
        1,  30, 30,  1.0f },
    { "F-16C Fighting Falcon", "Light and twitchy: rolls like nothing else",
        310, 15, 29, 0,       72, 56, 70, 154,    0.75f, 3.0f, 0.20f, 9.0f, 0.28f, 24, 4.5f,
        1,  30, 30,  1.0f },
    { "F/A-18C Hornet", "Slower, but tight turns and slow landings",
        290, 13, 24, 0,       66, 50, 68, 154,    0.70f, 2.2f, 0.22f, 7.5f, 0.32f, 26, 4.0f,
        1,  30, 30,  1.0f },
    { "F-14D Tomcat", "Big and heavy, long legs, lazy roll",
        300, 13, 25, 0,       68, 52, 72, 154,    0.55f, 1.4f, 0.16f, 7.5f, 0.30f, 34, 4.0f,
        1,  30, 30,  1.0f },
    { "F-22A Raptor", "Stealthy, supercruises, turns on a dime",
        330, 17, 30, 0,       66, 50, 68, 154,    0.85f, 2.6f, 0.25f, 9.0f, 0.24f, 32, 3.5f,
        1,  30, 30,  0.3f },
    { "F-35A Lightning II", "Stealthy striker, a little heavy",
        300, 14, 25, 0,       70, 54, 72, 154,    0.65f, 2.0f, 0.18f, 7.5f, 0.32f, 28, 4.0f,
        1,  30, 30,  0.35f },
    { "Eurofighter Typhoon", "Canard delta: climbs and turns like a rocket",
        320, 16, 30, 0,       68, 52, 70, 154,    0.80f, 2.6f, 0.22f, 9.0f, 0.26f, 28, 4.0f,
        1,  30, 30,  1.0f },
    { "F-4E Phantom II", "Old, heavy and fast, burns fuel like crazy",
        300, 13, 26, 0,       78, 60, 78, 154,    0.50f, 1.5f, 0.15f, 7.0f, 0.38f, 22, 4.5f,
        1,  20, 20,  1.0f },
    { "AV-8B Harrier II", "Subsonic jump jet: lands at a crawl",
        250, 11, 0, 0,        40, 30, 40, 154,    0.65f, 2.0f, 0.22f, 7.0f, 0.40f, 24, 1.0f,
        1,  30, 30,  1.0f },
    { "A-10C Thunderbolt II", "Slow and steady, no afterburner, flies all day",
        185, 9, 0, 0,         52, 40, 55, 154,    0.55f, 1.6f, 0.25f, 7.0f, 0.45f, 60, 1.0f,
        2,  60, 60,  1.0f },
    { "F-117A Nighthawk", "Stealthy but subsonic and sluggish, lands hot",
        260, 10, 0, 0,        80, 64, 80, 154,    0.40f, 1.0f, 0.12f, 6.0f, 0.45f, 36, 1.0f,
        1,  10, 10,  0.25f },
    { "B-1B Lancer", "Swing-wing bomber: heavy, fast down low, a flying arsenal",
        270, 9, 15, 0,        85, 68, 85, 154,    0.35f, 0.9f, 0.10f, 3.5f, 0.50f, 80, 3.0f,
        3,  90, 90,  0.5f },
    { "B-2A Spirit", "Flying wing: all but invisible to radar, fast for a bomber",
        298, 9, 0, 0,         85, 68, 85, 154,    0.35f, 0.9f, 0.10f, 3.0f, 0.50f, 90, 1.0f,
        3,  60, 60,  0.03f },
    { "SR-71A Blackbird", "Mach 3 above 50,000 ft, turns like a train",
        280, 7, 15, 7000,     88, 72, 100, 154,    0.28f, 0.9f, 0.08f, 3.0f, 0.55f, 60, 1.8f,
        1,  10, 10,  0.6f },
    { "SR-91 Aurora (top secret)", "Mach 4 up high, unseen until it's on top of you",
        320, 12, 30, 8000,    85, 70, 98, 154,    0.45f, 1.5f, 0.12f, 6.0f, 0.40f, 50, 1.5f,
        2,  20, 20,  0.08f },
};
const int        aircraft_count = (int)COUNT(aircraft_types);
const aircraft_t* craft = &aircraft_types[0];

void set_aircraft(int index) {
    craft = &aircraft_types[index >= 0 && index < aircraft_count ? index : 0];
}

void reset_controls(void) {
    ctl.cmd_x = ctl.cmd_y = ctl.cmd_z = 0;
    ctl.stick_x = ctl.stick_y = ctl.rudder = 0;
    ctl.mouse_x = ctl.mouse_y = 0;
    ctl.hold_x = ctl.hold_y = ctl.hold_z = 0;
}

static bool hook_live, catapult;    // On a carrier: the hook can still catch a wire; the catapult is firing

void reset_flight(bool in_air) {
    memset(&plane, 0, sizeof(plane));
    hook_live = catapult = false;
    plane.g_load = 1;
    ships_reset();
    int carrier = map.loaded ? ship_flagship(map.start_ship) : -1;
    base_t b;
    if (carrier >= 0) {
        // On a carrier: at the back of its straight deck, facing the bow,
        // or in the air behind it
        const ship_t* s = &ships[carrier];
        if (in_air) {
            float a = s->heading;
            plane.pos   = v3(s->pos.x - s->fwd.x * 6000, START_ALT, s->pos.z - s->fwd.z * 6000);
            plane.right = v3(cosf(a), 0, -sin_f(a));
            plane.up    = v3(0, 1, 0);
            plane.fwd   = v3(sin_f(a), 0, cosf(a));
            plane.speed = craft->cruise;
            plane.throttle = 100;
        } else {
            ship_deck_point(carrier, -120, 3, &plane.pos);
            plane.pos.y       += GEAR_H;
            plane.on_ground    = true;
            plane.gear_down    = true;
            plane.deck         = carrier + 1;
            plane.ground_fwd   = s->fwd;
            plane.ground_right = s->right;
            plane.fwd          = s->fwd;
            plane.right        = s->right;
            plane.up           = v3(0, 1, 0);
        }
        in_air = false;
        b.x = b.z = 0;
    } else if (map.loaded) {
        // A map's start: on a runway, or in the air if not at a base
        in_air = in_air || map.start_air || !base_at(map.start_x, map.start_z, &b);
    } else {
        base_in_cell(0, 0, &b);
    }
    if (carrier >= 0) {
        // Placed above
    } else if (in_air) {
        float sx = 0, sz = -RUNWAY_HALF_LEN, a = 0;
        if (map.loaded) {
            a  = map.start_heading * ((float)M_PI / 180.0f);
            sx = map.start_x - sin_f(a) * RUNWAY_HALF_LEN;
            sz = map.start_z - cosf(a) * RUNWAY_HALF_LEN;
        }
        float ground = terrain_height(sx, sz) + 600;
        plane.pos      = v3(sx, START_ALT > ground ? START_ALT : ground, sz);
        plane.right    = v3(cosf(a), 0, -sin_f(a));
        plane.up       = v3(0, 1, 0);
        plane.fwd      = v3(sin_f(a), 0, cosf(a));
        plane.speed    = craft->cruise;
        plane.throttle = 100;
    } else {
        // At the near end of the first base's runway, facing down it
        float start = -(RUNWAY_HALF_LEN - 80);
        plane.pos          = v3(b.x + b.along.x * start, GEAR_H, b.z + b.along.z * start);
        plane.on_ground    = true;
        plane.gear_down    = true;
        plane.ground_fwd   = b.along;
        plane.ground_right = b.across;
        plane.fwd          = b.along;
        plane.right        = b.across;
        plane.up           = v3(0, 1, 0);
    }
    plane.fuel = 100;
    reset_controls();
    sam_reset();
    air_reset();
    traffic_reset();
    weapons_reset();
    eject_reset();
}

// Turn a toward b (and b away from a) by angle: rotation in their plane
static void rotate_pair(vec3* a, vec3* b, float angle) {
    if (angle == 0)
        return;
    float c = cosf(angle), s = sin_f(angle);
    vec3 na = vadd(vscale(*a, c), vscale(*b, s));
    vec3 nb = vsub(vscale(*b, c), vscale(*a, s));
    *a = na;
    *b = nb;
}

void stick_press(float* cmd, uint64_t* hold, float dir, uint64_t now) {
    if (*cmd * dir < 0)
        *cmd = 0;                   // Reversing: through the middle first
    *cmd = clampf(*cmd + dir * STICK_STEP, -1, 1);
    *hold = now + (now < *hold ? STICK_REPEAT_MS : STICK_HOLD_MS);
}

void update_stick(float dt, uint64_t now) {
    float return_step = STICK_RETURN * dt;
    if (ctl.mouse) {
        ctl.cmd_x = ctl.mouse_x;
        ctl.cmd_y = ctl.invert ? -ctl.mouse_y : ctl.mouse_y;
    } else {
        if (now >= ctl.hold_x) ctl.cmd_x = approach(ctl.cmd_x, 0, return_step);
        if (now >= ctl.hold_y) ctl.cmd_y = approach(ctl.cmd_y, 0, return_step);
    }
    if (now >= ctl.hold_z) ctl.cmd_z = approach(ctl.cmd_z, 0, return_step);
    ctl.stick_x = approach(ctl.stick_x, ctl.cmd_x, STICK_SLEW * dt);
    ctl.stick_y = approach(ctl.stick_y, ctl.cmd_y, STICK_SLEW * dt);
    ctl.rudder  = approach(ctl.rudder, ctl.cmd_z, STICK_SLEW * dt);
}

void crash(const char* why) {
    if (plane.crashed)
        return;
    plane.crashed = true;
    plane.crash_reason = why;
    plane.speed = 0;
}

// Stealth: what sees others `range` away sees this aircraft only this close
float seen_range(float range) {
    if (craft->stealth >= 1)
        return range;
    float r = range * craft->stealth;
    return r < STEALTH_MIN ? STEALTH_MIN : r;
}

int throttle_limit(void) {
    return (int)((1.0f - plane.damage) * 100.0f + 0.5f);
}

bool afterburner_ok(void) {
    return craft->thrust_ab > 0 && plane.damage <= AB_DAMAGE_MAX;
}

void take_hit(void) {
    if (plane.crashed)
        return;
    if (plane.damage >= 1) {
        crash("Shot down by a SAM");        // Nothing left to lose
        return;
    }
    uint32_t r = hash2((int32_t)(world_time * 1000.0f), (int32_t)(plane.pos.x + plane.pos.z), 9);
    if ((int)(r % 100) < HIT_KILL_PCT)
        plane.damage = 1;
    else
        plane.damage = clampf(plane.damage + HIT_DAMAGE_MIN +
                              (float)(r >> 8 & 255) / 255.0f * (HIT_DAMAGE_MAX - HIT_DAMAGE_MIN), 0, 1);
    int limit = throttle_limit();
    if (plane.throttle > limit)
        plane.throttle = limit / 10 * 10;
    if (!afterburner_ok())
        plane.afterburner = false;
    plane.hit_time = world_time > 0 ? world_time : 0.001f;
}

static float thrust_now(void) {
    if (plane.fuel <= 0)
        return 0;
    if (plane.afterburner && afterburner_ok())
        return craft->thrust_ab * SPEED_BOOST * SPEED_BOOST * (1.0f - plane.damage);
    int t = plane.throttle < throttle_limit() ? plane.throttle : throttle_limit();
    return craft->thrust_mil * SPEED_BOOST * SPEED_BOOST * (float)t / 100.0f;
}

static float drag_now(float v) {
    float k = craft->thrust_mil / (craft->cruise * craft->cruise);
    if (craft->thin_air > 0 && plane.pos.y > 0)
        k *= powf(0.5f, plane.pos.y / craft->thin_air);    // Thinner air up high
    return k * (plane.brake && !plane.on_ground ? BRAKE_DRAG : 1) * (plane.gear_down ? GEAR_DRAG : 1) * v * v;
}

// Why coming down here and now is a crash, or NULL for a good landing
static const char* touchdown_problem(float ground) {
    float sink = -plane.fwd.y * sim_v(plane.speed);
    if (ground > 0)                             return "Flew into a hill";
    if (!plane.gear_down)                       return "Landed with the gear up";
    if (map_water_at(plane.pos.x, plane.pos.z)) return "Went into the sea";
    if (!on_runway(plane.pos.x, plane.pos.z))   return "Missed the runway";
    if (plane.speed > craft->v_land_max)               return "Too fast to land";
    if (sink > SINK_MAX)                        return "Hit the ground too hard";
    if (fabsf(plane.right.y) > BANK_MAX)        return "Wings weren't level";
    return NULL;
}

// Down on a carrier's deck: caught by a wire, or a bolter, or a crash
// On a carrier's landing deck after touching down: the hook catches the
// first wire it rolls over, lined up; past them all, it's a bolter
static void catch_wire(void) {
    int wire;
    float along;
    if (!hook_live || plane.arrested)
        return;
    if (ship_wire(plane.deck - 1, plane.pos.x, plane.pos.z, plane.ground_fwd, &wire, &along)) {
        plane.arrested = true;
        plane.arrest_decel = plane.speed * plane.speed / (2 * ARREST_RUN);
        if (plane.arrest_decel < 15)
            plane.arrest_decel = 15;
        hook_live = false;
        weapons_say("TRAPPED - %d wire", wire);
    } else if (along > WIRE_LAST + 8) {
        hook_live = false;
        weapons_say("BOLTER - full power, go around");
    }
}

static void deck_touchdown(int deck, float deck_y) {
    float sink = -plane.fwd.y * sim_v(plane.speed);
    const char* why = NULL;
    if (!plane.gear_down)                       why = "Landed on the deck with the gear up";
    else if (plane.speed > craft->v_land_max)   why = "Too fast for the deck";
    else if (sink > SINK_MAX)                   why = "Hit the deck too hard";
    else if (fabsf(plane.right.y) > BANK_MAX)   why = "Wings weren't level";
    if (why) {
        crash(why);
        return;
    }
    plane.on_ground    = true;
    plane.landed       = true;
    plane.deck         = deck + 1;
    plane.pos.y        = deck_y + GEAR_H;
    plane.ground_fwd   = vnorm(v3(plane.fwd.x, 0, plane.fwd.z));
    plane.ground_right = v3(plane.ground_fwd.z, 0, -plane.ground_fwd.x);
    plane.nose         = clampf(asin_f(plane.fwd.y), 0, NOSE_MAX);
    plane.speed        = sqrtf(plane.fwd.x * plane.fwd.x + plane.fwd.z * plane.fwd.z) * plane.speed;
    plane.speed       -= vdot(ships[deck].fwd, plane.ground_fwd) * ships[deck].speed;     // From here on, over the deck
    hook_live = true;
    catch_wire();
}

// Off a carrier's deck: back to speed over the water
static void leave_deck(void) {
    const ship_t* s = &ships[plane.deck - 1];
    plane.speed += vdot(s->fwd, plane.ground_fwd) * s->speed;
    plane.on_ground = false;
    plane.landed = false;
    plane.deck = 0;
    plane.arrested = false;
    hook_live = catapult = false;
}

static void update_air(float dt) {
    float v = plane.speed;
    float old_y = plane.pos.y;
    float authority = clampf(v / craft->v_lift, 0.15f, 1);

    // Pilot: pitch rate (limited by the g it pulls), roll and yaw rates
    float q = ctl.stick_y * craft->pitch_rate * authority;
    q = clampf(q, (MIN_G - 1) * GRAVITY / v, (craft->max_g - 1) * GRAVITY / v);
    float p = ctl.stick_x * craft->roll_rate * authority;
    float r = ctl.rudder * craft->yaw_rate * authority;

    // Forces: lift along the aircraft's up (1 g at speed, less when slow)
    // and gravity. What is across the flight path bends it.
    float lift = clampf((v * v) / (craft->v_lift * craft->v_lift), 0, 1) * GRAVITY;
    vec3 force = vscale(plane.up, lift);
    force.y -= GRAVITY;
    float along = vdot(force, plane.fwd);
    vec3 across = vsub(force, vscale(plane.fwd, along));
    float bend_up = vdot(across, plane.up) / v, bend_right = vdot(across, plane.right) / v;

    rotate_pair(&plane.fwd, &plane.up, (q + bend_up) * dt);         // Pitch
    rotate_pair(&plane.fwd, &plane.right, (r + bend_right) * dt);   // Yaw
    rotate_pair(&plane.up, &plane.right, p * dt);                   // Roll (right wing down)

    plane.fwd   = vnorm(plane.fwd);
    plane.right = vnorm(vcross(plane.up, plane.fwd));
    plane.up    = vcross(plane.fwd, plane.right);
    plane.g_load = (lift + q * v) / GRAVITY;

    // Speed: thrust, drag (more in hard turns), and gravity along the path
    float g2 = plane.g_load * plane.g_load;
    float induced = g2 > 1 ? craft->induced_drag * (g2 - 1) : 0;
    // (Gravity along the path times SIM_SPEED too: it climbs and dives that much faster, for the same speed)
    plane.speed = clampf(v + (thrust_now() - drag_now(v) - induced + along * sim_scale(v)) * dt, V_MIN, SPEED_LIMIT);
    plane.pos = vadd(plane.pos, vscale(plane.fwd, sim_dist(plane.speed, dt)));

    // Into a ship, onto a carrier's deck, or down on the ground
    const char* hit;
    float deck_y;
    int deck = ship_deck_at(plane.pos.x, plane.pos.z, &deck_y);
    if (deck >= 0 && plane.pos.y < deck_y + GEAR_H && old_y >= deck_y + GEAR_H - 1) {
        deck_touchdown(deck, deck_y);
        return;
    }
    if (ship_solid_at(vadd(plane.pos, v3(0, -GEAR_H * 0.5f, 0)), &hit)) {
        crash(hit);
        return;
    }
    float ground = terrain_height(plane.pos.x, plane.pos.z);
    if (plane.pos.y < ground + GEAR_H) {
        const char* why = touchdown_problem(ground);
        if (why) {
            crash(why);
            return;
        }
        plane.on_ground    = true;
        plane.landed       = true;
        plane.pos.y        = GEAR_H;
        plane.ground_fwd   = vnorm(v3(plane.fwd.x, 0, plane.fwd.z));
        plane.ground_right = v3(plane.ground_fwd.z, 0, -plane.ground_fwd.x);
        plane.nose         = clampf(asin_f(plane.fwd.y), 0, NOSE_MAX);
        plane.speed        = sqrtf(plane.fwd.x * plane.fwd.x + plane.fwd.z * plane.fwd.z) * plane.speed;
    }
}

static void update_ground(float dt) {
    float v = plane.speed;

    // Steering with the rudder or the roll stick: sharp when slow, gentle when fast
    float steer = clampf(ctl.rudder + ctl.stick_x, -1, 1);
    float turn = steer * STEER_RATE * clampf(v / 8.0f, 0, 1) * (v > 30 ? 30 / v : 1);
    rotate_pair(&plane.ground_fwd, &plane.ground_right, turn * dt);
    plane.ground_fwd   = vnorm(plane.ground_fwd);
    plane.ground_right = v3(plane.ground_fwd.z, 0, -plane.ground_fwd.x);

    // Pull to raise the nose once fast enough; otherwise it settles on its wheel
    bool rotating = v > craft->v_rotate * 0.85f && ctl.stick_y > 0.1f;
    plane.nose = clampf(plane.nose + (rotating ? ctl.stick_y * NOSE_RATE : -NOSE_RATE) * dt, 0, NOSE_MAX);

    // Speed: thrust, drag, rolling friction, brakes; or a carrier's wire,
    // which holds it against full power until it stops, then lets go
    float slow = ROLL_FRICTION + (plane.brake ? WHEEL_BRAKE : 0);
    float accel = thrust_now() - drag_now(v);
    if (plane.deck && !plane.arrested && !catapult && v < 10 && plane.throttle >= 100 && !plane.brake) {
        catapult = true;            // Stopped on a carrier at full power: the catapult fires
        weapons_say("CATAPULT");
    }
    if (catapult) {
        if (!plane.deck || plane.throttle < 100 || v > craft->v_rotate * 1.15f)
            catapult = false;
        else
            accel += CAT_ACCEL;
    }
    if (plane.arrested) {
        v -= plane.arrest_decel * dt;
        if (v <= 0.5f) {
            v = 0;
            plane.arrested = false;
            weapons_say("Stopped - wire released");
        }
    } else {
        v += accel * dt;
        v = v > slow * dt ? v - slow * dt : (accel > slow ? v : 0);
    }
    if (v < 0)
        v = 0;
    plane.speed = v;

    float c = cosf(plane.nose), s = sin_f(plane.nose);
    plane.fwd    = v3(plane.ground_fwd.x * c, s, plane.ground_fwd.z * c);
    plane.right  = plane.ground_right;
    plane.up     = vcross(plane.fwd, plane.right);
    plane.g_load = 1;
    plane.pos    = vadd(plane.pos, vscale(plane.ground_fwd, sim_dist(v, dt)));

    if (plane.deck) {
        // On a carrier: off its edge, it drops (and flies if fast enough)
        float deck_y;
        const char* hit;
        if (ship_deck_at(plane.pos.x, plane.pos.z, &deck_y) != plane.deck - 1) {
            leave_deck();
            return;
        }
        plane.pos.y = deck_y + GEAR_H;
        if (ship_solid_at(plane.pos, &hit)) {
            crash(hit);
            return;
        }
        catch_wire();
        if (plane.nose > 0.05f && v >= craft->v_rotate) {
            leave_deck();
            plane.pos.y += 0.5f;
        }
        return;
    }
    plane.pos.y  = GEAR_H;

    if (map_water_at(plane.pos.x, plane.pos.z)) {
        crash("Rolled into the water");
        return;
    }
    if (terrain_height(plane.pos.x, plane.pos.z) > 0) {
        crash("Ran into a hill");
        return;
    }
    if (v > V_GRASS_MAX && !on_pavement(plane.pos.x, plane.pos.z)) {
        crash("Too fast off the pavement");
        return;
    }

    // Off we go
    if (plane.nose > 0.05f && v >= craft->v_rotate) {
        plane.on_ground = false;
        plane.landed = false;
        plane.pos.y += 0.5f;
    }
}

// Fuel burned: by the throttle (what the engine actually gives), more with the afterburner
static void burn_fuel(float dt) {
    if (plane.fuel <= 0)
        return;
    float mil = 100.0f / (craft->fuel_minutes * 60), rate;
    if (plane.afterburner && afterburner_ok()) {
        rate = mil * craft->ab_burn;
    } else {
        int t = plane.throttle < throttle_limit() ? plane.throttle : throttle_limit();
        rate = mil * (FUEL_IDLE + (1 - FUEL_IDLE) * (float)t / 100.0f);
    }
    plane.fuel -= rate * dt;
    if (plane.fuel <= 0) {
        plane.fuel = 0;
        plane.afterburner = false;
    }
}

// Stopped on the pavement of a friendly base or a friendly carrier's deck:
// fuel up, patch up, rearm
static void service(float dt) {
    base_t b;
    plane.servicing = false;
    bool carrier = plane.deck && !ships[plane.deck - 1].enemy;
    if (!plane.on_ground || plane.speed > 1 ||
        (!carrier && (!on_pavement(plane.pos.x, plane.pos.z) || !base_at(plane.pos.x, plane.pos.z, &b) || b.enemy)))
        return;
    bool rearmed = weapons_rearm();
    if (rearmed)
        weapons_say("Rearmed");
    if (plane.fuel < 100 || plane.damage > 0) {
        plane.servicing = true;
        plane.fuel = clampf(plane.fuel + FUEL_REFILL * dt, 0, 100);
        plane.damage = clampf(plane.damage - REPAIR_RATE * dt, 0, 1);
    }
}

void update_flight(float dt) {
    if (plane.crashed)
        return;
    burn_fuel(dt);
    service(dt);
    if (plane.on_ground)
        update_ground(dt);
    else
        update_air(dt);
}
