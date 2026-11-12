FLIGHT MAPS
===========

A map is a theater: the ground's colors, seas and rivers, cities, air
bases, SAM sites, radars, mountains, buildings and targets, all placed by
hand. Every .map file in maps/ (in the folder flight runs from, or next
to the program) shows up in the start menu's THEATER list, by the name on
its "map" line. To have one chosen already:

    flight maps/gulf.map
    flight maps/gulf            (".map" may be left off)

"Endless farmland" in the menu is the made-up world, without a map.

Inside the map's bounds is everything the file puts there. Past the bounds
is plain ground (the "outside" color) with nothing on it, to the horizon.
You can fly out there, but there's nothing to find, and the HUD says
OFF THE MAP.


THE FILE
--------

Plain text, one thing per line, fields split by commas. Spaces around
fields don't matter, and neither does upper or lower case in keywords.

    # A comment: everything after a # is ignored
    airbase, Kuwait City, -10000, -10000
    radar, Kuwait, -10000, -9000
    radar, Bandar Abbas, 12000, 8000

A line that ends with a comma goes on to the next line, so long lists of
points can be spread out:

    water, sea,
        -90, 60,   -80, 70,
        -70, 65,   -75, 55

Positions are x (east) then y (north), in meters, or in kilometers after
"units, km". Sizes (widths, heights, radii) are always meters. Headings
are degrees: 0 north, 90 east.

If something is wrong with a line, the line is skipped. Problems are
printed when flight starts, and the first one is shown on screen for the
first 20 seconds of flying, like this: "MAP: line 14: don't know "radr"".


COLORS
------

A color is a name or three numbers, red green blue, 0 to 255:

    ground, sand
    ground, 198 178 130

Names that are always there:

    sand desert dune rock salt grass farm palms marsh water sea shallows
    river city road concrete white grey red brown

Name your own (or change one) with:

    color, name, r g b              color, gulf, 40 96 146


THE MAP AS A WHOLE
------------------

    map, Name                       Shown in the window's title
    units, km                       Positions in km from here on (or: m)
    bounds, west, south, east, north
                                    The edges. Left out: everything in
                                    the file, plus 10 km all round
    ground, color                   The ground inside the bounds
    outside, color                  ... and past them (default: the same;
                                    a different color shows the edge)
    start, Airbase name             Where to start when the map has no
    start, Carrier name             friendly airbase or carrier: each
    start, x, y, heading, air       flight starts at one of those picked
                                    at random (on its runway, or at the
                                    back of a carrier's straight deck)
    side, friendly                  What follows is friendly (or: enemy),
                                    until the next "side" line
    country, 10, farm, palms        Villages and farms: 10% of the usual
                                    (deserts are mostly empty), but all of
                                    them on areas of the colors named.
                                    Left out: all of them everywhere
    fighters, MiG-17, MiG-21        The fighters its enemy bases send up,
                                    picked at random from the list. Known:
                                    MiG-17 MiG-19 MiG-21 MiG-23 MiG-29
                                    Su-22 Su-27. Left out: MiG-21, MiG-29
                                    and Su-27


THE GROUND
----------

Areas are drawn in the file's order, each over the ones before it, all
on top of the ground color. So: the sea first, then islands on it, and so
on. An area is a list of at least three points going round its edge,
either way round. It may be concave, but its edge shouldn't cross itself.

    water, color, x, y, x, y, x, y, ...
                                    Sea or lake: no city buildings on it
    area, color, x, y, x, y, x, y, ...
                                    Anything else: salt flats, farmland,
                                    an island (drawn after the sea)
    zone, Name, color, x, y, x, y, ...
                                    An area with a name. The HUD shows
                                    "Over Name" while you're above it, and
                                    unnamed targets in it are named for it
    river, color, width, x, y, x, y, ...
                                    A strip along the points, width in
                                    meters: water
    path, color, width, x, y, x, y, ...
                                    The same, not water: roads


THINGS ON THE MAP
-----------------

Options go after the fields a line needs, in any order:

    heading N       which way it faces (runways: the direction down them)
    enemy           this one is enemy (overrides "side")
    friendly        this one is friendly
    launchers N     SAM launchers, 0 to 8
    buildings N     buildings in a city
    color C         its color

Without "side" or an option, airbases are friendly and everything else is
enemy.

    airbase, Name, x, y             Runway (3 km), taxiways, hangars, tower.
                                    Options: heading, enemy / friendly.
                                    Friendly ones refuel and repair you
    sam, Name, x, y                 A radar with launchers round it (5
                                    unless "launchers N"). Enemy ones shoot
    radar, Name, x, y               A radar on its own: it sees you (RADAR
                                    on the HUD) but can't shoot
    mountain, x, y, width, height   A pyramid. Option: color
    ridge, x, y, x, y, count, width, height
                                    "count" mountains in a row from one
                                    point to the other, sizes varied a bit
    building, x, y, width, depth, height
                                    One box. Options: heading, color
    city, Name, x, y, radius        A round zone (color "city") with
                                    buildings scattered over it, taller in
                                    the middle, none on water. Put it
                                    before the sea and the sea covers the
                                    part of its zone that's offshore.
                                    Options: buildings N, color

Targets: a group of buildings on a pad, marked on the HUD within 10 km
(a square with the name and distance) and on the radar (a cross).

    factory, Name, x, y
    refinery, Name, x, y
    depot, Name, x, y
    port, Name, x, y
    powerplant, Name, x, y
    bunker, Name, x, y              (a command post)
    bridge, Name, x, y              Put it across a river, heading across
    tanks, Name, x, y               An oil tank farm: rows of round tanks

Options: heading, enemy / friendly. Leave the name empty and the target is
named for the zone it's in:

    refinery, , 48.1, 29.06         -> "Al Ahmadi refinery"

Ships: put them in water, well clear of the shore. Each sails on its own
heading, turns away from land and the map's edge, and turns back once
it's 15 km from where it started (a missile boat 6 km). Enemy cruisers,
destroyers and frigates shoot radar SAMs; any enemy ship can be sunk with
air-to-ground missiles. Without "side" or an option, tankers are
friendly and the other ships enemy.

    carrier, Name, x, y             333 m with a straight deck, an angled
                                    landing deck and four wires: trap on
                                    a friendly one to refuel and rearm.
                                    Comes with 6 escorts (a cruiser,
                                    destroyers, missile boats, a frigate)
                                    unless "escorts N" (0 to 8)
    cruiser, Name, x, y             173 m, SAMs out to 14 km
    destroyer, Name, x, y           155 m, SAMs out to 11 km
    frigate, Name, x, y             135 m, SAMs out to 8 km
    missileboat, Name, x, y         39 m
    tanker, Name, x, y              330 m, slow

Options: heading, enemy / friendly, escorts N (a carrier's group, as
above; on any other ship, that many missile boats round it, up to 6):

    carrier, USS Nimitz, 0, -20, heading 120, escorts 4
    frigate, IRIS Sabalan, 50, 30, heading 260, escorts 2, enemy

A runway, target or SAM site put on a hill or in water (a port or bridge
may be by water) is moved, when the map loads, to the nearest place up to
8 km away that's clear, so rough positions are fine.


THE MAPS
--------

Each flight starts at a random friendly airbase or carrier.

Each was written by a script in tools/ from rough longitudes and
latitudes, squeezed to fit in roughly 200 to 250 km a side. Edit the map
itself, or the script and run it again (python3 tools/korea.py, and so
on; they share tools/maptool.py).

    gulf.map      The Persian Gulf, a quarter of real size: Kuwait and
                  Basra in the northwest, the Strait of Hormuz in the
                  east. Iran and the south of Iraq are enemy; Kuwait,
                  Saudi Arabia, Bahrain, Qatar and the Emirates are
                  friendly. At sea: USS Nimitz and USS George
                  Washington with their groups, tankers heading for
                  Hormuz, Iran's frigates, destroyer and missile boats,
                  Iraq's Osa boats. (tools/gulf.py)

    vietnam.map   Vietnam, 1965-72, a third of real size: Hanoi,
                  Haiphong, Thud Ridge, the Dragon's Jaw and Route Package
                  1 down to the DMZ, then South Vietnam through the Central
                  Highlands, Saigon and the Mekong delta, with Laos,
                  Cambodia, eastern Thailand and the Gulf of Tonkin out
                  past Hainan. North Vietnam, the trail
                  and the sanctuaries are enemy; MiG-17s, 19s and 21s.
                  Friendly: the bases in Thailand and the South, from Da
                  Nang to Bien Hoa and Binh Thuy, and Task Force 77: Kitty
                  Hawk, Constellation and Enterprise at Yankee Station,
                  Midway at Dixie Station. Komar and Osa boats off
                  Haiphong and Vinh.

    korea.map     Korea, 40% of real size: Seoul and the bases to its
                  south, the DMZ, Pyongyang, Wonsan, Yongbyon and MiG
                  Alley up to the Yalu. North Korea is enemy, with missile
                  boats off Wonsan and Nampo; USS Ranger is in the Sea of
                  Japan.

    europe.map    Central Europe, 1985, half real size: the bases round
                  Frankfurt and up to Hannover, the Fulda Gap, East
                  Germany with West Berlin (friendly airfields inside the
                  Wall), and Czechoslovakia to Prague and Pilsen. The
                  Warsaw Pact is enemy.

    iraq.map      Desert Storm, 1991, a bit over half real size (about
                  600 by 700 km): all of Iraq, occupied Kuwait and the
                  north of Saudi Arabia. Mostly empty desert, villages and
                  farms only along the Tigris and Euphrates and in the
                  Kurdish north. Oil round Kirkuk, Bayji, Basra and Kuwait;
                  Baghdad's SAM belt, Scud launchers out west by H-2 and
                  H-3, the Republican Guard along Kuwait's border.
                  Coalition: the Saudi bases from Al Jouf to Dhahran, and
                  Midway, Ranger and Theodore Roosevelt in the Gulf with
                  the battleships Missouri and Wisconsin. (tools/iraq.py)

    levant.map    Israel, Lebanon, Syria, Jordan and Iraq, 40% of real
                  size, from Eilat and Aqaba up to Aleppo and Mosul and
                  east to Baghdad. Syria and Iraq are enemy, with Syria's
                  SAMs packed into the Bekaa Valley as in 1982 and rings
                  round Baghdad; Israel, Jordan and Turkey (Incirlik) are
                  friendly, with USS Dwight D. Eisenhower off Lebanon and
                  Syria's Osa boats off Latakia.
