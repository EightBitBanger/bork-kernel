/*
 * flight: a basic flight simulator in the spirit of F-15 Strike Eagle, for
 * the desktop.
 *
 *     flight                  the start menu
 *     flight maps/gulf.map    ... with this map chosen (".map" may be left off)
 *
 * First pick a pilot, or sign up a new one (pilot.c). Pilots rise from
 * Second Lieutenant to General as their score grows: what you destroy on a
 * flight counts once you are down safe on the ground and press Escape. A
 * crash, being shot down or ejecting loses that flight's score, and a crash
 * or being shot down (not ejecting) kills the pilot for good.
 * A flight that scores well enough earns a medal, shown by the pilot's name
 * (and beside the tombstone afterwards).
 *
 * Then the menu lists the theaters (the endless made-up world, and every .map
 * file in maps/, here or next to the program) and the aircraft, each type
 * with its own speed, handling and fuel (the table in aircraft.c). Up and
 * Down choose, Left and Right switch lists, Enter (or a double click, or
 * FLY) flies; Escape goes back to the pilots. When a flight ends (Escape
 * once down, or after a crash a few seconds of black screen) you are back
 * at the pilots.
 *
 * You start at the end of a runway with the engine at idle. Throttle up,
 * pull back once past 140 knots, raise the gear and fly. Without a map the
 * world is endless farmland dotted with low-poly pyramid hills, air bases
 * (runway, taxiways, hangars, tower) and SAM sites (a radar ringed by
 * missile launchers). The HUD points to the nearest friendly base and
 * marks SAM sites: red ones are enemy, green ones friendly.
 *
 * A map (maps/README.txt describes the format) places all of that by hand,
 * with named cities, zones, targets, mountains, colored ground (seas,
 * rivers, farmland) and its own ground color. The HUD names the zone you
 * are over and marks targets within 10 km. Past the map's edges is plain
 * ground with nothing on it.
 *
 * Enemy SAMs: a site's radar sees you within 12 km when you are more than
 * 60 m above the ground and no hill is in the way (RADAR on the HUD). Stay
 * seen for a few seconds and it locks (LOCK) and launches (MISSILE). The
 * missile steers for where you will be, but it can only pull so many g, the
 * faster the wider: break hard across its path just before it arrives and
 * it overshoots; run and it runs out of energy once its motor burns out;
 * drop below the radar or behind a hill and it flies on blind. A hit
 * usually kills the engine (sometimes it only loses part of its power: the
 * throttle stops at what is left); then glide, to a runway if you can. A
 * hit on a dead engine finishes you. S switches the enemies off for
 * practice.
 *
 * Enemy fighters: an enemy base sends up to two after you when you come
 * within 30 km, one at a time. They orbit the base until they spot you,
 * then attack with heat seekers (dodge them like SAMs, or keep them from
 * getting behind you). Shoot back with air-to-air missiles: W arms them,
 * hold the fighter in the circle until the box says LOCK, then Enter. The
 * air-to-ground missiles (W again) lock onto a map's targets and enemy SAM
 * and radar sites the same way. Land at a friendly base to rearm.
 *
 * Fuel: a full load lasts about 15 minutes at full military power, much
 * less on afterburner. Run dry and the engine stops. Land at a friendly base
 * and stop on its pavement to refuel (and get repaired); enemy bases (red on
 * the radar) won't help.
 *
 * The radar scope at the bottom shows what is around you, heading up:
 * bases (friendly green, enemy red), SAM sites and missiles.
 *
 * The HUD's layout (where each part sits) is set by the UI_* defines at the
 * top of hud.c.
 *
 * Landing: come down on a runway with the gear down, slower than 300
 * knots, sinking less than about 1400 feet a minute, wings level. Get any
 * of that wrong (or land on the grass, or hit a hill) and you crash. Only
 * the touchdown counts: you can roll as fast as you like on the way up.
 * On the ground you can taxi anywhere, grass too, under 180 knots.
 *
 * Carriers: fly down the angled landing deck (the HUD's B points at the
 * nearest friendly carrier when it's closer than a base, and DECK gives the
 * landing deck's heading) with the gear down, and touch down on or just
 * short of the four wires. The hook catches one and stops you; land past
 * them and it's a bolter: full power and go round. Once stopped the wire
 * lets go and you're refueled and rearmed. Full power while stopped on the
 * deck (brakes off) fires the catapult. Come in low and you hit the stern.
 *
 * Keys (the window only reports key presses, so the stick is "nudged"):
 *   Up / Down       push / pull the stick (nose down / nose up)
 *   Left / Right    roll (on the ground: steer)
 *   , and .         rudder left / right (on the ground: steer)
 *   Space           center the stick
 *   + - (PgUp PgDn) throttle in 10% steps; past 100% lights the afterburner
 *   A               throttle all the way: full afterburner, or as much as a
 *                   damaged engine gives
 *   0 - 9           throttle 100% (0) or 10% - 90%
 *   L               landing gear up / down
 *   B               speed brake in the air, wheel brakes on the ground
 *   M               fly with the mouse: it takes the pointer (moving the
 *                   mouse moves the stick, which stays put until moved back
 *                   or Space centers it); M again gives the pointer back
 *   I               invert pitch (keys and mouse)
 *   R               radar range: 40, 20, 10 km, then 40 again
 *   N               map, right of the radar, on / off
 *   Z / X           map zoom in / out: 50, 100, 200 km across, the whole map
 *   O               night on / off (also N in the menu)
 *   C               chaff: may fool radar-guided missiles (SAMs)
 *   F               flare: may fool heat seekers (the fighters' missiles)
 *   W               arm the other of the two kinds of weapon carried (picked
 *                   in the menu, 1 and 2: Sidewinder, AMRAAM, Maverick,
 *                   HARM, Paveway, JDAM, tactical nuke; weapons.c)
 *   T               lock the next target in the seeker's view
 *   Enter or click  fire at what is locked
 *   S               enemies (SAMs and fighters) on / off
 *   D               chunky pixels (renders at half size, faster)
 *   V               frame rate on / off
 *   H or F1         help on / off
 *   P               pause
 *   Escape          flying: eject (the seat fires, the chute opens a
 *                   moment later; Left / Right steer it down). Stopped or
 *                   rolling on a runway or a carrier: the flight's score
 *                   goes to the pilot, back to the pilots. Crashed, or down
 *                   in the chute: back to the pilots, the score lost
 *   Enter           (down in the chute) back to the pilots
 *   Ctrl+Q          quit (in the menu too)
 *
 * A tap moves the stick part way and holds it briefly; holding the key
 * (key repeat) moves it all the way. Let go and it springs back.
 *
 * The parts are listed in flight.h. Built on os/wm.h, the same way as
 * minesweeper and 8086tiny. Mouse flying uses the dwm's pointer lock
 * (wm_window_lock_pointer); with a dwm that has none, the pointer's place in
 * the window is the stick instead. Needs SEC_GFX and SEC_IPC (the default
 * clearance has both).
 */

// Mouse pitch. 1: move the mouse up (away from you) to raise the nose, the
// way mouse look works in games. 0: like a joystick, pull the mouse back
// (toward you) to raise the nose. I flips whichever this picks while flying.
#define MOUSE_FLIP_Y    1

// Stick travel per pixel of mouse movement while the pointer is locked
#define MOUSE_SENS      (1.0f / 200.0f)

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/os.h>
#include <os/input.h>

#include "flight.h"

// Pitch from the mouse moving down by `down`: + pulls (nose up)
static float mouse_pitch(float down) {
    return MOUSE_FLIP_Y ? -down : down;
}

// Take or give back the pointer as mouse flying and pausing call for
static bool in_menu = true;         // The start menu is up (else flying)
static uint64_t blackout_until;     // After a fatal crash: black until then, then the pilots (0: not)

#define BLACKOUT_MS     4000

static void start_flight(uint64_t now) {
    in_menu = false;
    paused = false;
    world_time = 0;
    sortie_start();
    blackout_until = 0;
    map_random_start(hash2((int32_t)now, (int32_t)(now >> 32), 77));   // A different friendly base or carrier each time
    reset_flight(false);
    ctl.lock_try = true;
    char title[96];
    snprintf(title, sizeof(title), "Flight - %s%s%s", craft->name, map.loaded ? " - " : "", map.loaded ? map.name : "");
    wm_window_set_title(win, title);
}

// The flight is over: back to the pilots, the score banked if it ended well
static void to_menu(bool safe) {
    sortie_end(safe);
    blackout_until = 0;
    in_menu = true;
    pilot_screen = true;
    paused = false;
    wm_window_set_title(win, "Flight");
}

static void update_pointer_lock(void) {
    bool want = ctl.mouse && !paused && !ctl.no_lock && !in_menu;
    if (want == ctl.locked)
        return;
    if (want) {
        if (!ctl.lock_try)
            return;                     // Refused before: wait for the focus or a key
        ctl.lock_try = false;
        if (wm_window_lock_pointer(win, true) == 0) {
            ctl.locked = true;
            ctl.have_last = false;
        } else if (errno == EINVAL || errno == ENOSYS) {
            ctl.no_lock = true;         // An older dwm: the pointer's place is the stick
        }
    } else {
        wm_window_lock_pointer(win, false);
        ctl.locked = false;
    }
}

static void handle_key(uint16_t key, uint8_t mods, uint64_t now) {
    if (key == 0x11 || ((mods & INPUT_MOD_CTRL) && (key == 'q' || key == 'Q'))) {
        quit = true;                    // Ctrl+Q, anywhere
        return;
    }
    if (mods & INPUT_MOD_CTRL)
        return;
    if (blackout_until)
        return;                         // Nothing to do in the dark but wait
    if (in_menu && pilot_screen) {
        if (pilot_key(key))
            pilot_screen = false;       // On to the theater and aircraft
        return;
    }
    if (in_menu) {
        if (key == KEY_ESCAPE)
            pilot_screen = true;
        else if (menu_key(key))
            start_flight(now);
        return;
    }
    float pitch_dir = ctl.invert ? 1.0f : -1.0f;    // Up arrow pushes, unless inverted
    switch (key) {
        case KEY_UP:        stick_press(&ctl.cmd_y, &ctl.hold_y, pitch_dir, now);  return;
        case KEY_DOWN:      stick_press(&ctl.cmd_y, &ctl.hold_y, -pitch_dir, now); return;
        case KEY_LEFT:      stick_press(&ctl.cmd_x, &ctl.hold_x, -1, now);         return;
        case KEY_RIGHT:     stick_press(&ctl.cmd_x, &ctl.hold_x, 1, now);          return;
        case KEY_PAGE_UP:   key = '+'; break;
        case KEY_PAGE_DOWN: key = '-'; break;
        case KEY_ESCAPE:
            // Flying: bail out. Down safely (or down in the chute, or
            // crashed): back to the menu
            if (!plane.crashed && !plane.on_ground)
                eject();
            else if (!plane.ejected || chute.down)
                to_menu(!plane.crashed && !plane.ejected);
            return;
        case 0x3B00:        show_help = !show_help; return;                   // F1
        case KEY_ENTER: case '\r': case '\n':
            if (!plane.crashed)
                weapons_fire();
            else if (!plane.ejected || chute.down)      // Not while still coming down
                to_menu(false);
            return;
    }
    if (key >= 0x80)
        return;

    switch (tolower(key)) {
        case ',': case '<': stick_press(&ctl.cmd_z, &ctl.hold_z, -1, now); break;
        case '.': case '>': stick_press(&ctl.cmd_z, &ctl.hold_z, 1, now);  break;
        case ' ':           reset_controls(); break;
        case '+': case '=':
            // A damaged engine won't go past what it still gives
            if (plane.throttle + 10 <= throttle_limit()) plane.throttle += 10;
            else if (plane.throttle >= 100 && afterburner_ok()) plane.afterburner = true;
            break;
        case '-': case '_':
            if (plane.afterburner) plane.afterburner = false;
            else if (plane.throttle > 0) plane.throttle -= 10;
            break;
        case 'b': plane.brake = !plane.brake;   break;
        case 'l':
            if (!plane.on_ground && !plane.crashed)
                plane.gear_down = !plane.gear_down;
            break;
        case 'm':
            ctl.mouse = !ctl.mouse;
            ctl.lock_try = true;
            ctl.mouse_x = ctl.mouse_y = 0;
            ctl.cmd_x = ctl.cmd_y = 0;
            break;
        case 'i': ctl.invert = !ctl.invert;     break;
        case 'r': radar_zoom();                 break;
        case 'n': nav_map_toggle();             break;
        case 'z': nav_map_zoom(-1);             break;
        case 'x': nav_map_zoom(1);              break;
        case 'o': night = !night;               break;
        case 'c': weapons_chaff();              break;
        case 'f': weapons_flare();              break;
        case 'a':
            // Everything the engine has: full afterburner, or what is left of a damaged one
            if (afterburner_ok()) {
                plane.throttle = 100;
                plane.afterburner = true;
            } else {
                plane.throttle = throttle_limit();
                plane.afterburner = false;
            }
            break;
        case 's': sam_set_enabled(!sams_enabled); break;
        case 'w': weapons_select();             break;
        case 't': weapons_next_target();        break;
        case 'd': chunky = !chunky;             break;
        case 'v': show_fps = !show_fps;         break;
        case 'h': show_help = !show_help;       break;
        case 'p': paused = !paused; ctl.lock_try = true; break;
        default:
            if (key >= '0' && key <= '9') {
                int want = key == '0' ? 100 : (key - '0') * 10;
                plane.afterburner = false;
                plane.throttle = want <= throttle_limit() ? want : throttle_limit() / 10 * 10;
            }
            break;
    }
}

static void handle_event(const wm_event_t* ev, uint64_t now) {
    switch (ev->type) {
        case WM_EVENT_CLOSE:
        case WM_EVENT_DESTROYED:
            if (ev->window == win)
                quit = true;
            break;
        case WM_EVENT_RESIZED:
            surf = wm_window_surface(win);
            break;
        case WM_EVENT_KEY:
            handle_key(ev->key, ev->mods, now);
            break;
        case WM_EVENT_MOUSE_DOWN:
            if (blackout_until)
                break;
            if (in_menu && pilot_screen) {
                if (ev->button == INPUT_BTN_LEFT && pilot_click(ev->x, ev->y, ev->clicks))
                    pilot_screen = false;
            } else if (in_menu && ev->button == INPUT_BTN_LEFT && menu_click(ev->x, ev->y, ev->clicks))
                start_flight(now);
            else if (!in_menu && !paused && ev->button == INPUT_BTN_LEFT)
                weapons_fire();
            break;
        case WM_EVENT_MOUSE_MOVE:
            if (in_menu) {
                if (pilot_screen)
                    pilot_move(ev->x, ev->y);
                else
                    menu_move(ev->x, ev->y);
                break;
            }
            if (!ctl.mouse)
                break;
            if (ctl.locked) {
                // Locked: the mouse moves the stick, which stays where it is put
                if (ctl.have_last) {
                    ctl.mouse_x = clampf(ctl.mouse_x + (float)(ev->x - ctl.last_x) * MOUSE_SENS, -1, 1);
                    ctl.mouse_y = clampf(ctl.mouse_y + mouse_pitch((float)(ev->y - ctl.last_y)) * MOUSE_SENS, -1, 1);
                }
                ctl.last_x = ev->x;
                ctl.last_y = ev->y;
                ctl.have_last = true;
            } else if (surf) {
                // Not locked: full deflection 40% of the way from the middle to the edge
                float w = (float)gfx_surface_width(surf), h = (float)gfx_surface_height(surf);
                ctl.mouse_x = clampf(((float)ev->x - w * 0.5f) / (w * 0.2f), -1, 1);
                ctl.mouse_y = clampf(mouse_pitch(((float)ev->y - h * 0.5f) / (h * 0.2f)), -1, 1);
            }
            break;
        case WM_EVENT_MOUSE_LEAVE:
            if (!ctl.locked)
                ctl.mouse_x = ctl.mouse_y = 0;
            break;
        case WM_EVENT_FOCUS_GAINED:
            ctl.lock_try = true;
            break;
        case WM_EVENT_FOCUS_LOST:
            ctl.locked = false;         // The dwm let go of the pointer
            ctl.cmd_x = ctl.cmd_y = ctl.cmd_z = 0;
            break;
    }
}

int main(int argc, char** argv) {
    if (wm_connect(2000) < 0) {
        fprintf(stderr, APP_NAME ": needs the desktop (dwm): %s\n", strerror(errno));
        return 1;
    }

    wm_window_desc_t d;
    memset(&d, 0, sizeof(d));
    d.title      = "Flight";
    d.x          = WM_POS_DEFAULT;
    d.y          = WM_POS_DEFAULT;
    d.width      = WIN_W;
    d.height     = WIN_H;
    d.min_width  = 320;
    d.min_height = 200;
    d.style      = WM_STYLE_RESIZABLE;
    win = wm_window_create(&d);
    if (!win) {
        fprintf(stderr, APP_NAME ": can't create a window: %s\n", strerror(errno));
        wm_disconnect();
        return 1;
    }
    surf = wm_window_surface(win);

    hud_init();
    pilot_init();
    menu_init(argv[0], argc > 1 ? argv[1] : NULL);
    reset_flight(false);

    uint64_t last = os_time_ms(), lag = 0;
    render_menu(last);
    wm_window_focus(win);

    while (!quit) {
        uint64_t frame_start = os_time_ms();

        // Physics in fixed steps; after a long stall don't try to catch up
        uint64_t now = os_time_ms();
        lag += now - last;
        last = now;
        if (lag > 200)
            lag = 200;
        while (lag >= STEP_MS) {
            float dt = STEP_MS / 1000.0f;
            update_stick(dt, now);
            if (!paused && !in_menu && !blackout_until) {
                update_flight(dt);
                ships_update(dt);
                eject_update(dt);
                air_update(dt);
                traffic_update(dt);
                sam_update(dt);
                weapons_update(dt);
                world_time += dt;
            }
            lag -= STEP_MS;
        }
        // Crashed (or shot down) with the pilot still aboard: the pilot is
        // lost, the screen goes black, then back to the pilots
        if (!in_menu && !blackout_until && plane.crashed && !plane.ejected) {
            pilot_killed();
            blackout_until = now + BLACKOUT_MS;
            paused = false;
        }
        if (blackout_until && now >= blackout_until)
            to_menu(false);
        update_pointer_lock();

        if (blackout_until)
            render_black(now);
        else if (in_menu)
            render_menu(now);
        else
            render(now);

        // Wait for the next frame, handling input as it comes
        for (;;) {
            now = os_time_ms();
            int32_t wait = (int32_t)(frame_start + FRAME_MS - now);
            if (wait < 0)
                wait = 0;
            wm_event_t ev;
            int r = wm_wait_event(&ev, wait);
            if (r < 0) {
                quit = true;            // The dwm went away
                break;
            }
            if (r == 0) {
                if (wait == 0)
                    break;
                continue;
            }
            handle_event(&ev, os_time_ms());
            if (quit)
                break;
        }
    }

    if (ctl.locked)
        wm_window_lock_pointer(win, false);
    wm_disconnect();
    return 0;
}
