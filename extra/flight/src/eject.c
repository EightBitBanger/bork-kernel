/*
 * eject.c: bailing out.
 *
 * E fires the seat: it shoots you up out of the jet, which flies on
 * without you, noses over and goes in. After CHUTE_DELAY the parachute
 * opens and you float down at CHUTE_SINK; left and right steer it. The
 * view follows from behind the canopy. Where you come down decides how
 * it ends: nearer a friendly base than an enemy one and you are picked
 * up, nearer an enemy one and you are captured. Coming down before the
 * parachute has opened is the end of you.
 *
 * While you hang under the canopy, `plane` is you (its position and
 * heading), and it counts as crashed: SAMs and fighters lose interest.
 */

#include <string.h>

#include "flight.h"

#define SEAT_KICK       38.0f       // m/s up, out of the jet
#define SEAT_DRAG       0.55f       // Before the chute: how fast the air slows you, per second
#define CHUTE_DELAY     2.5f        // Seconds from the seat firing to the chute open
#define CHUTE_SINK      6.5f        // m/s down under the canopy
#define CHUTE_GLIDE     6.0f        // ... and forward
#define CHUTE_TURN      0.7f        // Radians a second, at full stick
#define CHUTE_SAFE      12.0f       // Hitting the ground faster than this (m/s) kills

chute_t  chute;
bandit_t wreck;

void eject_reset(void) {
    memset(&chute, 0, sizeof(chute));
    memset(&wreck, 0, sizeof(wreck));
}

bool eject(void) {
    if (plane.crashed || plane.on_ground)
        return false;
    // The jet goes on alone (drawn like a fighter)
    memset(&wreck, 0, sizeof(wreck));
    wreck.active = true;
    wreck.name = craft->name;
    wreck.type = 1;
    wreck.pos = plane.pos;
    wreck.fwd = plane.fwd;
    wreck.up = plane.up;
    wreck.speed = plane.speed;

    memset(&chute, 0, sizeof(chute));
    chute.active = true;
    chute.pos = vadd(plane.pos, vscale(plane.up, 3));
    chute.vel = vadd(vscale(plane.fwd, plane.speed), vscale(vnorm(vadd(plane.up, v3(0, 1, 0))), SEAT_KICK));
    chute.heading = atan2_f(plane.fwd.x, plane.fwd.z);

    crash(NULL);
    plane.ejected = true;
    plane.afterburner = false;
    plane.throttle = 0;
    return true;
}

static void fly_wreck(float dt) {
    if (!wreck.active)
        return;
    // Nose over, rolling, faster and faster down
    wreck.fwd = vnorm(vadd(wreck.fwd, v3(0, -0.22f * dt, 0)));
    wreck.state_time += dt;
    vec3 right = vcross(v3(0, 1, 0), wreck.fwd);
    right = vdot(right, right) < 1e-6f ? v3(1, 0, 0) : vnorm(right);
    vec3 up = vcross(wreck.fwd, right);
    float roll = wreck.state_time * 1.2f;
    wreck.up = vadd(vscale(up, cosf(roll)), vscale(right, sin_f(roll)));
    wreck.speed = approach(wreck.speed, 180, 10 * dt);
    wreck.pos = vadd(wreck.pos, vscale(wreck.fwd, sim_dist(wreck.speed, dt)));
    float ground = terrain_height(wreck.pos.x, wreck.pos.z);
    if (wreck.pos.y <= ground + 3) {
        blast_at(v3(wreck.pos.x, ground + 5, wreck.pos.z));
        wreck.active = false;
    }
}

// How it ends, by where you came down
static const char* outcome(float x, float z) {
    chute.safe = true;
    if (map_water_at(x, z))
        return "Down in the water - a helicopter picks you up";
    base_t near[64];
    int n = bases_near(x, z, 150000, near, 64);
    float friend_d = 1e12f, enemy_d = 1e12f;
    for (int i = 0; i < n; i++) {
        float dx = near[i].x - x, dz = near[i].z - z, d = dx * dx + dz * dz;
        if (near[i].enemy && d < enemy_d)
            enemy_d = d;
        else if (!near[i].enemy && d < friend_d)
            friend_d = d;
    }
    if (enemy_d < friend_d) {
        chute.safe = false;
        return "Behind enemy lines - captured";
    }
    return "Safe on friendly ground - rescued";
}

void eject_update(float dt) {
    fly_wreck(dt);
    if (!chute.active || chute.down)
        return;
    chute.time += dt;
    float ground = terrain_height(chute.pos.x, chute.pos.z);
    if (!chute.open) {
        chute.vel.y -= GRAVITY * dt;
        chute.vel = vscale(chute.vel, 1.0f - SEAT_DRAG * dt);
        if (chute.time >= CHUTE_DELAY && chute.pos.y > ground + 15)
            chute.open = true;
    } else {
        chute.heading += ctl.stick_x * CHUTE_TURN * dt;
        vec3 want = v3(sin_f(chute.heading) * CHUTE_GLIDE, -CHUTE_SINK, cosf(chute.heading) * CHUTE_GLIDE);
        chute.vel = vadd(chute.vel, vscale(vsub(want, chute.vel), clampf(1.5f * dt, 0, 1)));
    }
    chute.pos = vadd(chute.pos, vscale(chute.vel, dt));

    if (chute.pos.y <= ground) {
        chute.pos.y = ground;
        chute.down = true;
        chute.result = -chute.vel.y > CHUTE_SAFE ? "Hit the ground before the chute opened"
                                                 : outcome(chute.pos.x, chute.pos.z);
        chute.vel = v3(0, 0, 0);
    }

    // You, for the HUD, radar and map
    plane.pos = vadd(chute.pos, v3(0, 1.7f, 0));
    plane.fwd = v3(sin_f(chute.heading), 0, cosf(chute.heading));
    plane.up = v3(0, 1, 0);
    plane.right = vcross(plane.up, plane.fwd);
    plane.speed = vlen(chute.vel);
}
