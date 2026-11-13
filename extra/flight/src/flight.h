/*
 * flight.h: what the parts of the flight simulator share.
 *
 *   main.c      window, keys, mouse and the frame loop
 *   world.c     the world: fields, hills, air bases, SAM sites; endless
 *               and made up, or from a map file (map.c)
 *   map.c       theater maps: reading the text file, what it puts where
 *   aircraft.c  the aircraft and its controls: flying, take-off, landing
 *   sam.c       SAM sites at work: radar lock; every missile in flight, explosions
 *   air.c       enemy fighters: take off, orbit, attack
 *   traffic.c   friendly fighters and airliners flying between bases
 *   country.c   villages and farms out in the country
 *   ships.c     carriers, warships, missile boats and tankers at sea
 *   weapons.c   your missiles: arming, target lock, firing, what they destroy
 *   render.c    the 3D picture: sky, ground, polygons, objects
 *   hud.c       the green HUD and its text
 *   menu.c      the start menu: theater, aircraft and weapons
 *   pilot.c     the pilot screen before it: ranks, score, kept in the registry
 *
 * Meters and seconds; x east, y up, z north.
 */

#ifndef FLIGHT_H
#define FLIGHT_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <os/gfx.h>
#include <os/wm.h>

#ifndef APP_NAME
#define APP_NAME        "flight"
#endif

#define COUNT(a)        (sizeof(a) / sizeof((a)[0]))

// Window and timing
#define WIN_W           640
#define WIN_H           400
#define MAX_W           2048        // Largest picture drawn (bigger windows show the rest blank)
#define MAX_H           1536
#define FRAME_MS        33          // About 30 frames per second
#define STEP_MS         10          // Physics step

// How far you can see, in meters: everything fades into the haze by here,
// and nothing is drawn past it. Farther is slower (more to draw); 15000 is
// what the game was tuned with, 25000 or so still runs well on a fast
// machine, 8000 helps a slow one.
#define VIEW_DISTANCE   15000.0f

// World
#define TILE            1000.0f     // Field squares
#define TILE_RANGE      ((int)(VIEW_DISTANCE / TILE) - 2)  // Fields drawn this many squares around
#define HILL_CELL       3000.0f     // At most one hill per cell
#define FOG_FAR         VIEW_DISTANCE                       // Fully hazed at this distance
#define NEAR_Z          1.0f
#define FOV_SCALE       0.714f      // Focal length / width: 70 degrees across

// Air bases: at most one per BASE_CELL square, within BASE_SPREAD of its
// middle, so hills and fields can keep clear of it. The first is at (0, 0).
#define BASE_CELL       20000.0f
#define BASE_SPREAD     5000.0f
#define BASE_CLEAR      4000.0f     // No hill centers this close (either axis)
#define BASE_GRASS      2200.0f     // No fields this close
#define BASE_ENEMY_PCT  30          // Bases this far out (BASE_HOME_CLEAR) that are enemy ones
#define BASE_HOME_CLEAR 30000.0f
#define RUNWAY_HALF_LEN 1500.0f
#define RUNWAY_HALF_W   30.0f
#define DETAIL_RANGE    6000.0f     // Runway markings drawn this close

// SAM sites: at most one per SAM_CELL square, none near a base; enemy ones
// only well away from the first base, so a flight starts in peace
#define SAM_CELL        8000.0f
#define SAM_SPREAD      2500.0f
#define SAM_BASE_CLEAR  6000.0f
#define SAM_HOME_CLEAR  15000.0f
#define SAM_RING        70.0f       // Launchers this far around the radar

// How far things are drawn, each no farther than VIEW_DISTANCE: lower these
// for a faster picture with a long VIEW_DISTANCE (the map's land, water and
// hills still go out to it). Anything smaller than about a pixel isn't drawn either.
#define NEAR_OF(d)      ((d) < VIEW_DISTANCE ? (d) : VIEW_DISTANCE)
#define FIELD_DRAW_RANGE    NEAR_OF(20000.0f)   // Farm field squares
#define BASE_DRAW_RANGE     NEAR_OF(25000.0f)   // Runways and aprons
#define BASE_BUILDING_RANGE NEAR_OF(12000.0f)   // Hangars and towers
#define TARGET_DRAW_RANGE   NEAR_OF(15000.0f)   // A map's targets: pads and buildings
#define BUILDING_RANGE      NEAR_OF(10000.0f)   // A map's other buildings (cities)
#define COUNTRY_DRAW_RANGE  NEAR_OF(8000.0f)    // Village and farm buildings
#define COUNTRY_FIELD_RANGE NEAR_OF(12000.0f)   // Farm fields
#define SAM_DRAW_RANGE      NEAR_OF(12000.0f)   // SAM and radar sites
#define PLANE_DRAW_RANGE    NEAR_OF(15000.0f)   // Fighters
#define TRAFFIC_DRAW_RANGE  NEAR_OF(20000.0f)   // Friendly traffic (airliners are big)

// SAM behavior
#define RADAR_RANGE     12000.0f    // Sees the aircraft this far ...
#define RADAR_FLOOR     60.0f       // ... when it is this high above the ground (fly lower to hide)
#define LOCK_TIME       3.0f        // Seconds of tracking before a launch
#define LAUNCH_RANGE    10000.0f
#define STEALTH_MIN     800.0f      // Even the stealthiest aircraft is seen this close
#define LAUNCH_GAP      10.0f       // Seconds between launches from one site
#define RELOAD_TIME     45.0f       // A launcher reloads after this
#define MAX_MISSILES    16
#define MAX_INBOUND     2           // Enemy missiles in the air at once, from all sites and fighters
#define MAX_BLASTS      8
#define MISSILE_BOOST   4.0f        // Seconds of motor
#define MISSILE_THRUST  220.0f      // m/s^2 while it burns (to about Mach 2.5)
#define MISSILE_DRAG    5e-5f       // Times speed squared
#define MISSILE_MAX_G   8.0f        // Hardest turn it can pull: fast and stiff, so a hard break can beat it
#define MISSILE_LIFE    30.0f
#define MISSILE_MIN_V   150.0f      // Slower than this after burnout: it falls and blows up
#define MISSILE_FUSE    15.0f       // Proximity fuse
#define MISSILE_LAG     0.4f        // Seconds its guidance takes to follow a change (why a late break works)
#define TRAIL_LEN       48          // Smoke trail points
#define TRAIL_STEP      0.08f       // Seconds between them
#define TRAIL_FADE      3.0f        // A trail stays this long after its missile is gone

// Aircraft
#define GRAVITY         9.81f
#define START_ALT       1800.0f     // Air start: above the tallest hill
#define V_MIN           20.0f
#define SPEED_LIMIT     30000.0f    // m/s: no real top speed (diving from very high, only drag stops you)
#define SPEED_BOOST     1.12f       // Every type's top speed times this (its thrust times the square); fuel burn is the same
// Sim speed: everything that flies (the player, enemy fighters, traffic,
// missiles and bombs) covers ground this many times as fast as its airspeed
// says. The airspeeds themselves (the HUD, the menu, stall and landing
// speeds) don't change. It eases in between SIM_SLOW_V and SIM_FAST_V, so
// takeoffs, landings, taxiing and ships move at their own speed.
#define SIM_SPEED       1.8f
#define SIM_SLOW_V      80.0f       // m/s (155 knots): no faster than its own speed below this
#define SIM_FAST_V      180.0f      // m/s (350 knots): the full SIM_SPEED from here up
#define BRAKE_DRAG      3.2f        // Drag times this with the speed brake out
#define GEAR_DRAG       1.4f        // ... and this with the gear down
// How each type flies (speeds, thrust, turn rates, fuel): the table in aircraft.c

// Damage (0: none, 1: engine dead). A missile hit usually kills the engine
// outright (HIT_KILL_PCT in 100), otherwise it takes HIT_DAMAGE_MIN to
// HIT_DAMAGE_MAX of what is left of it. The throttle can't go past what the
// engine still gives, the afterburner needs it nearly whole, and a hit on a
// dead engine finishes the aircraft. With the engine out it glides: find a
// runway or come down somewhere.
#define HIT_KILL_PCT    75
#define HIT_DAMAGE_MIN  0.4f
#define HIT_DAMAGE_MAX  0.8f
#define AB_DAMAGE_MAX   0.15f       // More damage than this: no afterburner
#define HIT_FLASH       0.6f        // Seconds the picture flashes red after a hit

// Fuel, in percent of a full load. How long a load lasts is the aircraft's
// (aircraft.c); idle burns FUEL_IDLE of what full throttle does. Empty: no
// thrust. Stopped on the pavement of a friendly base, the aircraft is
// refueled (and repaired) FUEL_REFILL a second.
#define FUEL_IDLE       0.1f
#define FUEL_LOW        15.0f       // Warning below this
#define FUEL_REFILL     8.0f        // Percent a second
#define REPAIR_RATE     0.08f       // Damage taken off a second, at the same time
#define MIN_G           -3.0f

// On the ground
#define GEAR_H          3.0f        // Eye height on the wheels
#define PULL_UP_SPEED   257.2f      // Faster than this (500 knots) ...
#define PULL_UP_AGL     91.4f       // ... and lower than this (300 feet), gear up: the HUD flashes red
#define PULL_UP_ARM     304.8f      // Not after takeoff until first above this (1000 feet)
#define ARREST_RUN      95.0f       // A carrier's wire stops the aircraft in this far
#define CAT_ACCEL       30.0f       // A carrier's catapult adds this (m/s^2) to the engines
#define SINK_MAX        7.0f        // m/s down at touchdown (about 1400 ft/min)
#define BANK_MAX        0.35f       // sin of the most bank at touchdown (20 degrees)
#define V_GRASS_MAX     92.6f       // Off the pavement faster than this (180 knots) crashes
#define ROLL_FRICTION   0.3f        // m/s^2
#define WHEEL_BRAKE     8.0f        // m/s^2
#define STEER_RATE      0.5f        // rad/s at full steer, slow
#define NOSE_RATE       0.25f       // rad/s raising or lowering the nose
#define NOSE_MAX        0.26f       // 15 degrees

// Stick
#define STICK_STEP      0.34f       // Per key press
#define STICK_HOLD_MS   550         // A first press holds the stick this long (covers the key repeat delay)
#define STICK_REPEAT_MS 200         // A repeat (key still held) this long, so it springs back soon after
#define STICK_RETURN    3.0f        // Back to center, per second
#define STICK_SLEW      7.0f        // Stick follows the command this fast

// Colors
#define C_ZENITH        GFX_RGB(38, 88, 168)
#define C_HAZE          GFX_RGB(176, 198, 214)
#define C_GROUND        GFX_RGB(86, 128, 58)
#define C_RUNWAY        GFX_RGB(70, 70, 74)
#define C_TAXIWAY       GFX_RGB(84, 84, 86)
#define C_APRON         GFX_RGB(100, 100, 98)
#define C_MARKING       GFX_RGB(232, 232, 226)
#define C_SAM_PAD       GFX_RGB(140, 128, 92)
#define C_SAM_ENEMY     GFX_RGB(150, 72, 52)
#define C_SAM_FRIEND    GFX_RGB(92, 110, 72)
#define C_MISSILE       GFX_RGB(236, 236, 230)
#define C_SMOKE         GFX_RGB(214, 214, 210)
#define C_FLAME         GFX_RGB(255, 190, 70)
#define C_SCORCHED      GFX_RGB(64, 56, 48)         // Where something was destroyed
#define C_RUBBLE        GFX_RGB(84, 78, 70)
#define C_HUD_DAY       GFX_RGB(0, 0, 0)            // HUD lines by day ...
#define C_HUD           GFX_RGB(80, 255, 110)       // ... and at night (radar and map always)
#define C_HUD_PULL_UP   GFX_RGB(230, 30, 20)        // Flashing: fast and low
#define C_TEXT          GFX_RGB(0, 0, 0)            // HUD text
#define C_TEXT_SHADOW   GFX_RGB(200, 224, 204)      // ... drawn over this, a pixel down and right
#define C_TEXT_NIGHT    GFX_RGB(214, 236, 220)      // ... at night
#define C_TEXT_ALERT    GFX_RGB(220, 30, 20)        // Gear, brakes, warnings
#define C_RADAR_BG      GFX_RGB(0, 0, 0)            // Radar scope, blended over the picture
#define C_HEAT          GFX_RGB(255, 170, 30)       // Heat seekers on the radar and HUD
#define C_OWN_MISSILE   GFX_RGB(110, 210, 255)      // Yours on the radar
#define C_FRIEND        GFX_RGB(80, 255, 110)
#define C_ENEMY         GFX_RGB(255, 80, 60)
#define C_WARN          GFX_RGB(255, 80, 60)

//
// Vectors and small helpers
//

typedef struct { float x, y, z; } vec3;
typedef struct { float x, y; } vec2;

static inline vec3 v3(float x, float y, float z) { vec3 v = { x, y, z }; return v; }
static inline vec3 vadd(vec3 a, vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline vec3 vsub(vec3 a, vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline vec3 vscale(vec3 a, float k) { return v3(a.x * k, a.y * k, a.z * k); }
static inline float vdot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline vec3 vcross(vec3 a, vec3 b) {
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline float vlen(vec3 a) { return sqrtf(vdot(a, a)); }
static inline vec3 vnorm(vec3 a) {
    float l = vlen(a);
    return l > 1e-6f ? vscale(a, 1.0f / l) : a;
}

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
// How many times as fast as its speed v (m/s) something moves (SIM_SPEED)
static inline float sim_scale(float v) {
    float t = clampf((v - SIM_SLOW_V) / (SIM_FAST_V - SIM_SLOW_V), 0, 1);
    return 1.0f + (SIM_SPEED - 1.0f) * t * t * (3.0f - 2.0f * t);
}
// How far it gets in dt, and its speed over the ground
static inline float sim_dist(float v, float dt) { return v * sim_scale(v) * dt; }
static inline float sim_v(float v) { return v * sim_scale(v); }

// The libc has cos and acos only
static inline float sin_f(float a) { return cosf(a - (float)M_PI_2); }
static inline float asin_f(float x) { return (float)M_PI_2 - acosf(clampf(x, -1, 1)); }
float atan2_f(float y, float x);

static inline float approach(float v, float target, float step) {
    if (v < target) return v + step > target ? target : v + step;
    return v - step < target ? target : v - step;
}

static inline uint32_t mix(uint32_t a, uint32_t b, int t) {   // t 0 .. 256 toward b
    int ar = a >> 16 & 255, ag = a >> 8 & 255, ab = a & 255;
    int br = b >> 16 & 255, bg = b >> 8 & 255, bb = b & 255;
    return GFX_RGB(ar + ((br - ar) * t >> 8), ag + ((bg - ag) * t >> 8), ab + ((bb - ab) * t >> 8));
}

static inline uint32_t shade(uint32_t c, int k) {             // k 256 = as is
    int r = (c >> 16 & 255) * k >> 8, g = (c >> 8 & 255) * k >> 8, b = (c & 255) * k >> 8;
    return GFX_RGB(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
}

static inline int fog_amount(float dist) {
    return (int)(clampf(dist / FOG_FAR, 0, 1) * 256);
}

//
// world.c
//

typedef struct {
    float       x, z;               // Middle of the runway
    vec3        along, across;      // Unit vectors down the runway and to its right
    int         heading;            // Degrees, 0 - 179
    bool        enemy;              // No fuel or repairs there
    const char* name;               // From a map (else NULL)
} base_t;

typedef struct { float a0, a1, c0, c1; uint32_t color; } base_rect_t;      // In a base's coordinates
typedef struct { float a, c, half_a, half_c, height; uint32_t color; } building_t;

typedef struct {
    float    x, z;                  // Center of the base
    float    half;                  // Half the base's side
    float    height;
    uint32_t color;
} hill_t;

typedef struct {
    int32_t     ci, cj;             // Its cell (or map index, cj MAP_SITE): names the site
    float       x, z;               // The radar
    bool        enemy;
    int         launchers;          // 4 - 6, in a ring (0: a radar on its own)
    float       angle;              // Where the first launcher stands (radians)
    const char* name;               // From a map (else NULL)
} sam_site_t;

#define MAP_SITE        INT32_MIN   // sam_site_t.cj of a map's site
#define SHIP_SITE       (INT32_MIN + 1) // missile_t.site_cj of a ship's SAM (site_ci: the ship)

extern const uint32_t    tile_colors[5];
extern const base_rect_t base_pavement[6];
extern const building_t  base_buildings[7];

uint32_t hash2(int32_t i, int32_t j, uint32_t seed);
int32_t  cell_of(float v, float size);

int32_t  base_cell_of(float v);
bool     base_in_cell(int32_t i, int32_t j, base_t* out);
bool     base_at(float x, float z, base_t* out);         // The base whose cell holds (x, z)
void     base_local(const base_t* b, float x, float z, float* along, float* across);
vec3     base_point(const base_t* b, float along, float across, float y);
bool     on_runway(float x, float z);
bool     on_pavement(float x, float z);

int      tile_type(int32_t i, int32_t j);               // 0: plain ground
bool     hill_in_cell(int32_t i, int32_t j, hill_t* out);
float    terrain_height(float x, float z);
bool     sam_in_cell(int32_t i, int32_t j, sam_site_t* out);

// What is within `range` of (x, z) (roughly: a few farther ones may come
// too), up to `max` of them; how many
int      bases_near(float x, float z, float range, base_t* out, int max);
int      sams_near(float x, float z, float range, sam_site_t* out, int max);
int      hills_near(float x, float z, float range, hill_t* out, int max);

// A launcher's base, its outward and sideways directions, and its rail
void     sam_launcher(const sam_site_t* s, int n, vec3* pos, vec3* out, vec3* side, vec3* rail);

//
// map.c: a theater from a text file (the format is described there)
//

enum { TARGET_FACTORY, TARGET_REFINERY, TARGET_DEPOT, TARGET_PORT, TARGET_POWER,
       TARGET_BUNKER, TARGET_BRIDGE, TARGET_TANKS, TARGET_TYPES };

typedef struct {
    float       x, z;
    vec3        ax, az;             // Unit vectors: its width and depth
    float       half_w, half_d, height;
    uint32_t    color;
    int         target;             // The target it is part of (-1: none)
    bool        round;              // A tank or silo: half_w is its radius
} map_building_t;

typedef struct {
    char        name[48];           // Given, or made from where it is ("Basra refinery")
    int         type;               // TARGET_*
    float       x, z, radius;       // Its pad
    vec3        ax, az;
    bool        enemy;
    bool        destroyed;          // This flight
} map_target_t;

typedef struct {
    float       ax, az, bx, bz, cx, cz;     // A triangle of ground color
    float       x0, z0, x1, z1;             // Its bounding box
    uint32_t    color;
} map_tri_t;

typedef struct {
    int         kind;               // SHIP_*
    char        name[40];
    float       x, z, heading;      // Degrees
    bool        enemy;
    int         escorts;            // A carrier's group: this many ships round it
} map_ship_t;

typedef struct {
    bool            loaded;
    uint32_t        serial;         // Different for each load
    char            name[48];
    char            problem[96];    // The first thing wrong with the file ("" if none)
    int             problems;
    float           x0, z0, x1, z1; // Bounds: nothing outside
    uint32_t        ground, outside;
    float           start_x, start_z, start_heading;
    bool            start_air;

    map_tri_t*      tris;           // Overlays, in the file's order
    int             tri_count;
    map_building_t* buildings;
    int             building_count;
    map_target_t*   targets;
    int             target_count;
    int             fighters[6];    // Enemy fighter types (air.c) its bases fly; none: any
    int             country_pct;    // Villages and farms: this many percent of the usual ...
    uint32_t        country_full[4];// ... but all of them on areas of these colors
    int             country_full_count;
    int             fighter_count;
    map_ship_t*     ships;          // Ships as the file puts them (ships.c sails them)
    int             ship_count;
    int             start_ship;     // Start on this ship's deck (-1: not)
} map_t;

extern map_t map;

bool        map_load(const char* path);
void        map_unload(void);                       // Back to the endless world
int         map_country_pct(float x, float z);      // How many villages and farms there (percent of usual)
void        map_random_start(uint32_t seed);        // Start at one of its friendly bases or carriers, picked by seed
bool        map_peek_name(const char* path, char* out, size_t size);
bool        map_inside(float x, float z);
const char* map_zone_at(float x, float z);          // Name of the zone (or city) there, or NULL
int         map_bases(float x, float z, float range, base_t* out, int max);
int         map_sams(float x, float z, float range, sam_site_t* out, int max);
int         map_hills(float x, float z, float range, hill_t* out, int max);
float       map_terrain_height(float x, float z);
bool        map_water_at(float x, float z);         // Sea, lake or river there
bool        map_city_near(float x, float z, float margin);  // In or this near one of its cities
uint32_t    map_target_pad_color(int type);

//
// country.c: villages and farms out in the country, made up cell by cell
//

#define COUNTRY_CELL    3000.0f     // At most one village or farm in each square this big

enum { COUNTRY_VILLAGE = 1, COUNTRY_FARM };

typedef struct {
    float       x, z;
    vec3        ax, az;             // Its own grid's directions
    int         kind;               // COUNTRY_*
    uint32_t    seed;
} country_site_t;

typedef struct {
    vec3        corner[4];
    uint32_t    color;
} country_field_t;

int  country_sites_near(float x, float z, float range, country_site_t* out, int max);
int  country_buildings(const country_site_t* s, map_building_t* out, int max);
int  country_fields(const country_site_t* s, country_field_t* out, int max);

//
// ships.c: carriers, warships, missile boats and tankers
//

enum { SHIP_CARRIER, SHIP_CRUISER, SHIP_DESTROYER, SHIP_FRIGATE, SHIP_MISSILE_BOAT, SHIP_TANKER, SHIP_KINDS };

#define MAX_SHIPS       64
#define DECK_ANGLE      0.157f      // A carrier's landing deck: this far (9 degrees) to port of its heading
#define LANDING_HALF_W  13.0f       // ... and half this wide
#define WIRE_FIRST      40.0f       // Its four wires: this far up it, 12 m apart
#define WIRE_LAST       76.0f

typedef struct {
    bool        active;
    int         kind;               // SHIP_*
    const char* name;
    bool        enemy;
    vec3        pos;                // At the waterline (y: down as it sinks)
    vec3        fwd, right;         // Level
    float       heading, speed;     // Radians (0 north, clockwise), m/s
    float       turn;               // rad/s now (+ right)
    int         leader;             // Keeps station on this ship (-1: sails on its own)
    float       station_a, station_c;   // Where: ahead and to the right of the leader
    float       home_x, home_z;     // Turns back when this far off its patrol
    bool        sunk;               // Hit: sinking (then gone)
    float       sink_time;
    float       shot_timer;
    bool        tracking;           // Its radar has you
    float       spread;             // Its escorts are out to this far
    int         avoid;              // Turning away from land this way (-1 left, 1 right; 0 not)
} ship_t;

extern ship_t ships[MAX_SHIPS];
extern int    ship_count;

int         ship_kind_of(const char* key);          // A map keyword ("carrier") to SHIP_*, or -1
const char* ship_label(int kind);                   // "destroyer"
float       ship_length(int kind);
float       ship_deck(int kind);                    // Deck height above the water
float       ship_beam(int kind);
void        ships_reset(void);                      // The map's ships where they start, none sunk
void        ships_update(float dt);
int         ship_flagship(int map_index);           // The ship a map line made (-1: none)
int         ship_deck_at(float x, float z, float* deck_y);    // A carrier's deck there: its index (-1: none)
bool        ship_solid_at(vec3 p, const char** why);         // Inside a hull, island or superstructure
bool        ship_wire(int s, float x, float z, vec3 dir, int* wire, float* along);  // The hook catches a wire there
                                                    // (along: how far up the landing deck)
void        ship_deck_point(int s, float a, float c, vec3* pos);      // A point on the deck, ship's own coords
void        ship_angled_point(int s, float along, float across, vec3* pos);  // ... the landing deck's own
bool        ship_tracking(int s);                   // Its radar still guides its missiles
void        ship_hit(int s);

//
// aircraft.c
//

typedef struct {
    vec3        pos;
    vec3        right, up, fwd;     // Orientation (orthonormal)
    float       speed;              // m/s along fwd
    int         throttle;           // 0 .. 100
    bool        afterburner;
    bool        brake;              // Speed brake, or wheel brakes on the ground
    bool        gear_down;
    float       g_load;
    bool        on_ground;
    vec3        ground_fwd;         // On the ground: heading (horizontal) ...
    vec3        ground_right;
    float       nose;               // ... and how far the nose is up (radians)
    bool        landed;             // Came down from a flight and is rolling out
    bool        crashed;
    const char* crash_reason;
    float       damage;             // 0 .. 1
    float       fuel;               // 0 .. 100
    bool        servicing;          // Being refueled or repaired now
    float       hit_time;           // world_time of the last hit (0: never)
    bool        ejected;            // Bailed out: under the parachute (eject.c)
    int         deck;               // On a ship's deck: its index + 1 (0: not)
    bool        arrested;           // Caught by a wire, slowing hard
    float       arrest_decel;
} plane_t;

typedef struct {
    float       cmd_x, cmd_y, cmd_z;    // Commanded stick (roll, pitch: + is pull) and rudder
    uint64_t    hold_x, hold_y, hold_z;
    float       stick_x, stick_y, rudder;
    bool        mouse;              // Flying with the mouse
    bool        locked;             // ... with the pointer locked (else: absolute)
    bool        no_lock;            // The dwm has no pointer lock
    bool        lock_try;           // Ask for the lock (again) at the next frame
    bool        have_last;
    int32_t     last_x, last_y;
    float       mouse_x, mouse_y;   // Mouse stick (pitch: + is pull)
    bool        invert;             // I: pitch the other way, keys and mouse
} controls_t;

// How a type of aircraft flies. Speeds in m/s (1 knot is 0.514 m/s),
// thrust in m/s^2, rates in rad/s at full stick
typedef struct {
    const char* name;
    const char* about;              // A line for the menu
    float       cruise;             // Level speed at full military power (sets the drag)
    float       thrust_mil;
    float       thrust_ab;          // 0: no afterburner
    float       thin_air;           // Drag halves every this many meters up (0: same everywhere)
    float       v_lift;             // Full lift (1 g) at and above this speed
    float       v_stall;            // STALL warning below this
    float       v_rotate;           // The nose comes up from here
    float       v_land_max;         // Faster touchdowns crash
    float       pitch_rate, roll_rate, yaw_rate;
    float       max_g;
    float       induced_drag;       // m/s^2 per (g^2 - 1): hard turns bleed speed
    float       fuel_minutes;       // A full load at full military power ...
    float       ab_burn;            // ... and the afterburner burns this many times as fast
    int         load;               // Carries this many times the usual few of each weapon
    int         chaff, flares;      // Countermeasures carried
    float       stealth;            // Enemy radars and pilots see it this much as far (1: plain)
} aircraft_t;

extern const aircraft_t aircraft_types[];
extern const int        aircraft_count;
extern const aircraft_t* craft;     // The one flying

extern plane_t    plane;
extern controls_t ctl;

void set_aircraft(int index);

void reset_controls(void);
void reset_flight(bool in_air);
void stick_press(float* cmd, uint64_t* hold, float dir, uint64_t now);
void update_stick(float dt, uint64_t now);
void update_flight(float dt);
void crash(const char* why);
void take_hit(void);                // A missile went off close by
int  throttle_limit(void);          // Highest throttle the engine still gives (0 - 100)
bool afterburner_ok(void);

//
// sam.c
//

// What a weapon is aimed at
enum { TGT_NONE, TGT_PLAYER, TGT_BANDIT, TGT_MAP, TGT_SAM, TGT_SHIP };

typedef struct {
    int      kind;                  // TGT_*
    int      index;                 // Bandit, map target or ship
    uint32_t id;                    // The bandit's (its slot may be reused)
    int32_t  ci, cj;                // SAM site
} target_t;

// Kinds of missile, each with its own motor and guidance (sam.c)
enum { MSL_SAM, MSL_ENEMY_AAM, MSL_AAM, MSL_AMRAAM, MSL_AGM, MSL_HARM, MSL_PAVEWAY, MSL_JDAM, MSL_NUKE, MSL_KINDS };

typedef struct {
    bool     active;                // Flying (else maybe a fading trail)
    int      kind;                  // MSL_*
    target_t target;
    vec3     pos, dir;
    vec3     aim;                   // Where its guidance points it (lags the target)
    float    speed, age;
    bool     guided;                // Its radar still sees the target
    int32_t  site_ci, site_cj;
    vec3     trail[TRAIL_LEN];      // Ring, oldest at trail_head when full
    int      trail_count, trail_head;
    float    trail_timer;
    float    fade;                  // Seconds since it blew up
    int      decoy;                 // Chasing this decoy + 1 instead (0: not fooled)
} missile_t;

// Chaff clouds and flares (sam.c)
enum { DECOY_CHAFF, DECOY_FLARE };
#define MAX_DECOYS      32

typedef struct {
    bool     active;
    int      kind;                  // DECOY_*
    vec3     pos, vel;
    float    age;
} decoy_t;

extern decoy_t decoys[MAX_DECOYS];

bool decoy_drop(int kind, vec3 pos, vec3 vel, bool by_player);
bool heat_seeker(int kind);         // A missile kind that flares fool (else chaff does)

typedef struct {
    bool     active;
    vec3     pos;
    float    age;
} blast_t;

enum { THREAT_NONE, THREAT_RADAR, THREAT_LOCK, THREAT_MISSILE };

extern missile_t missiles[MAX_MISSILES];
extern blast_t   blasts[MAX_BLASTS];
extern int       sam_threat;        // THREAT_*: the worst this moment
extern bool      sams_enabled;

void    sam_reset(void);
void    sam_update(float dt);
uint8_t sam_loaded(const sam_site_t* s);    // Launchers with a missile on (bit per launcher)
void    sam_set_enabled(bool on);       // Enemies (SAMs and fighters) on / off
missile_t* missile_new(int kind, vec3 pos, vec3 dir, float speed, target_t target);   // NULL if no room
int     enemy_missiles(void);           // Enemy missiles in the air
void    blast_at(vec3 p);

//
// air.c: enemy fighters
//

enum { BANDIT_CLIMB, BANDIT_PATROL, BANDIT_ATTACK, BANDIT_FALLING };

#define MAX_BANDITS     8

typedef struct {
    bool        active;
    uint32_t    id;
    const char* name;               // "MiG-29"
    int         type;
    int         state;              // BANDIT_*
    vec3        pos, fwd, up;
    float       speed, turn;        // turn: rad/s now (+ right), for the bank
    float       home_x, home_z;     // Orbits around here
    int         missiles;
    float       shot_timer, evade_timer, state_time;
    float       bank;               // Radians, right wing down (smoothed, for the picture)
    int         look;               // LOOK_*: how it is drawn
    float       size;               // An airliner's: 1 is a 747
    uint32_t    color;              // 0: a fighter's own gray
} bandit_t;

enum { LOOK_FIGHTER, LOOK_AIRLINER };

extern bandit_t bandits[MAX_BANDITS];

void air_reset(void);
int  fighter_type(const char* name);    // Index of a fighter type by name, or -1
void air_update(float dt);
bool bandit_attacking(void);        // Any fighter after you now
void bandit_hit(int index);         // A missile got it
float turn_toward(vec3* fwd, vec3 want, float max_turn);   // At most max_turn; radians turned right
void  bank_with_turn(bandit_t* b, vec3 old_fwd, float dt, float limit);   // Sets up from the heading change

//
// traffic.c: friendly fighters and airliners going about their business
//

#define MAX_TRAFFIC     6

typedef struct {
    bandit_t    p;                  // Where it is and how it looks (state: TRAFFIC_*)
    bool        has_dest;           // Off to a base, or (false) out of the area
    base_t      dest;
    vec3        land_dir;           // Down the runway it lands on
    float       cruise_alt, land_speed;
} traffic_t;

enum { TRAFFIC_ROLL, TRAFFIC_CLIMB, TRAFFIC_CRUISE, TRAFFIC_FINAL, TRAFFIC_ROLLOUT };

extern traffic_t traffic[MAX_TRAFFIC];

void traffic_reset(void);
void traffic_update(float dt);

//
// weapons.c: your missiles
//

// The weapons you can carry, two kinds at a time (picked in the menu)
enum { WPN_AIM9, WPN_AIM120, WPN_MAVERICK, WPN_HARM, WPN_PAVEWAY, WPN_JDAM, WPN_NUKE, WPN_TYPES };
#define LOAD_SLOTS      2

typedef struct {
    const char* name;               // "AIM-9 Sidewinder"
    const char* tag;                // "AIM-9", on the HUD
    const char* about;              // A line for the menu
    int         missile;            // MSL_*
    int         count;              // A fighter carries this many
    bool        air;                // Air-to-air (else air-to-ground)
    bool        bomb;               // Unpowered: glides, range from height and speed
    float       range, cone;        // Locks within this range and cos of this angle off the nose
    float       blast;              // Destroys everything this close where it hits (0: what it hit)
    const char* call;               // Said on firing
} weapon_type_t;

extern const weapon_type_t weapon_types[WPN_TYPES];
extern int loadout[LOAD_SLOTS];     // The menu's choice: WPN_* (-1: none)

typedef struct {
    int      armed;                 // Slot
    int      type[LOAD_SLOTS];      // WPN_* (-1: empty)
    int      left[LOAD_SLOTS];
    target_t lock;                  // Being locked or locked
    float    lock_time;             // Seconds on it
    bool     locked;                // Long enough: ready to fire
    int      chaff, flares;         // Countermeasures left
    char     message[64];           // What just happened, for the HUD ...
    float    message_time;          // ... and when (world_time)
} weapons_t;

int  weapon_capacity(int type);     // How many the aircraft carries
float weapon_range(int type);       // Now (a bomb's: from height and speed)
void weapons_area_hit(vec3 where, float radius);     // A big bomb: everything round it
bool player_missile(int kind);
void nuke_detonate(vec3 where);     // The tactical nuke goes off

// The last nuclear blast, for the renderer: fireball, then mushroom cloud
#define NUKE_LIFE       90.0f       // Seconds the cloud hangs there
typedef struct {
    bool     active;
    vec3     pos;
    float    age;                   // Seconds since it went off
} nuke_t;
extern nuke_t nuke;
extern weapons_t weapons;

void weapons_reset(void);           // A new flight: full load, nothing destroyed
void weapons_update(float dt);
void weapons_select(void);          // Arm the next kind
void weapons_next_target(void);
void weapons_fire(void);
void weapons_chaff(void);           // Chaff: decoys radar missiles (SAMs)
void weapons_flare(void);           // A flare: decoys heat seekers
float seen_range(float range);      // How far something that sees `range` sees the aircraft (stealth)
bool weapons_rearm(void);           // Full load again; false if it was full
bool target_state(const target_t* t, vec3* pos, vec3* vel);    // false if gone
void target_hit(const target_t* t, vec3 where);
void target_name(const target_t* t, char* out, size_t size);
bool sam_destroyed(int32_t ci, int32_t cj);
void weapons_say(const char* fmt, ...);

//
// render.c
//

extern wm_window_t    win;
extern gfx_surface_t* surf;
extern bool           quit;

extern uint32_t*      frame;            // sw x sh: what goes to the window
extern int32_t        sw, sh;
extern bool           chunky;
extern bool           night;            // Dark, with lights on the ground and stars
extern vec3           cam_pos, cam_r, cam_u, cam_f;
extern bool           paused, show_help, show_fps;
extern unsigned       fps;
extern float          world_time;       // Seconds of flying (stops while paused)

void render(uint64_t now);
void render_menu(uint64_t now);
void render_black(uint64_t now);    // The whole window black, nothing on it

//
// hud.c
//

void hud_init(void);
void radar_zoom(void);              // Next radar range
void nav_map_zoom(int dir);         // Map range: out (dir > 0) or in
void nav_map_toggle(void);          // Map on / off
void draw_hud(uint64_t now);
void draw_texts(bool light);        // The text queued this frame, onto the window (light: on a dark picture)

// Drawing into the frame, for the HUD and the menu
void text_at(int32_t x, int32_t y, int align, uint32_t color, const char* fmt, ...);
void draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
void fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color, int every);

//
// menu.c: picking the theater and the aircraft
//

void menu_init(const char* exe, const char* preselect);    // Finds the maps
void menu_draw(void);
bool menu_key(uint16_t key);        // True: start flying (the choice is made)
bool menu_click(int32_t x, int32_t y, int clicks);    // True: start flying
void menu_move(int32_t x, int32_t y);

//
// eject.c: bailing out
//

typedef struct {
    bool        active;
    bool        open;               // The parachute
    bool        down;               // On the ground
    vec3        pos, vel;
    float       heading;            // Radians, 0 north
    float       time;               // Since the seat fired
    const char* result;             // Once down: how it ended ...
    bool        safe;               // ... and whether well
} chute_t;

extern chute_t  chute;
extern bandit_t wreck;              // The jet you left, going in

void eject_reset(void);
bool eject(void);                   // Fire the seat (false if you can't now)
void eject_update(float dt);


//
// menu.c: the pieces pilot.c draws its screen with too
//

typedef struct { int32_t x, y, w, h; } rect_t;

void menu_panel(int32_t x, int32_t y, int32_t w, int32_t h);   // The light box the menus sit in
int  menu_list(int32_t x, int32_t y, int32_t w, int rows, const char* title, int count, int sel,
               int* first, bool active, const char* (*name)(int), rect_t* where);
bool menu_in_rect(const rect_t* r, int32_t x, int32_t y);

//
// pilot.c: the pilots, their ranks and score
//
// The first screen lists the pilots (and "New pilot"); the theater and
// aircraft menu comes after. What a flight destroys adds to its sortie score
// (SCORE_* points, taken away for anything friendly), which goes to the pilot
// only when the aircraft is down safe on the ground and Escape ends the
// flight. A crash, being shot down or ejecting loses it; a crash or being
// shot down (not ejecting) also kills the pilot, who stays listed but can't
// fly. The rank follows the pilot's score, the career total (the table in
// pilot.c); the best single flight is kept too, and one flight's score can
// earn a medal, each held once (the other table there). Pilots are kept in
// the registry, PILOT_KEY: value "pilots" (a line
// "name<TAB>score<TAB>rank<TAB>fate<TAB>best<TAB>medals" each, fate "KIA"
// for a dead one; older, shorter lines still read) and "last" (the one
// flown last).
//

#define PILOT_KEY       "hkey_user/software/flight"
#define MAX_PILOTS      16
#define PILOT_NAME_MAX  20          // Characters in a name

#define SCORE_FIGHTER   150         // An enemy fighter shot down
#define SCORE_SHIP      200         // A ship sunk
#define SCORE_SAM       100         // A SAM site
#define SCORE_RADAR     50          // A radar site (no launchers)
#define SCORE_TARGET    100         // A map's target

extern int  sortie_score;           // This flight's, not yet banked
extern bool pilot_screen;           // The pilot screen is up (else the theater and aircraft menu)

void        pilot_init(void);       // Reads the pilots
void        pilot_draw(void);
bool        pilot_key(uint16_t key);                        // True: a pilot is chosen
bool        pilot_click(int32_t x, int32_t y, int clicks);  // True: a pilot is chosen
void        pilot_move(int32_t x, int32_t y);
const char* pilot_title(void);      // "Capt. Name" of the one chosen ("" for none)
int         pilot_score(void);
void        sortie_start(void);     // A new flight: nothing scored yet
void        sortie_add(int points); // Something destroyed (less than 0: something friendly)
void        sortie_end(bool safe);  // Back to the pilots: bank it (safe) or lose it
void        pilot_killed(void);     // Crashed or shot down: the pilot is dead (kept at once)

#endif
