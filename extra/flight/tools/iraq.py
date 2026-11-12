# Writes maps/iraq.map: Desert Storm, 1991. All of Iraq, occupied Kuwait
# and the north of Saudi Arabia, at a bit over half real size (about 600
# by 700 km), from rough longitudes and latitudes. Mostly empty desert:
# villages and farms only along the rivers and in the north.
# Run: python3 tools/iraq.py
from maptool import Map

m = Map(44.5, 31.6, 1.8)
w, km = m.w, m.km

# Borders, from the Syria-Turkey-Iraq corner round clockwise
turkey_iraq = [(42.36,37.11),(42.8,37.33),(43.3,37.3),(43.8,37.22),(44.2,37.28),(44.6,37.18),(44.79,37.15)]
iran_iraq = [(44.79,37.15),(45.0,36.75),(45.3,36.45),(45.5,36.05),(45.9,35.85),(46.1,35.65),(46.15,35.25),(45.9,35.0),
             (45.65,34.6),(45.45,34.3),(45.75,33.9),(46.15,33.6),(46.5,33.15),(47.1,32.75),(47.4,32.45),(47.85,31.8),
             (47.7,31.4),(47.7,31.0),(48.0,30.98),(48.03,30.5),(48.4,30.1),(48.55,29.95)]
iraq_coast = [(48.55,29.95),(48.3,29.98),(48.18,29.88),(47.95,30.05)]
kuwait_iraq = [(47.95,30.05),(47.7,30.1),(47.15,30.0),(46.95,29.6),(46.55,29.1)]
saudi_iraq = [(46.55,29.1),(45.4,29.18),(44.72,29.2),(43.5,30.1),(42.08,31.1),(40.4,31.95),(39.2,32.15)]
jordan_iraq = [(39.2,32.15),(39.3,32.25),(38.8,33.38)]
syria_iraq = [(38.8,33.38),(40.95,34.42),(41.22,34.8),(41.25,35.5),(41.35,36.1),(41.8,36.6),(42.36,37.11)]
turkey_syria = [(42.36,37.11),(41.2,37.07),(40.0,36.82),(38.8,36.7)]
kuwait_saudi = [(48.42,28.53),(47.67,28.53),(47.43,29.0),(46.55,29.1)]
iraq = turkey_iraq + iran_iraq[1:] + iraq_coast[1:] + kuwait_iraq[1:] + saudi_iraq[1:] + jordan_iraq[1:] + syria_iraq[1:-1]

gulf = [  # The northern Gulf, round from the Shatt to past Dammam (clipped to the bounds)
 (48.55,29.95),(48.9,30.07),(49.2,30.08),(49.5,30.0),(49.75,30.15),(50.05,30.1),(50.2,29.9),(50.45,29.6),(50.65,29.25),
 (50.83,28.97),(50.88,28.75),(51.05,28.5),(51.25,28.15),(51.4,26.0),(50.6,25.0),(50.4,25.4),(50.22,25.8),(50.2,26.25),
 (50.15,26.47),(50.17,26.7),(49.9,26.86),(49.65,27.02),(49.42,27.2),(49.27,27.5),(48.9,27.66),(48.5,28.4),(48.4,28.62),
 (48.3,28.85),(48.15,29.05),(48.05,29.2),(47.97,29.38),(47.75,29.38),(47.9,29.55),(48.1,29.62),(48.18,29.85),(47.95,30.05),
 (48.3,29.98),
]
bubiyan = [(48.12,29.75),(48.33,29.93),(48.4,29.8),(48.22,29.62)]
failaka = [(48.28,29.45),(48.38,29.46),(48.38,29.41),(48.28,29.42)]
bahrain_tip = [(50.47,26.25),(50.63,26.26),(50.66,26.05),(50.45,26.05)]

euphrates = [(39.0,35.95),(39.6,35.7),(40.14,35.33),(40.5,34.9),(40.92,34.45),(41.4,34.4),(41.95,34.37),(42.38,34.13),
             (42.83,33.64),(43.3,33.42),(43.78,33.35),(44.0,33.05),(44.27,32.78),(44.42,32.48),(44.4,32.03),(44.75,31.75),
             (45.28,31.31),(45.8,31.15),(46.26,31.04),(46.47,30.89),(47.0,30.95),(47.43,31.0)]
tigris = [(42.36,37.11),(42.7,36.85),(42.85,36.62),(43.13,36.34),(43.25,35.8),(43.45,35.3),(43.49,34.93),(43.68,34.6),
          (43.88,34.2),(44.15,33.95),(44.3,33.6),(44.38,33.32),(44.6,33.1),(45.0,32.85),(45.4,32.6),(45.82,32.5),
          (46.68,32.46),(47.15,31.84),(47.3,31.4),(47.43,31.0)]
shatt = [(47.43,31.0),(47.6,30.75),(47.82,30.52),(48.05,30.38),(48.3,30.15),(48.55,29.95)]
great_zab = [(44.25,36.95),(43.9,36.5),(43.6,36.15),(43.35,35.95)]
little_zab = [(45.0,36.05),(44.5,35.8),(44.0,35.5),(43.5,35.25)]
diyala = [(45.7,35.1),(45.3,34.5),(44.95,34.0),(44.6,33.55),(44.5,33.25)]
karun = [(48.68,31.32),(48.5,30.9),(48.25,30.55),(48.18,30.43)]

w("# Desert Storm, January and February 1991: all of Iraq, occupied Kuwait")
w("# and the north of Saudi Arabia, at a bit over half real size (some")
w("# 600 km by 700). Mostly empty desert, villages and farms only along the")
w("# Tigris and Euphrates and in the Kurdish north; oil fields round Kirkuk,")
w("# Bayji, Basra and Kuwait. Iraq and occupied Kuwait are the enemy, with")
w("# Baghdad's SAM belt, Scud launchers in the western desert and the")
w("# Republican Guard along Kuwait's border. The coalition flies from the")
w("# Saudi bases and from carriers in the Gulf. maps/README.txt describes")
w("# every line.")
w("#")
w("# Run: flight maps/iraq.map    (written by tools/iraq.py)")
w()
w("map, Desert Storm")
w("units, km                   # Positions in km, x east and y north of As Samawah-ish")
m.bounds(38.8, 26.0, 50.3, 37.3)
w("color, gravel, 186 166 124")
w("color, steppe, 160 150 104")
w("color, kurd, 116 128 80")
w("color, gulf, 40 96 146")
w("color, shatt, 54 100 138")
w("color, sabkha, 222 214 190")
w("color, pipe, 120 112 96")
w("ground, desert")
w("outside, 170 152 112")
w("country, 4, farm, palms, kurd")
w("fighters, MiG-21, MiG-23, MiG-29, Su-22")
w()
w("#")
w("# The countries and their deserts, plains and mountains")
w("#")
w()
m.poly("zone, Iraq", "desert", iraq)
m.poly("zone, Kuwait", "sand", kuwait_iraq + [(47.43,29.0),(47.67,28.53),(48.42,28.53),(48.6,29.5),(48.3,30.0)])
m.poly("zone, Saudi Arabia", "sand", [(38.8,26.0),(50.3,26.0),(50.3,27.0),(49.4,27.2),(48.42,28.53)] + kuwait_saudi[1:] +
       saudi_iraq[1:] + [(38.8,32.0)])
m.poly("zone, Jordan", "gravel", [(38.8,33.38),(39.3,32.25),(39.2,32.15),(38.8,32.0)])
m.poly("zone, Syria", "steppe", syria_iraq + turkey_syria[1:])
m.poly("zone, Turkey", "kurd", turkey_iraq + [(44.79,37.3),(38.8,37.3)] + list(reversed(turkey_syria))[:-1])
m.poly("zone, Iran", "steppe", iran_iraq + [(48.9,30.07),(50.3,30.0),(50.3,37.3),(44.79,37.3)])
w()
m.poly("zone, Western Desert", "gravel", [(39.3,32.25),(40.4,31.95),(42.08,31.1),(43.0,31.4),(43.0,33.2),(41.8,34.0),
                                          (40.95,34.42),(38.8,33.38)])
m.poly("zone, Al Jazira", "steppe", [(41.25,35.5),(41.35,36.1),(41.8,36.6),(42.6,36.6),(43.0,35.9),(43.2,34.9),
                                     (42.4,34.4),(41.6,34.6)])
m.poly("zone, Kurdistan", "kurd", [(42.36,37.11)] + turkey_iraq[1:] + iran_iraq[1:9] + [(45.3,34.9),(44.4,35.6),(43.6,36.2),
                                   (43.0,36.5),(42.7,36.85)])
m.poly("zone, Southern Desert", "desert", [(42.08,31.1),(43.5,30.1),(44.72,29.2),(45.4,29.18),(46.55,29.1),(46.0,30.4),
                                          (45.0,30.9),(44.0,31.4),(43.0,31.4)])
m.poly("zone, Neutral Zone", "sand", [(44.72,29.2),(45.4,29.18),(45.1,28.95),(44.6,29.0)])
m.poly("zone, Nafud", "dune", [(39.2,28.0),(42.5,28.4),(43.2,27.9),(42.0,27.6),(39.8,27.2),(38.8,27.6)])
m.poly("zone, Dahna", "dune", [(44.3,28.0),(45.0,27.8),(46.5,26.0),(45.8,26.0),(44.8,27.0)])
m.poly("zone, Wadi al-Batin", "rock", [(46.55,29.1),(46.95,29.6),(47.02,29.55),(46.6,29.0),(46.0,28.3),(45.9,28.35),(46.45,29.05)])
w()
w("# The river plain: farms and palm groves between the Tigris and Euphrates")
m.poly("zone, Mesopotamia", "farm", [(43.2,33.6),(43.9,34.3),(44.3,34.0),(44.8,33.6),(45.3,33.2),(45.9,32.7),(46.8,32.6),
                                     (47.3,31.9),(47.4,31.1),(46.6,30.85),(46.1,30.95),(45.3,31.2),(44.7,31.6),(44.25,32.0),
                                     (44.0,32.6),(43.6,33.2)])
m.poly("zone, Euphrates valley", "palms", [(40.9,34.5),(41.95,34.45),(42.4,34.2),(42.9,33.68),(43.3,33.5),(43.3,33.35),
                                           (42.8,33.58),(42.33,34.06),(41.95,34.3),(40.95,34.38)])
m.poly("zone, Tigris valley", "farm", [(43.0,36.4),(43.2,36.4),(43.35,35.8),(43.6,34.95),(43.95,34.25),(43.8,34.15),
                                       (43.4,34.9),(43.15,35.8)])
m.poly("zone, Hammar Marshes", "marsh", [(46.35,30.92),(47.4,30.98),(47.3,30.75),(46.6,30.7)])
m.poly("zone, Hawizeh Marshes", "marsh", [(47.35,31.1),(47.55,31.1),(47.75,31.7),(47.5,31.9),(47.3,31.5)])
m.poly("zone, Basra", "palms", [(47.45,31.0),(47.65,30.98),(48.05,30.5),(48.4,30.12),(48.2,30.05),(47.75,30.35),(47.5,30.6)])
m.poly("zone, Khuzestan", "farm", [(48.0,30.98),(48.4,31.8),(49.2,31.6),(49.4,30.6),(48.9,30.1),(48.4,30.12),(48.03,30.5)])
m.poly("zone, Sabkha", "sabkha", [(47.0,30.2),(47.4,30.15),(47.5,30.05),(47.1,30.05)])
w()
w("#")
w("# Cities, then the sea, lakes and rivers over them")
w("#")
w()
cities = [
 ("Baghdad",44.38,33.32,6000),("Basra",47.8,30.5,3200),("Mosul",43.13,36.34,3600),("Kirkuk",44.39,35.47,2600),
 ("Erbil",44.01,36.19,2500),("Sulaymaniyah",45.43,35.56,2300),("Najaf",44.33,32.0,2100),("Karbala",44.03,32.62,2000),
 ("Hillah",44.42,32.48,1800),("Nasiriyah",46.26,31.05,1800),("Amarah",47.15,31.84,1600),("Kut",45.82,32.51,1500),
 ("Diwaniyah",44.93,31.99,1500),("Samawah",45.28,31.32,1300),("Ramadi",43.3,33.42,1800),("Fallujah",43.78,33.35,1600),
 ("Tikrit",43.68,34.6,1300),("Samarra",43.88,34.2,1300),("Baqubah",44.64,33.75,1500),("Duhok",42.99,36.87,1500),
 ("Zakho",42.69,37.14,1000),("Tal Afar",42.45,36.37,1200),("Sinjar",41.87,36.32,900),("Bayji",43.49,34.93,1100),
 ("Haditha",42.38,34.13,900),("Hit",42.83,33.64,900),("Anah",41.95,34.37,700),("Al Qaim",41.08,34.37,900),
 ("Rutba",40.29,33.03,800),("Nukhayb",42.27,32.04,400),("Khanaqin",45.38,34.35,900),("Kufa",44.4,32.03,1200),
 ("Az Zubayr",47.7,30.39,1300),("Umm Qasr",47.93,30.04,900),("Safwan",47.71,30.11,600),("Al Kufrah",45.3,30.0,300),
 ("Kuwait City",47.98,29.37,3000),("Al Jahra",47.66,29.34,1300),("Al Ahmadi",48.08,29.08,1500),("Al Wafrah",47.93,28.64,500),
 ("Hafar al-Batin",45.97,28.43,1600),("King Khalid Military City",45.53,27.9,1200),("Rafha",43.49,29.63,900),
 ("Ar'ar",41.04,30.98,1400),("Sakakah",40.2,29.97,1500),("Qaisumah",46.13,28.31,700),("Khafji",48.48,28.42,1000),
 ("Nairyah",48.47,27.47,700),("Jubail",49.62,27.0,2000),("Dammam",50.1,26.42,3000),("Dhahran",50.1,26.29,1800),
 ("Al Khobar",50.2,26.28,1800),("Ras Tanura",50.15,26.65,900),("Ha'il",41.7,27.52,2200),("Buraydah",43.97,26.33,2500),
 ("Deir ez-Zor",40.14,35.33,2000),("Abu Kamal",40.92,34.45,900),("Hasakah",40.75,36.5,1500),("Qamishli",41.22,37.05,1500),
 ("Raqqa",39.0,35.95,1800),("Abadan",48.3,30.34,2000),("Khorramshahr",48.18,30.43,1500),("Ahvaz",48.68,31.32,2500),
 ("Dezful",48.4,32.38,1500),("Ilam",46.42,33.64,1200),("Kermanshah",47.07,34.31,2800),("Mehran",46.17,33.12,600),
]
for n, lo, la, r in cities:
    m.at("city", n, lo, la, str(r))
w()
w("# The Gulf, and islands on it")
m.poly("water", "gulf", gulf)
m.poly("area", "sand", bubiyan)
m.poly("area", "sand", failaka)
m.poly("area", "sand", bahrain_tip)
w("# Lakes and reservoirs")
m.poly("water", "water", [(43.15,34.1),(43.45,34.15),(43.6,33.85),(43.45,33.6),(43.2,33.7)])         # Tharthar
m.poly("water", "water", [(43.4,33.3),(43.6,33.3),(43.6,33.15),(43.42,33.15)])                       # Habbaniyah
m.poly("water", "water", [(43.6,32.9),(44.0,32.85),(43.95,32.55),(43.65,32.6)])                      # Razzaza
m.poly("water", "water", [(42.15,34.15),(42.35,34.4),(42.5,34.3),(42.4,34.1),(42.25,34.05)])         # Qadisiyah
m.poly("water", "water", [(42.75,36.75),(42.95,36.75),(42.95,36.62),(42.8,36.62)])                   # Mosul Dam lake
m.poly("water", "water", [(44.9,36.0),(45.0,36.0),(45.0,35.9),(44.9,35.9)])                          # Dukan
m.poly("water", "water", [(38.9,35.95),(39.3,36.03),(39.55,35.98),(39.6,35.85),(39.3,35.88),(38.95,35.83)])   # Lake Assad
w("# Rivers")
m.poly("river, river", "500", euphrates)
m.poly("river, river", "450", tigris)
m.poly("river, shatt", "650", shatt)
m.poly("river, river", "250", great_zab)
m.poly("river, river", "200", little_zab)
m.poly("river, river", "200", diyala)
m.poly("river, shatt", "250", karun)
w()
w("# Roads, and the pipelines across the desert")
m.poly("path, road", "30", [(44.38,33.3),(44.42,32.48),(44.93,31.99),(45.28,31.32),(46.26,31.05),(47.0,30.7),(47.8,30.5)])     # Highway 8
m.poly("path, road", "30", [(47.8,30.5),(47.71,30.11),(47.68,29.6),(47.66,29.34),(47.98,29.37)])                       # Highway 80
m.poly("path, road", "30", [(44.38,33.32),(43.78,33.35),(43.3,33.42),(42.0,33.2),(40.29,33.03),(38.8,32.55)])           # To Amman
m.poly("path, road", "30", [(44.38,33.32),(44.15,33.95),(43.68,34.6),(43.49,34.93),(43.13,36.34),(42.69,37.14)])        # North to Turkey
m.poly("path, road", "30", [(44.38,33.32),(44.64,33.75),(44.39,35.47),(44.01,36.19),(43.13,36.34)])                     # Kirkuk and Erbil
m.poly("path, road", "25", [(45.97,28.43),(46.13,28.31),(47.0,28.2),(48.48,28.42),(48.5,27.9),(49.62,27.0),(50.1,26.42)])   # Hafar to Dhahran
m.poly("path, road", "25", [(45.97,28.43),(45.53,27.9),(44.5,27.0),(43.97,26.33)])
m.poly("path, pipe", "18", [(46.13,28.31),(44.8,29.0),(43.49,29.63),(42.3,30.3),(41.04,30.98),(39.8,31.5),(38.8,31.75)])   # Tapline
m.poly("path, pipe", "18", [(44.39,35.47),(43.5,35.6),(43.0,36.2),(42.5,36.7),(42.2,37.3)])                          # Kirkuk-Ceyhan
m.poly("path, pipe", "18", [(43.49,34.93),(44.3,33.6),(45.0,32.8),(45.9,31.8),(46.8,31.0),(47.3,30.5)])                 # Strategic pipeline
w()
w("#")
w("# Mountains: Kurdistan and the Zagros along Iran, Jebel Sinjar, the")
w("# Hamrin ridge, and Jebel Shammar in Saudi Arabia")
w("#")
w()
m.ridge((42.9,37.2),(44.6,37.15), 12, 4500, 1900)
m.ridge((43.4,36.85),(44.7,36.55), 10, 4500, 1600)
m.ridge((44.2,36.4),(45.2,35.95), 9, 4000, 1500)
m.ridge((44.9,35.7),(45.6,35.2), 7, 4000, 1300)
m.ridge((45.2,37.1),(45.9,36.0), 9, 5000, 2400)
m.ridge((46.0,35.8),(46.8,34.6), 10, 5000, 2300)
m.ridge((46.6,34.4),(47.6,33.4), 10, 5000, 2500)
m.ridge((47.4,33.3),(48.6,32.4), 11, 5000, 2600)
m.ridge((46.3,33.4),(47.2,32.95), 6, 4000, 1800)          # Kabir Kuh
m.ridge((41.55,36.38),(42.2,36.33), 6, 4000, 1100)        # Jebel Sinjar
m.ridge((44.05,35.15),(45.2,34.25), 12, 3000, 450)        # Hamrin
m.ridge((40.6,37.25),(42.2,37.28), 8, 4000, 1300)         # Turkey's hills
m.ridge((41.0,27.65),(42.4,27.35), 8, 4500, 1300)         # Jebel Shammar
m.mountain(43.4,35.12, 3000, 500)                          # Jebel Makhul
m.mountain(40.25,33.2, 3000, 350, "color rock")            # Rutba's hills
w()
w("#")
w("# The coalition")
w("#")
w()
w("side, friendly")
w()
for n, lo, la, h in [("King Khalid Military City",45.53,27.85,150),("Al Qaisumah",46.13,28.33,160),("Dhahran",50.15,26.27,160),
                     ("Al Jubail",49.4,27.04,170),("Ras al-Mishab",48.61,28.08,170),("Al Jouf",40.1,29.79,100),
                     ("Ar'ar",41.14,30.9,100),("Rafha",43.49,29.6,100),("Ha'il",41.69,27.44,180),("Gassim",43.77,26.3,150)]:
    m.at("airbase", n, lo, la, "heading %d" % h)
w()
for n, lo, la, l in [("Patriot Dhahran",50.05,26.35,6),("Patriot Jubail",49.5,27.05,5),("Patriot KKMC",45.6,27.95,5),
                     ("Patriot Hafar al-Batin",46.0,28.5,4),("Patriot Rafha",43.55,29.7,4)]:
    m.at("sam", n, lo, la, "launchers %d" % l)
for n, lo, la in [("Ar'ar",41.1,31.0),("Hafar al-Batin",45.9,28.4),("Ras al-Mishab",48.55,28.1)]:
    m.at("radar", n, lo, la)
w()
for n, lo, la in [("Ras Tanura tank farm",50.12,26.6),("Jubail tank farm",49.55,26.95),("Safaniya tank farm",48.62,28.0),
                  ("Khafji tank farm",48.45,28.38),("Dhahran tank farm",50.05,26.25),("Qaisumah pumping station",46.2,28.3)]:
    m.at("tanks", n, lo, la, "heading 30")
m.at("refinery", "Ras Tanura refinery", 50.08,26.66)
m.at("port", "Dammam port", 50.2,26.5, "heading 80")
m.at("port", "Jubail port", 49.68,27.05, "heading 60")
m.at("depot", "KKMC depot", 45.45,27.95)
m.at("depot", "Hafar al-Batin depot", 46.05,28.38)
w()
w("# At sea: carriers and battleships in the Gulf, tankers")
w("# Start on one with: start, USS Midway")
m.at("carrier", "USS Midway", 49.4,28.4, "heading 140")
m.at("carrier", "USS Ranger", 49.95,27.6, "heading 320")
m.at("carrier", "USS Theodore Roosevelt", 49.35,29.1, "heading 120, escorts 4")
m.at("cruiser", "USS Missouri", 48.8,28.95, "heading 200")
m.at("cruiser", "USS Wisconsin", 48.95,28.55, "heading 20")
for n, lo, la, h in [("Sea Isle City",49.7,28.8,150),("Gas King",50.1,27.2,330),("Ras Tanura Star",49.9,26.9,60)]:
    m.at("tanker", n, lo, la, "heading %d" % h)
w()
w("#")
w("# Iraq and occupied Kuwait")
w("#")
w()
w("side, enemy")
w()
for n, lo, la, h in [("H-3",40.07,32.93,120),("H-2",40.59,33.34,90),("Al Asad",42.44,33.79,90),("Al Taqaddum",43.6,33.34,130),
                     ("Saddam International",44.23,33.26,150),("Rashid",44.5,33.28,140),("Balad",44.36,33.94,130),
                     ("Al Sahra",43.55,34.67,100),("Kirkuk",44.35,35.47,140),("Mosul",43.15,36.3,150),
                     ("Qayyarah West",43.13,35.77,90),("Al Kut",45.75,32.48,120),("Tallil",46.09,30.94,120),
                     ("Jalibah",46.6,30.58,150),("Shaibah",47.66,30.42,130),("Ali Al Salem",47.52,29.35,150),
                     ("Ahmed Al Jaber",47.79,28.93,150),("Mudaysis",43.1,32.4,90)]:
    m.at("airbase", n, lo, la, "heading %d" % h)
w()
w("# Radars: early warning along the borders, and at the big bases")
for n, lo, la in [("Rutba",40.3,33.1),("Nukhayb",42.3,32.1),("H-3",40.1,32.98),("Al Qaim",41.1,34.3),("Basra",47.7,30.55),
                  ("Kuwait",47.95,29.4),("Mosul",43.2,36.25),("Kirkuk",44.4,35.4),("Samawah",45.3,31.4),("Baghdad",44.4,33.4)]:
    m.at("radar", n, lo, la)
w()
w("# SAMs: Baghdad's belt, the cities, the bases, Kuwait")
for n, lo, la, l in [("Baghdad North",44.38,33.45,6),("Baghdad South",44.4,33.18,6),("Baghdad East",44.55,33.32,6),
                     ("Baghdad West",44.22,33.33,6),("Baghdad NE",44.5,33.42,5),("Baghdad NW",44.27,33.43,5),
                     ("Baghdad SE",44.52,33.21,5),("Baghdad SW",44.26,33.2,5),("Taji",44.25,33.55,5),("Salman Pak",44.6,33.05,4),
                     ("Basra North",47.75,30.6,5),("Basra South",47.78,30.38,5),("Shaibah",47.6,30.45,4),
                     ("Mosul",43.1,36.25,5),("Kirkuk",44.3,35.45,5),("Kirkuk North",44.4,35.6,4),("Bayji",43.5,34.98,5),
                     ("Tikrit",43.7,34.65,5),("Samarra",43.9,34.15,4),("H-3",40.05,32.88,4),("H-2",40.6,33.3,4),
                     ("Al Qaim",41.1,34.35,4),("Al Asad",42.45,33.75,4),("Al Taqaddum",43.6,33.3,4),("Tallil",46.1,30.9,4),
                     ("Nasiriyah",46.25,31.1,4),("Kut",45.8,32.55,4),("Najaf",44.35,32.05,3),("Kuwait City",47.98,29.32,5),
                     ("Kuwait Bay",47.85,29.42,4),("Ahmed Al Jaber",47.8,28.9,4),("Ali Al Salem",47.5,29.3,4),
                     ("Republican Guard West",46.9,30.05,4),("Republican Guard East",47.3,30.25,4)]:
    m.at("sam", n, lo, la, "launchers %d" % l)
w()
w("# Baghdad")
m.at("bunker", "Presidential palace", 44.38,33.3)
m.at("bunker", "Ministry of Defense", 44.4,33.34)
m.at("bunker", "Al Firdos bunker", 44.35,33.29)
m.at("factory", "Baghdad telephone exchange", 44.41,33.32)
m.at("refinery", "Daura refinery", 44.41,33.25)
m.at("powerplant", "Daura power plant", 44.45,33.23)
m.at("tanks", "Daura tank farm", 44.38,33.22, "heading 30")
w("bridge, Jumhuriya bridge, %s, heading 90" % km(44.39,33.31))
w("bridge, 14th of July bridge, %s, heading 80" % km(44.4,33.29))
m.at("factory", "Al Taji military complex", 44.22,33.52)
m.at("depot", "Abu Ghraib depot", 44.1,33.29)
m.at("factory", "Tuwaitha nuclear site", 44.53,33.21)
m.at("bunker", "Salman Pak", 44.56,33.08)
m.at("factory", "Al Muthanna chemical works", 43.83,33.83)
w()
w("# The north: oil and power")
m.at("refinery", "Bayji refinery", 43.52,34.93)
m.at("tanks", "Bayji tank farm", 43.55,34.88, "heading 45")
m.at("powerplant", "Bayji power plant", 43.46,34.95)
m.at("refinery", "Kirkuk refinery", 44.3,35.4)
m.at("tanks", "Kirkuk oil field tank farm", 44.45,35.52, "heading 60")
m.at("tanks", "Bai Hassan tank farm", 44.1,35.58, "heading 20")
m.at("powerplant", "Kirkuk power plant", 44.42,35.42)
m.at("depot", "Mosul depot", 43.18,36.38)
w("bridge, Mosul bridge, %s, heading 60" % km(43.14,36.34))
m.at("depot", "Tikrit depot", 43.7,34.55)
m.at("factory", "Samarra chemical plant", 43.95,34.25)
w()
w("# The south: Basra's oil, the bridges on the way to Kuwait")
m.at("refinery", "Basra refinery", 47.65,30.45)
m.at("tanks", "Rumaila tank farm", 47.3,30.3, "heading 30")
m.at("tanks", "North Rumaila tank farm", 47.35,30.6, "heading 10")
m.at("tanks", "Zubair tank farm", 47.68,30.32, "heading 60")
m.at("port", "Umm Qasr port", 47.93,30.04, "heading 45")
m.at("port", "Basra port", 47.86,30.52, "heading 30")
m.at("powerplant", "Basra power plant", 47.75,30.55)
w("bridge, Nasiriyah bridge, %s, heading 0" % km(46.26,31.04))
w("bridge, Samawah bridge, %s, heading 0" % km(45.28,31.31))
w("bridge, Kut bridge, %s, heading 90" % km(45.82,32.5))
w("bridge, Qurna bridge, %s, heading 90" % km(47.43,31.0))
m.at("depot", "Nasiriyah depot", 46.2,31.1)
m.at("depot", "Kut depot", 45.85,32.45)
m.at("depot", "Najaf depot", 44.3,31.95)
m.at("depot", "Medina division", 47.2,30.35)
m.at("depot", "Tawakalna division", 46.9,30.0)
m.at("depot", "Hammurabi division", 47.45,30.7)
w()
w("# Occupied Kuwait")
m.at("refinery", "Mina al-Ahmadi refinery", 48.13,29.06)
m.at("refinery", "Mina Abdullah refinery", 48.15,28.98)
m.at("tanks", "Burgan oil field tank farm", 47.95,28.98, "heading 40")
m.at("tanks", "Ahmadi tank farm", 48.05,29.1, "heading 20")
m.at("port", "Shuwaikh port", 47.93,29.36, "heading 0")
m.at("bunker", "Kuwait City command post", 47.98,29.37)
m.at("depot", "Mutla Ridge depot", 47.62,29.42)
m.at("depot", "Al Jahra depot", 47.7,29.3)
w()
w("# Scud launchers hiding in the western desert")
for n, lo, la in [("H-2 Scud launchers",40.75,33.15),("H-3 Scud launchers",40.3,32.7),("Al Qaim Scud launchers",41.3,34.15),
                  ("Rutba Scud launchers",39.9,33.2),("Western Scud launchers",39.6,32.55),("Nukhayb Scud launchers",42.5,32.3)]:
    m.at("depot", n, lo, la)
w()
w("# Iraq's navy: Osa boats off Bubiyan and in Kuwait Bay")
m.at("missileboat", "Osa boats", 48.45,29.7, "heading 120, escorts 2")
m.at("missileboat", "Osa boat", 48.3,29.25, "heading 60")

m.save("iraq.map")
