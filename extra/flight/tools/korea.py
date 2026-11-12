# Writes maps/korea.map: the Korean peninsula from Daejeon to the Yalu,
# at 40% of real size, from rough longitudes and latitudes.
# Run: python3 tools/korea.py
from maptool import Map

m = Map(127.0, 38.1, 2.5)
w, km = m.w, m.km

west = [  # The Yellow Sea coast, from the Yalu's mouth south
 (124.35,39.9),(124.75,39.62),(125.1,39.55),(125.4,39.5),(125.45,39.38),(125.2,39.2),(125.15,38.95),(125.35,38.75),
 (125.1,38.6),(124.9,38.45),(124.7,38.15),(124.85,37.92),(125.2,37.85),(125.5,37.75),(125.75,37.95),(126.1,37.72),
 (126.4,37.68),(126.55,37.72),(126.6,37.45),(126.65,37.2),(126.85,36.98),(126.55,36.95),(126.35,36.95),(126.15,36.75),
 (126.35,36.6),(126.5,36.35),(126.55,36.1),(126.7,35.95),(126.6,35.7),
]
east = [  # The East Sea coast, from the south up
 (129.45,35.7),(129.45,36.05),(129.38,36.4),(129.42,36.75),(129.4,37.05),(129.2,37.45),(128.95,37.75),(128.6,38.2),
 (128.35,38.6),(127.95,38.95),(127.55,39.15),(127.45,39.3),(127.55,39.55),(127.65,39.8),(128.0,39.95),(128.35,40.05),
 (128.65,40.25),(129.0,40.5),(129.4,40.75),
]
dmz = [(126.55,37.75),(126.75,37.92),(127.05,38.25),(127.5,38.3),(128.0,38.3),(128.25,38.5),(128.37,38.62)]

w("# Korea, at 40% of real size: Seoul and the bases south of it, the DMZ,")
w("# Pyongyang, Wonsan and MiG Alley up to the Yalu. North Korea is the")
w("# enemy; you start at Osan. maps/README.txt describes every line.")
w("#")
w("# Run: flight maps/korea.map    (written by tools/korea.py)")
w()
w("map, Korea")
w("units, km                   # Positions in km, x east and y north of the DMZ's middle")
m.bounds(124.3, 35.8, 129.7, 40.6)
w()
w("color, korea, 96 128 72")
w("color, paddy, 128 148 84")
w("color, yellowsea, 64 100 120")
w("color, eastsea, 34 74 128")
w("color, hills, 84 112 66")
w("color, mudflat, 150 140 110")
w("ground, korea")
w("outside, 92 110 80")
w("fighters, MiG-19, MiG-21, MiG-23, MiG-29")
w()
w("#")
w("# The countries and plains")
w("#")
w()
m.poly("zone, North Korea", "hills", [(124.3,39.9),(124.4,40.1),(124.9,40.45),(125.2,40.6),(129.7,40.6),(129.7,38.4),(128.37,38.62)] +
       list(reversed(dmz)) + [(126.3,37.7),(124.3,37.7)])
m.poly("zone, China", "88 112 70", [(124.3,39.88),(124.4,40.08),(124.6,40.25),(124.9,40.45),(125.2,40.6),(124.3,40.6)])
m.poly("zone, South Korea", "korea", [(126.55,37.75)] + dmz[1:] + [(129.7,38.4),(129.7,35.8),(126.0,35.8),(126.0,37.75)])
m.poly("zone, DMZ", "grass", [(126.6,37.72),(126.8,37.88),(127.1,38.22),(127.5,38.27),(128.0,38.27),(128.3,38.47),(128.4,38.6),
                              (128.33,38.65),(128.2,38.53),(128.0,38.33),(127.5,38.33),(127.0,38.28),(126.7,37.96),(126.5,37.77)])
m.poly("zone, Pyongyang plain", "paddy", [(125.3,39.3),(125.95,39.3),(126.1,38.9),(125.7,38.55),(125.3,38.7)])
m.poly("zone, MiG Alley", "hills", [(124.5,40.2),(125.4,40.5),(126.0,40.3),(125.8,39.7),(124.9,39.65)])
m.poly("zone, Han River plain", "paddy", [(126.6,37.75),(127.2,37.75),(127.3,37.3),(127.1,36.9),(126.75,37.0)])
m.poly("zone, Hamhung plain", "paddy", [(127.3,39.9),(127.8,39.95),(127.65,39.7),(127.4,39.65)])
m.poly("zone, Iron Triangle", "grass", [(127.05,38.3),(127.45,38.35),(127.3,38.6)])
w()
w("#")
w("# Cities, then the seas over their seaward part")
w("#")
w()
cities = [
 ("Seoul",126.98,37.56,3800),("Incheon",126.7,37.46,2200),("Suwon",127.01,37.27,1800),("Cheongju",127.49,36.64,1500),
 ("Daejeon",127.38,36.35,2200),("Chuncheon",127.73,37.88,1300),("Gangneung",128.9,37.75,1300),("Wonju",127.95,37.34,1300),
 ("Andong",128.73,36.57,1100),("Pohang",129.36,36.03,1500),("Cheonan",127.15,36.81,1300),("Uijeongbu",127.05,37.74,1200),
 ("Sokcho",128.59,38.2,900),("Chungju",127.93,36.99,1000),("Seosan",126.45,36.78,900),
 ("Pyongyang",125.75,39.03,3500),("Nampo",125.4,38.73,1800),("Kaesong",126.55,37.97,1500),("Haeju",125.71,38.04,1500),
 ("Sariwon",125.76,38.51,1400),("Wonsan",127.44,39.15,2000),("Hamhung",127.53,39.92,2300),("Hungnam",127.62,39.83,1300),
 ("Sinuiju",124.4,40.1,1800),("Dandong",124.38,40.14,1500),("Anju",125.67,39.62,1200),("Kanggye",126.6,40.97,1000),
 ("Pyongsong",125.84,39.25,1500),("Kimchaek",129.2,40.67,1000),("Tanchon",128.91,40.46,900),("Sinpo",128.18,40.04,800),
 ("Chongju",125.33,39.69,900),("Kusong",125.24,39.98,900),("Huichon",126.28,40.17,900),("Pyonggang",127.29,38.42,700),
]
for n, lo, la, r in cities:
    m.at("city", n, lo, la, str(r))
w()
w("# The Yellow Sea and the East Sea")
m.poly("water", "yellowsea", west + [(124.3,35.7),(124.3,39.9)])
m.poly("water", "eastsea", east + [(129.8,40.75),(129.8,35.7)])
w("# Mud flats off the west coast, and islands")
m.poly("area", "mudflat", [(126.4,37.68),(126.55,37.72),(126.6,37.45),(126.45,37.4),(126.35,37.55)])
m.poly("area", "korea", [(126.35,37.55),(126.5,37.52),(126.5,37.4),(126.38,37.42)])         # Yeongjong
m.poly("area", "hills", [(126.3,37.78),(126.52,37.78),(126.5,37.62),(126.35,37.6)])          # Ganghwa
m.poly("area", "hills", [(124.65,37.98),(124.75,37.98),(124.75,37.92),(124.65,37.92)])       # Baengnyeong
m.poly("area", "hills", [(125.95,36.65),(126.15,36.65),(126.1,36.55),(125.95,36.55)])
w()
w("# Rivers")
m.poly("river, 70 106 130", "450", [(126.55,37.72),(126.75,37.62),(126.98,37.53),(127.2,37.55),(127.4,37.5),(127.6,37.55),(127.95,37.35),(128.2,37.15)])
m.poly("river, 70 106 130", "250", [(126.68,37.76),(126.9,37.95),(127.05,38.05),(127.25,38.2),(127.45,38.45),(127.55,38.75)])     # Imjin
m.poly("river, 70 106 130", "350", [(125.38,38.72),(125.55,38.85),(125.72,39.0),(125.85,39.15),(126.05,39.3),(126.3,39.45)])     # Taedong
m.poly("river, 70 106 130", "300", [(125.45,39.55),(125.7,39.62),(126.0,39.68),(126.3,39.85),(126.5,40.1)])                     # Chongchon
m.poly("river, 70 106 130", "450", [(124.3,39.85),(124.4,40.05),(124.55,40.2),(124.85,40.4),(125.2,40.55),(125.5,40.7)])          # Yalu
m.poly("river, 70 106 130", "300", [(126.7,35.98),(127.0,36.05),(127.25,36.2),(127.45,36.45),(127.55,36.75)])                     # Geum
m.poly("river, 70 106 130", "250", [(127.65,39.85),(127.5,40.05),(127.4,40.4)])
w()
w("# Roads")
m.poly("path, road", "25", [(126.98,37.56),(127.01,37.27),(127.15,36.81),(127.38,36.35),(127.6,36.0)])
m.poly("path, road", "25", [(126.98,37.56),(126.7,37.46)])
m.poly("path, road", "25", [(126.98,37.56),(127.05,37.74),(126.55,37.97),(125.76,38.51),(125.75,39.03),(125.67,39.62),(125.33,39.69),(124.4,40.1)])
m.poly("path, road", "25", [(125.75,39.03),(126.5,39.1),(127.44,39.15),(127.53,39.92)])
m.poly("path, road", "20", [(126.98,37.56),(127.73,37.88),(128.59,38.2),(128.9,37.75)])
w()
w("#")
w("# Mountains: the Taebaek down the east, the Nangnim in the north, the")
w("# Sobaek across the south, and the hills along the DMZ")
w("#")
w()
m.ridge((128.3,38.7),(128.9,37.4), 14, 4500, 1500)
m.ridge((128.8,37.3),(129.15,36.3), 10, 4500, 1200)
m.ridge((127.6,36.3),(128.6,36.95), 10, 4500, 1300)
m.ridge((126.9,40.45),(127.25,39.35), 12, 5000, 1800)
m.ridge((127.6,40.5),(128.4,40.3), 8, 5000, 2000)
m.ridge((125.85,40.15),(126.5,39.85), 8, 4000, 1300)        # Myohyang
m.ridge((126.4,39.2),(127.1,38.6), 10, 4000, 1100)
m.ridge((127.5,38.95),(128.1,38.5), 8, 4000, 1300)
m.ridge((127.6,38.0),(128.1,37.5), 8, 4000, 1100)
m.ridge((126.1,38.55),(126.6,38.25), 6, 3500, 800)
m.ridge((127.1,37.75),(127.5,37.65), 5, 3000, 600)
m.mountain(128.47,38.12, 5000, 1700)                         # Seoraksan
m.mountain(128.1,38.65, 5000, 1500, "color rock")             # Kumgangsan
m.mountain(126.98,37.66, 2500, 800)                           # Bukhansan
w()
w("#")
w("# Friendly")
w("#")
w()
w("side, friendly")
w("start, Osan")
w()
for n, lo, la, h in [("Osan",127.03,37.09,90),("Kunsan",126.75,35.93,180),("Suwon",127.0,37.24,150),("Seoul",127.11,37.44,10),
                     ("Cheongju",127.5,36.72,60),("Gangneung",128.84,37.72,10),("Wonju",127.96,37.44,90),("Pohang",129.3,35.98,10),
                     ("Seosan",126.48,36.7,0),("Yecheon",128.35,36.63,10)]:
    m.at("airbase", n, lo, la, "heading %d" % h)
w()
m.at("radar", "Osan", 127.08,37.12)
m.at("radar", "Baengnyeong", 124.7,37.95)
m.at("radar", "Taebaek", 128.6,37.55)
for n, lo, la in [("Seoul North",127.0,37.68),("Seoul South",127.05,37.45),("Osan",127.0,37.05),("Incheon",126.65,37.5),
                  ("Chuncheon",127.75,37.83),("Gangneung",128.88,37.7)]:
    m.at("sam", n, lo, la, "launchers 4")
w()
w("#")
w("# Enemy")
w("#")
w()
for n, lo, la in [("Incheon tank farm",126.65,37.45),("Pyeongtaek tank farm",126.95,36.98),("Seoul fuel farm",126.85,37.53)]:
    m.at("tanks", n, lo, la, "heading 30")
w()
w("# Task Force 77 in the Sea of Japan. Start on it with: start, USS Ranger")
m.at("carrier", "USS Ranger", 129.35,37.6, "heading 10")
w()
w("side, enemy")
w()
w("# Missile boats off Wonsan and Nampo")
m.at("missileboat", "Osa boats", 127.75,39.25, "heading 160, escorts 2")
m.at("missileboat", "Komar boats", 124.95,38.6, "heading 200, escorts 1")
for n, lo, la, h in [("Sunan",125.67,39.22,10),("Mirim",125.86,39.01,140),("Pukchang",126.25,39.5,170),("Kaechon",125.9,39.75,90),
                     ("Wonsan",127.48,39.17,10),("Sondok",127.47,39.75,170),("Sinuiju",124.48,40.05,50),("Hwangju",125.88,38.65,150),
                     ("Taetan",125.25,38.13,90),("Onchon",125.25,38.9,10),("Toksan",127.75,39.98,60),("Uiju",124.55,40.18,40),
                     ("Panghyon",125.12,39.92,30),("Kuum-ni",127.05,38.85,50)]:
    m.at("airbase", n, lo, la, "heading %d" % h)
w()
for n, lo, la in [("Kaesong",126.5,38.05),("Haeju",125.65,38.1),("Wonsan",127.4,39.2),("Pyongyang",125.6,39.1),
                  ("Sinuiju",124.6,40.05),("Kosong",128.25,38.75),("Hamhung",127.45,39.95)]:
    m.at("radar", n, lo, la)
w()
for n, lo, la, l in [("Pyongyang North",125.75,39.12,6),("Pyongyang West",125.6,39.0,6),("Pyongyang South",125.78,38.92,6),
                     ("Pyongyang East",125.92,39.05,6),("Sunan",125.62,39.25,5),("Nampo",125.42,38.78,5),("Kaesong",126.52,38.0,5),
                     ("Pyonggang",127.3,38.45,5),("Haeju",125.75,38.08,4),("Sariwon",125.8,38.48,4),("Wonsan North",127.42,39.22,5),
                     ("Wonsan South",127.5,39.08,5),("Hamhung",127.55,39.88,5),("Anju",125.7,39.58,4),("Yongbyon",125.8,39.82,6),
                     ("Sinuiju",124.45,40.02,5),("Kusong",125.25,39.95,4),("Kosong",128.2,38.7,4),("Chorwon",127.2,38.35,4)]:
    m.at("sam", n, lo, la, "launchers %d" % l)
w()
m.at("powerplant", "Yongbyon reactor", 125.76,39.8)
m.at("factory", "Yongbyon reprocessing plant", 125.74,39.78)
m.at("bunker", "Pyongyang command post", 125.76,39.04)
m.at("powerplant", "Pyongyang power plant", 125.72,39.02)
m.at("factory", "Pyongyang tank works", 125.82,39.0)
m.at("port", "Nampo port", 125.38,38.72, "heading 60")
m.at("port", "Wonsan port", 127.45,39.17, "heading 90")
m.at("port", "Hungnam port", 127.64,39.82, "heading 120")
m.at("factory", "Hungnam chemical works", 127.6,39.85)
m.at("factory", "Hamhung factory", 127.5,39.93)
w("bridge, Yalu bridge, %s, heading 140" % km(124.4,40.12))
w("bridge, Chongchon bridge, %s, heading 0" % km(125.68,39.61))
w("bridge, Taedong bridge, %s, heading 140" % km(125.75,39.01))
m.at("depot", "Sinuiju rail yard", 124.42,40.08)
m.at("depot", "Kaesong depot", 126.58,38.0)
m.at("bunker", "Kaesong command post", 126.52,37.95)
m.at("depot", "Sariwon depot", 125.74,38.52)
m.at("port", "Haeju port", 125.7,38.02, "heading 0")
m.at("depot", "Pyonggang depot", 127.28,38.4)
m.at("refinery", "Sungri refinery", 124.45,40.03)
for n, lo, la in [("Nampo tank farm",125.42,38.75),("Sungri tank farm",124.5,40.0),("Wonsan tank farm",127.4,39.13),
                  ("Pyongyang tank farm",125.82,38.98),("Hungnam tank farm",127.6,39.85)]:
    m.at("tanks", n, lo, la, "heading 40")
m.at("depot", "Anju depot", 125.65,39.6)
m.at("bunker", "Kosong command post", 128.2,38.72)

m.save("korea.map")
