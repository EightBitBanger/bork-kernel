# Writes maps/gulf.map from rough longitudes / latitudes, squeezed to a
# quarter of real size (the whole Gulf in about 240 x 190 km)
import math
LON0, LAT0, SQUEEZE = 52.25, 27.5, 4.0
def km(lon, lat):
    x = (lon - LON0) * 111.32 * math.cos(math.radians(27.5)) / SQUEEZE
    y = (lat - LAT0) * 110.6 / SQUEEZE
    return "%.1f, %.1f" % (x, y)
def pts(lst, per=4):
    out = []
    for i in range(0, len(lst), per):
        out.append(",  ".join(km(*p) for p in lst[i:i+per]))
    return ",\n    ".join(out)
L = []
def w(s=""): L.append(s)

gulf = [  # Counterclockwise-ish, from the Shatt al-Arab mouth along Iran
 (48.55,29.95),(48.9,30.07),(49.2,30.08),(49.5,30.0),(49.75,30.15),(50.05,30.1),(50.2,29.9),(50.45,29.6),
 (50.65,29.25),(50.83,28.97),(50.88,28.75),(51.05,28.5),(51.25,28.15),(51.6,27.95),(51.95,27.82),(52.25,27.7),
 (52.45,27.58),(52.65,27.45),(53.0,27.12),(53.4,26.95),(53.75,26.75),(54.3,26.62),(54.85,26.53),(55.25,26.62),
 (55.6,26.93),(56.0,27.05),(56.27,27.16),(56.6,27.1),(56.85,26.95),(57.05,26.6),(57.3,26.15),(57.3,24.05),
 (56.75,24.35),(56.47,24.74),(56.36,25.05),(56.36,25.35),(56.27,25.62),(56.4,26.0),(56.42,26.38),(56.25,26.25),
 (56.1,26.0),(55.95,25.8),(55.55,25.57),(55.42,25.4),(55.27,25.27),(55.0,25.02),(54.6,24.62),(54.37,24.5),
 (54.0,24.22),(53.5,24.12),(53.0,24.17),(52.6,24.2),(52.0,23.9),(51.6,24.2),(51.35,24.62),(51.55,24.98),
 (51.58,25.3),(51.58,25.62),(51.55,25.92),(51.28,26.15),(51.05,25.97),(50.86,25.62),(50.8,25.2),(50.85,24.82),
 (50.75,24.7),(50.58,25.0),(50.4,25.4),(50.22,25.8),(50.2,26.25),(50.15,26.47),(50.17,26.7),(49.9,26.86),
 (49.65,27.02),(49.42,27.2),(49.27,27.5),(48.9,27.66),(48.5,28.4),(48.4,28.62),(48.3,28.85),(48.15,29.05),
 (48.05,29.2),(47.97,29.38),(47.75,29.38),(47.9,29.55),(48.1,29.62),(48.18,29.85),(47.95,30.05),(48.3,29.98),
]
bahrain = [(50.47,26.25),(50.63,26.26),(50.66,26.05),(50.61,25.8),(50.5,25.85),(50.45,26.05)]
bubiyan = [(48.12,29.75),(48.33,29.93),(48.4,29.8),(48.22,29.62)]
kharg = [(50.29,29.27),(50.34,29.28),(50.35,29.22),(50.3,29.2)]
qeshm = [(55.3,26.75),(55.9,26.98),(56.25,26.98),(56.0,26.85),(55.6,26.65),(55.3,26.55)]
failaka = [(48.28,29.45),(48.38,29.46),(48.38,29.41),(48.28,29.42)]
shatt = [(48.55,29.95),(48.45,30.05),(48.3,30.3),(48.2,30.43),(47.82,30.52),(47.6,30.75),(47.43,31.05)]
karun = [(48.2,30.43),(48.35,30.6),(48.55,30.85),(48.68,31.1)]
khor_musa = [(49.0,30.07),(49.05,30.25),(49.06,30.42)]

w("# The Persian Gulf, about a quarter of real size: some 240 km from")
w("# Kuwait to the Strait of Hormuz' far side, 190 km north to south.")
w("# Iran and southern Iraq are the enemy; Kuwait, Saudi Arabia, Bahrain,")
w("# Qatar and the Emirates are friendly. maps/README.txt describes every line.")
w("#")
w("# Run: flight maps/gulf.map")
w()
w("map, Persian Gulf")
w("units, km                   # Positions in km, x east and y north of the middle of the Gulf")
w("bounds, -122, -97, 122, 97")
w("ground, sand")
w("outside, 150 140 118        # Duller past the edges, so you can tell")
w()
w("color, gulf, 40 96 146")
w("color, shatt, 54 100 138")
w("color, sabkha, 222 214 190")
w("color, cityland, 176 166 146")
w()
w("#")
w("# Land: drawn first, so cities, the sea and rivers go over it")
w("#")
w()
w("# Salt flats and dunes")
w("zone, Sabkha Matti, sabkha,\n    " + pts([(52.0,23.9),(52.6,24.2),(53.0,24.17),(52.9,23.9)]))
w("zone, Rub al Khali, dune,\n    " + pts([(50.6,24.0),(50.75,24.3),(51.35,24.4),(51.6,24.2),(52.0,23.9)]))
w("zone, Dahna sands, dune,\n    " + pts([(47.3,27.3),(48.2,27.6),(48.6,26.8),(48.0,26.2),(47.3,26.3)]))
w()
w("# Marshes and farms of the south of Iraq and Khuzestan")
w("zone, Hawizeh Marshes, marsh,\n    " + pts([(47.35,30.75),(47.6,30.7),(47.75,30.95),(47.6,31.0),(47.35,31.0)]))
w("zone, Shatt al-Arab, palms,\n    " + pts([(48.5,29.95),(48.62,30.0),(48.35,30.38),(48.25,30.52),(47.85,30.62),(47.62,30.88),(47.5,30.8),(47.72,30.48),(48.12,30.38),(48.3,30.12)]))
w("zone, Khuzestan, farm,\n    " + pts([(48.6,30.6),(49.2,30.55),(49.9,30.65),(50.3,31.0),(48.6,31.0)]))
w()
w("#")
w("# Cities: a named zone with buildings (radius in meters). Before the sea,\n# so it covers their part over the water (and no buildings stand there)")
w("#")
w()
cities = [
 ("Kuwait City",47.96,29.36,2000),("Al Jahra",47.66,29.34,1200),("Al Ahmadi",48.08,29.08,1400),
 ("Basra",47.78,30.5,2600),("Az Zubayr",47.7,30.39,1200),("Umm Qasr",47.9,30.05,900),
 ("Abadan",48.33,30.33,1800),("Khorramshahr",48.14,30.45,1400),("Bandar Mahshahr",49.2,30.55,1300),
 ("Bushehr",50.88,28.92,1800),("Genaveh",50.53,29.6,1000),("Kangan",52.08,27.86,1000),
 ("Asaluyeh",52.62,27.5,1000),("Bandar Lengeh",54.9,26.58,1100),("Bandar Abbas",56.28,27.2,2800),
 ("Shiraz",52.55,29.62,3500),("Lar",54.3,27.7,1300),
 ("Dammam",50.1,26.42,2400),("Al Khobar",50.2,26.28,1800),("Jubail",49.62,27.0,1600),
 ("Ras Tanura",50.15,26.65,900),("Khafji",48.48,28.42,900),("Manama",50.58,26.21,1700),
 ("Doha",51.52,25.29,2800),("Abu Dhabi",54.4,24.45,2400),("Dubai",55.3,25.22,3200),
 ("Sharjah",55.42,25.33,1800),("Ras al Khaimah",55.95,25.78,1300),("Fujairah",56.33,25.12,1100),
 ("Khasab",56.25,26.18,700),("Sohar",56.72,24.36,1300),("Al Ain",55.75,24.22,2200),
]
for n, lo, la, r in cities:
    if n != "Manama":
        w("city, %s, %s, %d" % (n, km(lo, la), r))
w()
w("#")
w("# The sea, islands, rivers and roads")
w("#")
w()
w("# The Gulf and the Gulf of Oman")
w("water, gulf,\n    " + pts(gulf))
w()
w("# Shallows off the Emirates")
w("water, shallows,\n    " + pts([(52.6,24.2),(53.0,24.17),(53.5,24.12),(54.0,24.22),(54.37,24.5),(54.2,24.6),(53.4,24.35),(52.7,24.35)]))
w()
w("# Islands (drawn over the sea)")
w("area, sand,\n    " + pts(bahrain))
w("area, sand,\n    " + pts(bubiyan))
w("area, marsh,\n    " + pts([(48.2,29.7),(48.3,29.86),(48.36,29.8),(48.24,29.66)]))
w("area, sand,\n    " + pts(kharg))
w("area, rock,\n    " + pts(qeshm))
w("area, sand,\n    " + pts(failaka))
w("city, Manama, %s, 1700                    # On Bahrain: after the island" % km(50.58,26.21))
w()
w("# Rivers")
w("river, shatt, 350,\n    " + pts(shatt))
w("river, shatt, 220,\n    " + pts(karun))
w("river, gulf, 900,\n    " + pts(khor_musa))
w()
w("# Roads")
w("path, road, 25,\n    " + pts([(47.98,29.2),(48.15,28.7),(48.35,28.35),(49.1,27.45),(49.5,26.95),(50.1,26.43),(50.2,26.28),(50.1,25.9),(50.25,25.4),(50.45,25.0),(50.65,24.55),(51.2,24.45),(51.6,24.1),(52.6,24.05),(53.5,24.02),(54.4,24.35),(55.0,24.85),(55.3,25.15),(55.95,25.72)]))
w("path, road, 25,\n    " + pts([(48.17,30.44),(48.3,30.33),(48.6,30.5),(49.1,30.4),(49.53,30.8),(50.1,30.3),(50.95,28.95),(51.9,28.4),(52.55,29.62)]))
w()
w("#")
w("# Mountains: the Zagros along Iran, the Hajar in Oman")
w("#")
w()
w("ridge, %s, %s, 14, 4000, 1100" % (km(49.8,30.82), km(51.2,30.0)))
w("ridge, %s, %s, 12, 4500, 1500" % (km(50.6,30.82), km(51.8,30.2)))
w("ridge, %s, %s, 10, 4000, 1300" % (km(51.4,29.6), km(52.2,28.75)))
w("ridge, %s, %s, 14, 4200, 1400" % (km(51.65,28.55), km(53.3,27.75)))
w("ridge, %s, %s, 14, 4500, 1700" % (km(52.9,28.35), km(54.0,28.05)))
w("ridge, %s, %s, 18, 4000, 1300" % (km(53.4,27.55), km(55.6,27.35)))
w("ridge, %s, %s, 14, 4500, 1700" % (km(55.0,27.85), km(57.05,27.65)))
w("ridge, %s, %s, 9, 3500, 1200" % (km(56.05,25.65), km(56.25,26.3)))
w("ridge, %s, %s, 12, 4000, 1500" % (km(56.05,25.45), km(56.45,24.3)))
w("mountain, %s, 6000, 2000, color 160 150 140     # Jebel Hafeet-ish" % km(55.78,24.08))
w()
w("#")
w("# Friendly")
w("#")
w()
w("side, friendly")
w("start, Kuwait City")
w()
w("airbase, Kuwait City, %s, heading 150" % km(47.98,29.2))
w("airbase, Ali Al Salem, %s, heading 120" % km(47.52,29.35))
w("airbase, Ahmad al-Jaber, %s, heading 150" % km(47.79,28.93))
w("airbase, Jubail, %s, heading 160" % km(49.4,27.04))
w("airbase, Dhahran, %s, heading 160" % km(50.12,26.27))
w("airbase, Bahrain, %s, heading 0" % km(50.59,25.95))
w("airbase, Al Udeid, %s, heading 160" % km(51.3,25.12))
w("airbase, Al Dhafra, %s, heading 130" % km(54.55,24.25))
w("airbase, Al Minhad, %s, heading 90" % km(55.37,25.03))
w()
w("radar, Kuwait, %s" % km(47.85,29.1))
w("radar, Dhahran, %s" % km(50.0,26.35))
w("sam, Kuwait City, %s" % km(48.0,29.28))
w("sam, Dhahran, %s" % km(50.2,26.15))
w("sam, Bahrain, %s, launchers 4" % km(50.52,26.1))
w()
w("refinery, Ras Tanura refinery, %s, heading 20" % km(50.1,26.6))
w("port, Jubail port, %s, heading 45" % km(49.6,26.93))
w("refinery, , %s           # No name: named for where it is (Al Ahmadi refinery)" % km(48.1,29.06))
for n, lo, la in [("Mina al Ahmadi tank farm",48.1,29.12),("Shuaiba tank farm",48.08,29.0),("Ras al Khafji tank farm",48.42,28.4),
                  ("Juaymah tank farm",50.0,26.88),("Ras Tanura tank farm",50.1,26.68),("Abqaiq tank farm",49.68,25.93),
                  ("Dhahran tank farm",50.05,26.33),("Jubail tank farm",49.5,27.0),("Sitra tank farm",50.6,26.12),
                  ("Mesaieed tank farm",51.5,25.0),("Ras Laffan tank farm",51.5,25.85),("Ruwais tank farm",52.7,24.05),
                  ("Jebel Ali tank farm",55.05,24.95),("Fujairah tank farm",56.3,25.2),("Abu Dhabi tank farm",54.45,24.38),
                  ("Das Island tank farm",52.85,24.0),("Wafra tank farm",47.95,28.62)]:
    w("tanks, %s, %s, heading 30" % (n, km(lo, la)))
w()
w("#")
w("# Enemy")
w("#")
w()
w()
w("# Carrier groups, and tankers on the way out through Hormuz.")
w("# Start on a carrier with: start, USS Nimitz")
w("carrier, USS Nimitz, %s, heading 120" % km(51.3,26.9))
w("carrier, USS George Washington, %s, heading 290" % km(53.4,25.55))
for n, lo, la, h in [("Bridgeton",50.6,27.55,140),("Sea Isle City",51.9,26.5,120),("Gas Prince",53.0,26.35,100),
                     ("Ocean City",54.4,25.95,95),("Surf City",55.7,26.3,60),("Gas King",50.1,27.2,330),
                     ("Ras Tanura Star",50.4,26.85,60),("Kuwait Star",48.7,29.05,150)]:
    w("tanker, %s, %s, heading %d" % (n, km(lo, la), h))
w()
w("side, enemy")
w()
w("airbase, Basra, %s, heading 140" % km(47.66,30.55))
w("airbase, Omidiyeh, %s, heading 120" % km(49.53,30.83))
w("airbase, Bushehr, %s, heading 130" % km(51.0,28.95))
w("airbase, Shiraz, %s, heading 110" % km(52.6,29.48))
w("airbase, Lar, %s, heading 90" % km(54.35,27.62))
w("airbase, Bandar Abbas, %s, heading 90" % km(56.42,27.23))
w()
w("radar, Bandar Abbas, %s" % km(56.15,27.25))
w("radar, Kangan, %s" % km(52.15,27.9))
w("radar, Genaveh, %s" % km(50.6,29.65))
w("radar, Al Faw, %s" % km(48.45,30.0))
w()
w("sam, Basra North, %s" % km(47.8,30.62))
w("sam, Basra South, %s, launchers 4" % km(47.88,30.38))
w("sam, Abadan, %s" % km(48.4,30.38))
w("sam, Omidiyeh, %s" % km(49.65,30.75))
w("sam, Kharg Island, %s, launchers 4" % km(50.32,29.245))
w("sam, Bushehr, %s, launchers 6" % km(50.95,28.82))
w("sam, Asaluyeh, %s" % km(52.7,27.55))
w("sam, Shiraz, %s, launchers 6" % km(52.5,29.52))
w("sam, Bandar Lengeh, %s" % km(54.95,26.65))
w("sam, Qeshm, %s, launchers 4" % km(55.9,26.88))
w("sam, Bandar Abbas West, %s, launchers 6" % km(56.1,27.3))
w("sam, Bandar Abbas East, %s, launchers 6" % km(56.55,27.18))
w()
w("refinery, Abadan refinery, %s, heading 40" % km(48.36,30.3))
w("refinery, Basra refinery, %s, heading 140" % km(47.7,30.46))
w("bridge, Basra bridge, %s, heading 50" % km(47.82,30.52))
w("depot, , %s" % km(47.74,30.55))
w("port, Umm Qasr port, %s, heading 45" % km(47.93,30.04))
w()
w("# Iran's navy: frigates and a destroyer with missile boats round them,")
w("# Revolutionary Guard boats; Iraq's Osa boats off Bubiyan; Iran's tankers")
w("frigate, IRIS Sabalan, %s, heading 260, escorts 2" % km(54.2,26.25))
w("frigate, IRIS Sahand, %s, heading 80, escorts 2" % km(52.3,27.05))
w("destroyer, IRIS Damavand, %s, heading 300, escorts 3" % km(50.9,28.2))
for n, lo, la, h in [("Kaman boats",56.55,26.75,240),("Osa boats",48.6,29.6,120),("Boghammar boats",50.25,28.05,180),
                     ("Kaman boat",50.55,28.8,200),("Boghammar boat",54.6,26.2,90)]:
    w("missileboat, %s, %s, heading %d%s" % (n, km(lo, la), h, ", escorts 2" if n.endswith("s") else ""))
for n, lo, la, h in [("Iranian tanker",50.6,29.05,250),("Iranian tanker",54.0,26.25,270)]:
    w("tanker, %s, %s, heading %d" % (n, km(lo, la), h))
w("port, Bandar Imam Khomeini, %s, heading 90" % km(49.1,30.42))
w("factory, , %s, heading 30" % km(49.25,30.55))
w("bunker, , %s" % km(49.5,30.75))
w("port, Kharg oil terminal, %s, heading 80" % km(50.315,29.235))
w("powerplant, Bushehr power plant, %s, heading 10" % km(50.95,28.86))
w("depot, , %s" % km(50.54,29.59))
w("powerplant, Kangan gas plant, %s, heading 120" % km(52.1,27.9))
w("refinery, South Pars refinery, %s, heading 120" % km(52.6,27.52))
w("factory, , %s" % km(52.6,29.66))
w("bunker, Shiraz command post, %s" % km(52.45,29.58))
w("depot, , %s" % km(54.28,27.72))
w("depot, , %s" % km(54.92,26.6))
w("depot, Qeshm depot, %s" % km(55.95,26.9))
w("port, Bandar Abbas port, %s, heading 100" % km(56.2,27.19))
w("powerplant, , %s, heading 90" % km(56.35,27.25))
w("bunker, Bandar Abbas command post, %s" % km(56.3,27.3))
w("factory, , %s" % km(56.22,27.24))
for n, lo, la in [("Kharg Island tank farm",50.31,29.255),("Abadan tank farm",48.26,30.36),("Fao tank farm",48.45,30.0),
                  ("Zubair tank farm",47.72,30.38),("Rumaila tank farm",47.42,30.45),("Basra tank farm",47.85,30.48),
                  ("Mahshahr tank farm",49.18,30.55),("Ganaveh tank farm",50.5,29.6),("Bushehr tank farm",50.88,28.98),
                  ("Asaluyeh tank farm",52.62,27.5),("Kangan tank farm",52.05,27.86),("Bandar Abbas tank farm",56.28,27.2),
                  ("Bandar Lengeh tank farm",54.9,26.6),("Umm Qasr tank farm",47.9,30.08),("Khor al Amaya tank farm",48.6,29.9)]:
    w("tanks, %s, %s, heading 60" % (n, km(lo, la)))

open("/home/claude/flight/maps/gulf.map", "w").write("\n".join(L) + "\n")
