# Writes maps/levant.map: Israel, Lebanon, Syria, Jordan and Iraq, from the
# Gulf of Aqaba up to Aleppo and Mosul and east to Baghdad, at 40% of real
# size, from rough longitudes and latitudes. Run: python3 tools/levant.py
from maptool import Map

m = Map(36.2, 32.2, 2.5)
w, km = m.w, m.km

coast = [  # The Mediterranean shore, south to north
 (34.0,31.15),(34.24,31.3),(34.45,31.52),(34.55,31.67),(34.64,31.8),(34.77,32.08),(34.85,32.33),(34.88,32.45),
 (34.95,32.72),(34.97,32.83),(35.07,32.85),(35.07,32.93),(35.1,33.1),(35.2,33.27),(35.37,33.56),(35.47,33.9),
 (35.6,34.05),(35.64,34.12),(35.75,34.3),(35.83,34.45),(35.97,34.65),(35.88,34.9),(35.9,35.05),
]
north_coast = [  # On north: Latakia, Hatay, the Gulf of Iskenderun, round to Adana
 (35.9,35.05),(35.93,35.25),(35.78,35.5),(35.83,35.85),(35.97,36.05),(35.88,36.4),(36.17,36.59),(36.2,36.85),
 (35.9,36.85),(35.6,36.75),(35.2,36.65),(34.8,36.75),(34.6,36.8),(33.9,36.8),
]
turkey_border = [  # Syria and Iraq's northern border, west to east
 (35.87,35.92),(36.15,35.95),(36.6,36.25),(36.65,36.83),(37.0,36.65),(38.0,36.85),(39.0,36.7),(40.0,36.85),
 (41.0,37.1),(42.36,37.11),(43.5,37.25),(44.78,37.15),
]
iraq_syria = [(38.79,33.38),(40.95,34.4),(41.2,34.8),(41.25,35.6),(41.4,36.1),(41.8,36.6),(42.36,37.11)]
iraq_saudi = [(39.3,32.23),(40.4,31.95),(41.4,31.4),(42.1,31.1),(44.7,29.1)]
golan = [(35.63,32.72),(35.78,32.72),(35.92,32.88),(35.86,33.15),(35.8,33.3),(35.65,33.25)]
lebanon_syria = [(35.97,34.65),(36.45,34.6),(36.35,34.5),(36.6,34.2),(36.35,33.85),(36.0,33.6),(35.8,33.3)]
israel_lebanon = [(35.65,33.25),(35.5,33.1),(35.1,33.1)]
syria_jordan = [(35.78,32.72),(35.65,32.68),(36.1,32.52),(36.84,32.31),(38.79,33.38)]
jordan_east = [(38.79,33.38),(39.3,32.23),(37.0,31.5),(38.0,30.5),(37.5,30.3),(36.5,29.5),(36.07,29.19)]
rift = [(35.0,29.53),(35.15,30.0),(35.35,30.6),(35.45,31.2),(35.5,31.5),(35.55,31.77),(35.55,32.1),(35.57,32.4),(35.6,32.68)]
west_bank = [(34.95,31.35),(35.45,31.35),(35.5,31.5),(35.55,31.77),(35.55,32.1),(35.55,32.5),(35.25,32.55),(35.02,32.4),(34.97,32.15),
             (35.05,31.85),(34.98,31.6)]

w("# The Levant, at 40% of real size: Israel, Lebanon, Syria, Jordan and Iraq,")
w("# from Eilat and Aqaba in the south up to Aleppo and Mosul, and east to")
w("# Baghdad. Syria and Iraq are the enemy, with Syria's SAMs in the Bekaa")
w("# Valley as in 1982; Israel, Jordan and Turkey are friendly. You start at")
w("# Ramat David. maps/README.txt describes every line.")
w("#")
w("# Run: flight maps/levant.map    (written by tools/levant.py)")
w()
w("map, Levant")
w("units, km                   # Positions in km, x east and y north of Mafraq-ish")
m.bounds(34.0, 29.4, 45.0, 37.15)
w()
w("color, med, 40 92 150")
w("color, deadsea, 60 110 130")
w("color, galilee, 46 104 140")
w("color, levant, 196 176 128")
w("color, negev, 204 172 118")
w("color, basalt, 120 106 92")
w("color, hills, 150 146 100")
w("color, cedar, 92 116 70")
w("color, orchards, 132 146 84")
w("color, jazira, 180 166 120")
w("color, mesopotamia, 140 150 90")
w("color, kurdistan, 120 128 86")
w("color, anatolia, 150 140 100")
w("color, euphrates, 56 104 130")
w("ground, levant")
w("outside, 168 152 116")
w("fighters, MiG-21, MiG-23, MiG-29, Su-22")
w()
w("#")
w("# The countries, then the land within them")
w("#")
w()
m.poly("zone, Sinai", "negev", [(34.0,29.4),(34.88,29.4),(34.9,29.5),(34.27,31.22),(34.0,31.15)])
m.poly("zone, Saudi Arabia", "desert", [(34.95,29.4),(36.07,29.19),(36.5,29.5),(37.5,30.3),(38.0,30.5),(37.0,31.5)] + iraq_saudi +
       [(45.1,29.1),(34.95,29.1)])
m.poly("zone, Turkey", "anatolia", turkey_border[::-1] +
       [(35.97,36.05),(35.88,36.4),(36.17,36.59),(36.2,36.85),(35.9,36.85),(35.6,36.75),(35.2,36.65),(34.8,36.75),(34.6,36.8),
        (33.9,36.8),(33.9,37.3),(45.1,37.3)])
m.poly("zone, Iraq", "jazira", iraq_syria + turkey_border[-2:] + [(45.1,37.0),(45.1,29.1)] + iraq_saudi[::-1] + [(38.79,33.38)])
m.poly("zone, Iran", "kurdistan", [(44.78,37.15),(45.0,36.7),(45.1,36.5),(45.1,37.3)])
m.poly("zone, Israel", "orchards", [(34.27,31.22),(34.9,29.5)] + rift + [(35.63,32.72)] + golan[::-1][:1] + israel_lebanon[1:] +
       [p for p in reversed(coast) if 31.3 < p[1] < 33.11][::-1][::-1])
m.poly("zone, Negev", "negev", [(34.27,31.22),(34.9,29.5),(35.0,29.53),(35.15,30.0),(35.35,30.6),(35.45,31.2),(34.9,31.35),(34.5,31.45)])
m.poly("zone, West Bank", "hills", west_bank)
m.poly("zone, Gaza", "levant", [(34.22,31.3),(34.27,31.22),(34.56,31.6),(34.5,31.62)])
m.poly("zone, Golan Heights", "basalt", golan)
m.poly("zone, Lebanon", "cedar", [(35.1,33.1),(35.5,33.1),(35.65,33.25),(35.8,33.3)] + lebanon_syria[::-1] + [p for p in coast if p[1] >= 33.1 and p[1] <= 34.65])
m.poly("zone, Bekaa Valley", "orchards", [(35.7,33.45),(35.9,33.5),(36.25,33.95),(36.45,34.3),(36.3,34.35),(35.95,33.95),(35.75,33.65)])
m.poly("zone, Syria", "levant", [(35.97,34.65),(35.88,34.9),(35.9,35.05),(35.93,35.25),(35.78,35.5)] + turkey_border[:6] +
       iraq_syria[::-1] + syria_jordan[::-1][1:] + golan[1:4] + lebanon_syria[::-1][:1] + lebanon_syria[::-1][1:])
m.poly("zone, Jordan", "desert", [(35.0,29.53),(36.07,29.19)] + jordan_east[::-1] + syria_jordan[::-1][:-1] + rift[::-1])
m.poly("zone, Jordan Valley", "orchards", [(35.55,31.77),(35.62,31.77),(35.65,32.1),(35.65,32.65),(35.6,32.68),(35.57,32.4),(35.55,32.1)])
m.poly("zone, Hauran", "orchards", [(35.95,32.75),(36.35,32.6),(36.6,33.0),(36.2,33.25),(35.95,33.05)])
m.poly("zone, Jebel Druze", "basalt", [(36.45,32.95),(36.85,32.85),(36.9,32.5),(36.55,32.45)])
m.poly("zone, Ghouta", "orchards", [(36.2,33.6),(36.55,33.6),(36.55,33.4),(36.2,33.42)])
m.poly("zone, Syrian Desert", "desert", [(37.0,34.2),(38.6,34.6),(40.0,34.5),(40.9,34.35),(38.79,33.38),(37.3,32.9),(37.2,33.6)])
m.poly("zone, Jazira", "jazira", [(38.6,35.6),(40.0,35.3),(41.25,35.6),(41.4,36.1),(41.8,36.6),(42.36,37.11),(41.0,37.1),(40.0,36.85),(39.0,36.7),(38.5,36.3)])
m.poly("zone, Orontes Valley", "orchards", [(36.2,35.1),(36.55,35.05),(36.8,35.6),(36.5,35.95),(36.25,35.6)])
m.poly("zone, Aleppo plateau", "orchards", [(36.7,35.8),(37.6,35.8),(37.7,36.5),(36.9,36.6)])
m.poly("zone, Western Desert", "desert", [(38.79,33.38),(40.95,34.4),(42.3,33.9),(43.0,33.0),(42.6,31.0),(41.4,31.4),(40.4,31.95),(39.3,32.23)])
m.poly("zone, Mesopotamia", "mesopotamia", [(43.2,34.4),(43.9,34.7),(44.7,34.3),(45.1,33.9),(45.1,32.3),(44.6,31.9),(43.9,32.3),(43.4,32.9),(43.0,33.5)])
m.poly("zone, Kurdistan", "kurdistan", [(42.36,37.11),(43.5,37.25),(44.78,37.15),(45.0,36.7),(45.1,36.5),(45.1,35.0),(44.3,35.6),(43.6,36.4),(42.8,36.7)])
m.poly("zone, Nineveh Plains", "mesopotamia", [(42.9,36.6),(43.6,36.5),(43.8,36.0),(43.3,35.7),(42.8,36.1)])
m.poly("zone, Harrat al Shaam", "basalt", [(36.9,32.3),(37.6,32.7),(37.9,32.3),(37.4,31.8),(36.9,31.9)])
m.poly("zone, Wadi Rum", "160 110 80", [(35.3,29.7),(35.6,29.7),(35.6,29.45),(35.3,29.45)])
m.poly("zone, Galilee", "orchards", [(35.1,33.1),(35.5,33.1),(35.6,32.85),(35.3,32.65),(35.08,32.85)])
w()
w("#")
w("# Cities, then the seas and rivers")
w("#")
w()
cities = [
 ("Tel Aviv",34.78,32.08,3200),("Jerusalem",35.21,31.78,2500),("Haifa",35.0,32.8,2200),("Beersheba",34.79,31.25,1600),
 ("Eilat",34.95,29.56,900),("Ashdod",34.65,31.8,1400),("Ashkelon",34.57,31.67,1200),("Netanya",34.86,32.32,1400),
 ("Tiberias",35.53,32.79,900),("Nazareth",35.3,32.7,1200),("Afula",35.29,32.61,900),("Dimona",35.03,31.07,800),
 ("Kiryat Shmona",35.57,33.21,700),("Hadera",34.92,32.44,1000),("Ramallah",35.2,31.9,1000),("Nablus",35.26,32.22,1200),
 ("Hebron",35.1,31.53,1200),("Jenin",35.3,32.46,900),("Gaza",34.46,31.5,1800),("Khan Yunis",34.3,31.34,1000),
 ("Beirut",35.5,33.89,3200),("Tripoli",35.84,34.43,1800),("Sidon",35.38,33.56,1200),("Tyre",35.2,33.27,1000),
 ("Zahle",35.9,33.85,1000),("Baalbek",36.2,34.0,1000),("Nabatieh",35.48,33.38,800),("Jounieh",35.63,33.98,1000),
 ("Damascus",36.29,33.51,3800),("Homs",36.72,34.73,2400),("Daraa",36.1,32.62,1200),("Quneitra",35.82,33.13,600),
 ("Palmyra",38.27,34.56,900),("Suwayda",36.57,32.71,1100),("Nabk",36.73,34.02,800),("Qusayr",36.58,34.51,700),
 ("Amman",35.93,31.95,3400),("Zarqa",36.09,32.07,1800),("Irbid",35.85,32.55,1600),("Karak",35.7,31.18,800),
 ("Ma'an",35.73,30.2,800),("Aqaba",35.0,29.53,1000),("Mafraq",36.2,32.34,900),("Salt",35.73,32.04,900),("Madaba",35.79,31.72,900),
 ("Tafilah",35.6,30.84,700),("Azraq",36.82,31.88,500),("Ruwaished",38.2,32.5,400),
]
cities += [  # North Syria, Hatay and Iraq
 ("Aleppo",37.16,36.2,3200),("Hama",36.75,35.13,1600),("Idlib",36.63,35.93,1000),("Latakia",35.79,35.53,1600),
 ("Tartus",35.89,34.89,1000),("Baniyas",35.95,35.18,700),("Raqqa",39.0,35.95,1300),("Deir ez-Zor",40.14,35.33,1300),
 ("Hasakah",40.75,36.5,1000),("Qamishli",41.23,37.03,900),("Abu Kamal",40.92,34.45,700),("Manbij",37.95,36.53,800),
 ("Antakya",36.16,36.2,1200),("Iskenderun",36.2,36.58,1000),("Gaziantep",37.38,37.06,1600),("Kilis",37.12,36.72,600),
 ("Mosul",43.13,36.34,3000),("Erbil",44.01,36.19,2400),("Kirkuk",44.39,35.47,2000),("Dohuk",43.0,36.87,1000),
 ("Tikrit",43.68,34.6,1000),("Samarra",43.87,34.2,1000),("Baghdad",44.37,33.31,4800),("Fallujah",43.78,33.35,1200),
 ("Ramadi",43.3,33.42,1300),("Baqubah",44.65,33.75,1100),("Karbala",44.02,32.62,1300),("Hillah",44.42,32.48,1300),
 ("Haditha",42.35,34.13,600),("Rutbah",40.28,33.04,500),("Anah",41.95,34.47,500),("Hit",42.83,33.64,600),
 ("Tal Afar",42.45,36.37,800),("Sinjar",41.87,36.32,600),("Baiji",43.49,34.93,700),("Al Qa'im",41.05,34.38,500),
 ("Najaf",44.33,32.0,1300),("Diwaniyah",44.93,31.99,900),
]
for n, lo, la, r in cities:
    m.at("city", n, lo, la, str(r))
w()
w("# The Mediterranean, the Dead Sea, the Sea of Galilee and the Gulf of Aqaba")
m.poly("water", "med", coast + north_coast[1:] + [(33.9,31.1)])
m.poly("water", "deadsea", [(35.45,31.77),(35.58,31.77),(35.6,31.5),(35.55,31.2),(35.42,31.2),(35.38,31.5)])
m.poly("water", "galilee", [(35.52,32.88),(35.63,32.86),(35.65,32.76),(35.6,32.7),(35.53,32.74)])
m.poly("water", "med", [(34.86,29.3),(34.9,29.5),(34.95,29.56),(35.0,29.53),(35.0,29.3)])
m.poly("water", "deadsea", [(36.55,34.68),(36.62,34.68),(36.62,34.62),(36.55,34.63)])          # Lake Qattinah
m.poly("water", "euphrates", [(38.0,35.9),(38.3,36.03),(38.55,35.98),(38.6,35.85),(38.3,35.88),(38.05,35.83)])   # Lake Assad
m.poly("water", "euphrates", [(42.15,34.15),(42.35,34.4),(42.5,34.3),(42.4,34.1),(42.25,34.05)])                  # Lake Qadisiyah
m.poly("water", "euphrates", [(43.15,34.1),(43.45,34.15),(43.6,33.85),(43.45,33.6),(43.2,33.7)])                  # Lake Tharthar
m.poly("water", "euphrates", [(43.4,33.3),(43.6,33.3),(43.6,33.15),(43.42,33.15)])                                # Lake Habbaniyah
m.poly("water", "euphrates", [(43.6,32.9),(44.0,32.85),(43.95,32.55),(43.65,32.6)])                               # Lake Razzaza
m.poly("water", "euphrates", [(42.75,36.75),(42.95,36.75),(42.95,36.62),(42.8,36.62)])                            # Mosul Dam lake
w("# Rivers")
m.poly("river, euphrates", "300", [(38.0,37.15),(38.0,36.82),(38.0,35.95),(38.6,35.9),(39.0,35.93),(39.6,35.7),(40.14,35.33),(40.45,35.02),
                                  (40.92,34.45),(41.05,34.38),(41.95,34.47),(42.3,34.2),(42.6,33.85),(42.83,33.64),(43.3,33.42),
                                  (43.78,33.35),(44.1,32.9),(44.3,32.6),(44.42,32.48),(44.9,32.0),(45.1,31.8)])          # Euphrates
m.poly("river, euphrates", "300", [(42.36,37.11),(42.75,36.75),(43.13,36.34),(43.25,35.9),(43.3,35.5),(43.48,34.95),(43.68,34.6),
                                  (43.87,34.2),(44.2,33.75),(44.37,33.31),(44.6,33.05),(45.1,32.85)])                  # Tigris
m.poly("river, euphrates", "150", [(40.75,37.05),(40.75,36.5),(40.6,35.8),(40.45,35.02)])                              # Khabur
m.poly("river, euphrates", "150", [(44.3,36.95),(43.8,36.3),(43.3,35.95)])                                            # Great Zab
m.poly("river, euphrates", "120", [(45.1,35.6),(44.6,35.4),(43.8,35.0),(43.55,34.9)])                                  # Little Zab
m.poly("river, euphrates", "150", [(45.1,34.4),(44.9,34.0),(44.6,33.6),(44.5,33.25)])                                  # Diyala
m.poly("river, galilee", "150", [(35.6,32.7),(35.57,32.4),(35.55,32.1),(35.55,31.77)])       # Jordan
m.poly("river, galilee", "100", [(35.62,33.22),(35.6,33.0),(35.6,32.88)])
m.poly("river, galilee", "100", [(36.0,32.72),(35.8,32.68),(35.6,32.65)])                  # Yarmouk
m.poly("river, galilee", "100", [(36.15,33.95),(35.85,33.6),(35.6,33.35),(35.25,33.33)])   # Litani
m.poly("river, galilee", "120", [(36.35,34.1),(36.5,34.4),(36.57,34.6),(36.6,34.68),(36.72,34.8),(36.7,34.95)])   # Orontes
m.poly("river, galilee", "80", [(36.0,33.65),(36.2,33.55),(36.35,33.5)])                    # Barada
w("# Roads")
m.poly("path, road", "25", [(34.95,29.56),(35.03,31.07),(34.79,31.25),(34.65,31.8),(34.78,32.08),(34.92,32.44),(35.0,32.8),(35.2,33.27),
                            (35.38,33.56),(35.5,33.89),(35.84,34.43),(36.0,34.7)])
m.poly("path, road", "25", [(34.78,32.08),(35.21,31.78),(35.75,31.95),(35.93,31.95)])
m.poly("path, road", "25", [(35.0,29.53),(35.73,30.2),(35.7,31.18),(35.79,31.72),(35.93,31.95),(36.09,32.07),(36.2,32.34),(36.1,32.62),
                            (36.29,33.51),(36.73,34.02),(36.72,34.73)])
m.poly("path, road", "25", [(35.5,33.89),(35.9,33.85),(36.29,33.51)])
m.poly("path, road", "20", [(36.72,34.73),(37.5,34.6),(38.27,34.56)])
m.poly("path, road", "20", [(36.2,32.34),(36.82,31.88),(37.6,32.2),(38.2,32.5),(38.8,32.6)])
m.poly("path, road", "20", [(35.82,33.13),(36.29,33.51)])
m.poly("path, road", "25", [(36.72,34.73),(36.75,35.13),(37.16,36.2),(37.38,37.06)])
m.poly("path, road", "20", [(35.79,35.53),(36.63,35.93),(37.16,36.2)])
m.poly("path, road", "20", [(37.16,36.2),(38.0,36.0),(39.0,35.95),(40.14,35.33),(40.92,34.45),(41.95,34.47),(42.83,33.64),
                            (43.3,33.42),(43.78,33.35),(44.37,33.31)])
m.poly("path, road", "20", [(38.8,32.6),(40.28,33.04),(41.5,33.2),(43.3,33.42)])
m.poly("path, road", "25", [(44.37,33.31),(43.87,34.2),(43.68,34.6),(43.49,34.93),(43.13,36.34),(43.0,36.87)])
m.poly("path, road", "20", [(43.13,36.34),(44.01,36.19),(44.39,35.47),(44.65,33.75),(44.37,33.31)])
m.poly("path, road", "20", [(44.37,33.31),(44.42,32.48),(44.33,32.0)])
m.poly("path, road", "20", [(40.14,35.33),(40.75,36.5),(41.23,37.03)])
m.poly("path, road", "20", [(43.13,36.34),(42.45,36.37),(41.87,36.32),(40.75,36.5)])
w()
w("#")
w("# Mountains: Mount Lebanon, the Anti-Lebanon and Hermon, the hills of")
w("# Galilee and Judea, the Jordanian highlands and the Jebel Druze")
w("#")
w()
m.ridge((35.95,34.35),(35.65,33.55), 10, 4500, 2000)               # Mount Lebanon
m.ridge((36.45,34.15),(35.95,33.45), 10, 4500, 1900)               # Anti-Lebanon
m.mountain(35.86,33.42, 7000, 2300, "color 220 220 220")           # Hermon
m.ridge((35.25,33.05),(35.45,32.95), 3, 3500, 950)                 # Galilee
m.mountain(35.39,32.69, 2500, 590)                                 # Tabor
m.ridge((35.0,32.72),(35.15,32.6), 3, 3000, 520)                   # Carmel
m.ridge((35.1,31.4),(35.25,32.3), 8, 3500, 950)                    # Judea and Samaria
m.ridge((35.65,30.2),(35.8,32.3), 14, 4000, 1100)                  # Jordanian highlands
m.ridge((35.45,30.3),(35.6,30.9), 5, 4000, 1500, "color 170 120 90")   # Edom
m.ridge((34.75,30.55),(35.05,30.75), 4, 3500, 800, "color negev")  # Ramon
m.ridge((34.82,29.55),(34.86,29.8), 3, 3500, 800, "color 170 120 90")   # Eilat mountains
m.ridge((36.72,32.85),(36.82,32.5), 4, 4500, 1700, "color basalt")  # Jebel Druze
m.ridge((36.8,34.2),(37.6,34.4), 6, 4000, 1100)                    # Palmyra ranges
m.ridge((37.0,33.9),(38.1,34.4), 7, 4000, 1000)
m.ridge((35.35,29.5),(35.55,29.65), 3, 3000, 1100, "color 160 110 80")  # Wadi Rum
m.ridge((36.08,35.25),(36.2,35.9), 6, 4000, 1200, "color cedar")   # Jebel Ansariya, behind Latakia
m.ridge((36.25,36.25),(36.45,36.75), 4, 4000, 1500, "color cedar")  # The Nur mountains
m.ridge((36.45,36.95),(36.95,37.05), 3, 4000, 1400, "color anatolia")
m.ridge((38.2,35.2),(39.3,34.95), 5, 4000, 800)                    # Jebel Bishri
m.ridge((41.55,36.38),(42.1,36.33), 4, 3500, 1300)                 # Jebel Sinjar
m.ridge((42.8,37.0),(44.0,36.85), 8, 4500, 1800, "color kurdistan")   # Kurdistan's mountains
m.ridge((44.3,36.9),(44.95,36.25), 6, 4500, 2200, "color kurdistan")
m.ridge((44.4,35.95),(44.95,35.3), 5, 4000, 1300, "color kurdistan")
m.ridge((43.55,34.95),(44.75,34.05), 7, 3000, 450)                 # Jebel Hamrin
m.mountain(43.35,36.6, 3500, 900, "color kurdistan")               # Jebel Maqloub
w()
w("#")
w("# Friendly")
w("#")
w()
w("side, friendly")
w("start, Ramat David")
w()
for n, lo, la, h in [("Ramat David",35.18,32.67,90),("Hatzor",34.73,31.76,110),("Tel Nof",34.82,31.84,150),("Nevatim",35.01,31.21,80),
                     ("Hatzerim",34.67,31.23,90),("Ramon",34.67,30.78,10),("Ovda",34.94,29.94,20),("Palmachim",34.72,31.9,0),
                     ("Muwaffaq Salti",36.78,31.83,130),("King Hussein",36.25,32.36,80),("Prince Hassan",37.15,32.16,140),
                     ("Marka",35.99,31.97,60),("King Faisal",36.15,30.35,90)]:
    m.at("airbase", n, lo, la, "heading %d" % h)
w()
m.at("radar", "Mount Meron", 35.42,32.99)
m.at("radar", "Mount Hermon", 35.8,33.35)
m.at("radar", "Ajloun", 35.75,32.33)
for n, lo, la in [("Tel Aviv",34.85,32.0),("Haifa",35.05,32.75),("Jerusalem",35.15,31.75),("Beersheba",34.85,31.3),
                  ("Ramat David",35.25,32.7),("Amman",35.95,31.9),("Mafraq",36.25,32.4)]:
    m.at("sam", n, lo, la, "launchers 4")
m.at("depot", "Haifa depot", 35.03,32.79)
for n, lo, la in [("Haifa tank farm",35.06,32.8),("Ashdod tank farm",34.68,31.79),("Ashkelon tank farm",34.56,31.64),
                  ("Eilat tank farm",34.97,29.6),("Zarqa refinery tanks",36.15,32.08),("Aqaba tank farm",35.03,29.43)]:
    m.at("tanks", n, lo, la, "heading 20")
w()
m.at("airbase", "Incirlik", 35.43,37.0, "heading 50")
m.at("tanks", "Incirlik fuel farm", 35.47,36.96, "heading 50")
m.at("sam", "Incirlik", 35.38,36.97, "launchers 4")
m.at("radar", "Iskenderun", 36.25,36.55)
w()
w("#")
w("# Enemy")
w("#")
w()
w()
w("# The Sixth Fleet: a carrier group off Lebanon. Start on it with:")
w("# start, USS Dwight D. Eisenhower")
m.at("carrier", "USS Dwight D. Eisenhower", 34.6,33.2, "heading 10")
m.at("destroyer", "USS New Jersey", 35.05,33.9, "heading 20")
w()
w("side, enemy")
w()
for n, lo, la, h in [("Mezzeh",36.22,33.48,110),("Dumayr",36.75,33.61,60),("Sayqal",37.2,33.68,80),("Khalkhalah",36.55,33.06,150),
                     ("Nasiriyah",36.86,33.92,30),("Al Qusayr",36.66,34.53,80),("Shayrat",36.9,34.49,110),("Tiyas",37.63,34.52,90),
                     ("Tha'lah",36.6,32.7,110),("Palmyra",38.32,34.55,90),("Rayak",36.0,33.85,40),("Marj Ruhayyil",36.46,33.29,30),
                     ("Damascus",36.52,33.41,50),("Bley",36.65,33.25,170)]:
    m.at("airbase", n, lo, la, "heading %d" % h)
w()
w("# Syria's Osa boats off Latakia and Tartus")
m.at("missileboat", "Osa boats", 35.55,35.5, "heading 200, escorts 2")
m.at("missileboat", "Osa boat", 35.6,34.85, "heading 20")
for n, lo, la in [("Jebel Barouk",35.75,33.68),("Qatana",36.08,33.45),("Kiswah",36.25,33.37),("Homs",36.7,34.65),
                  ("Tiyas",37.55,34.55),("Daraa",36.15,32.65),("Baalbek",36.25,34.05)]:
    m.at("radar", n, lo, la)
w()
for n, lo, la, l in [("Bekaa North",36.15,34.0,6),("Bekaa Middle",36.0,33.82,6),("Bekaa South",35.82,33.62,6),("Zahle",35.95,33.88,5),
                     ("Chtaura",35.88,33.8,5),("Baalbek",36.25,34.07,5),("Damascus West",36.15,33.5,6),("Damascus North",36.3,33.6,6),
                     ("Damascus South",36.3,33.4,6),("Damascus East",36.45,33.5,6),("Kiswah",36.23,33.35,5),("Sa'sa'",36.0,33.3,5),
                     ("Quneitra",35.95,33.15,5),("Khan Arnabah",35.9,33.2,4),("Daraa",36.1,32.68,5),("Suwayda",36.55,32.75,4),
                     ("Homs",36.75,34.7,6),("Qusayr",36.5,34.55,5),("Shayrat",36.95,34.45,5),("Tiyas",37.6,34.48,5),
                     ("Palmyra",38.25,34.5,4),("Dumayr",36.7,33.65,5),("Nabk",36.75,34.05,4)]:
    m.at("sam", n, lo, la, "launchers %d" % l)
w()
m.at("bunker", "Damascus command post", 36.27,33.52)
m.at("powerplant", "Damascus power plant", 36.35,33.48)
m.at("depot", "Kiswah depot", 36.24,33.36)
m.at("factory", "Adra factory", 36.5,33.6)
m.at("refinery", "Homs refinery", 36.68,34.71, "heading 60")
m.at("factory", "Homs fertilizer plant", 36.65,34.69)
m.at("powerplant", "Homs power plant", 36.76,34.75)
w("bridge, Orontes bridge, %s, heading 90" % km(36.6,34.66))
w("bridge, Litani bridge, %s, heading 0" % km(35.85,33.6))
m.at("depot", "Bekaa depot", 35.95,33.75)
m.at("depot", "Baalbek depot", 36.18,34.02)
m.at("bunker", "Bekaa command post", 36.05,33.9)
m.at("depot", "Quneitra depot", 35.85,33.1)
m.at("depot", "Daraa depot", 36.12,32.64)
m.at("bunker", "Katana command post", 36.07,33.44)
m.at("factory", "Dumayr factory", 36.72,33.64)
m.at("depot", "Palmyra depot", 38.28,34.53)
m.at("depot", "Tiyas depot", 37.6,34.55)
m.at("depot", "Nabk depot", 36.7,34.03)
w()
w("# North Syria")
for n, lo, la, h in [("Nayrab",37.22,36.18,90),("Kuweires",37.58,36.19,100),("Abu al-Duhur",37.1,35.73,90),("Hama",36.71,35.12,100),
                     ("Jirah",37.95,36.1,90),("Tabqa",38.57,35.75,90),("Deir ez-Zor",40.08,35.22,110),("Bassel al-Assad",35.95,35.4,170),
                     ("Qamishli",41.19,36.98,90)]:
    m.at("airbase", n, lo, la, "heading %d" % h)
for n, lo, la, l in [("Aleppo West",37.05,36.2,6),("Aleppo East",37.3,36.2,5),("Aleppo South",37.15,36.05,5),("Hama",36.8,35.15,5),
                     ("Latakia",35.85,35.5,5),("Tartus",35.92,34.95,4),("Tabqa",38.5,35.8,5),("Raqqa",39.05,35.98,4),
                     ("Deir ez-Zor",40.1,35.3,5),("Qamishli",41.25,37.0,4)]:
    m.at("sam", n, lo, la, "launchers %d" % l)
m.at("radar", "Aleppo", 37.2,36.3)
m.at("radar", "Deir ez-Zor", 40.2,35.38)
m.at("bunker", "Aleppo command post", 37.17,36.21)
m.at("powerplant", "Aleppo power plant", 37.4,36.15)
m.at("factory", "Hama factory", 36.78,35.1)
m.at("refinery", "Baniyas refinery", 35.97,35.22, "heading 20")
m.at("port", "Latakia port", 35.77,35.52, "heading 0")
m.at("port", "Tartus port", 35.87,34.91, "heading 10")
m.at("powerplant", "Tabqa dam power plant", 38.52,35.8)
w("bridge, Raqqa bridge, %s, heading 0" % km(39.0,35.93))
w("bridge, Deir ez-Zor bridge, %s, heading 30" % km(40.15,35.34))
m.at("depot", "Deir ez-Zor depot", 40.2,35.36)
m.at("depot", "Abu Kamal depot", 40.95,34.48)
for n, lo, la in [("Homs tank farm",36.66,34.68),("Baniyas tank farm",35.98,35.15),("Tartus tank farm",35.92,34.84),
                  ("Deir ez-Zor oil tanks",40.22,35.22),("Jbessa oil tanks",40.8,36.3),("Suwaydiyah oil tanks",41.6,36.8)]:
    m.at("tanks", n, lo, la, "heading 30")
w()
w("# Iraq")
for n, lo, la, h in [("Mosul",43.22,36.31,150),("Qayyarah West",43.13,35.77,130),("Kirkuk",44.35,35.47,120),("Erbil",43.96,36.24,150),
                     ("Al Asad",42.44,33.79,90),("H-2",40.47,33.33,90),("H-3",39.6,32.93,90),("Al Taqaddum",43.6,33.34,140),
                     ("Balad",44.36,33.94,120),("Saddam International",44.23,33.26,150),("Rasheed",44.43,33.43,130),
                     ("Tikrit",43.54,34.67,140),("Al Sahra",43.5,34.73,90)]:
    m.at("airbase", n, lo, la, "heading %d" % h)
for n, lo, la, l in [("Baghdad North",44.38,33.45,6),("Baghdad West",44.2,33.33,6),("Baghdad South",44.4,33.18,6),
                     ("Baghdad East",44.55,33.32,6),("Taji",44.25,33.53,5),("Osirak",44.55,33.2,6),("Mosul",43.1,36.38,5),
                     ("Mosul South",43.2,36.25,4),("Kirkuk",44.4,35.5,5),("Erbil",44.05,36.15,4),("Tikrit",43.65,34.6,5),
                     ("Baiji",43.45,34.9,4),("Al Asad",42.4,33.82,4),("H-3",39.65,32.95,5),("H-2",40.5,33.3,4),
                     ("Habbaniyah",43.55,33.4,4),("Samarra",43.93,34.28,4),("Karbala",44.05,32.65,4)]:
    m.at("sam", n, lo, la, "launchers %d" % l)
for n, lo, la in [("Baghdad",44.3,33.4),("Mosul",43.05,36.3),("Kirkuk",44.3,35.4),("Rutbah",40.3,33.1),("Haditha",42.3,34.05)]:
    m.at("radar", n, lo, la)
m.at("bunker", "Baghdad command post", 44.38,33.31)
m.at("bunker", "Tikrit command post", 43.68,34.62)
m.at("powerplant", "Daura power plant", 44.4,33.25)
m.at("refinery", "Daura refinery", 44.43,33.27, "heading 40")
m.at("powerplant", "Osirak reactor", 44.52,33.21)
m.at("refinery", "Baiji refinery", 43.55,34.92, "heading 30")
m.at("refinery", "Kirkuk refinery", 44.33,35.5, "heading 120")
m.at("powerplant", "Mosul dam power plant", 42.88,36.57)
m.at("powerplant", "Haditha dam power plant", 42.38,34.0)
m.at("factory", "Samarra chemical plant", 43.88,34.3)
m.at("factory", "Fallujah factory", 43.8,33.38)
m.at("factory", "Mosul factory", 43.18,36.32)
m.at("depot", "Taji depot", 44.25,33.56)
m.at("depot", "Mosul depot", 43.08,36.28)
m.at("depot", "Rutbah depot", 40.3,33.0)
m.at("depot", "Kirkuk depot", 44.42,35.42)
w("bridge, Baghdad bridge, %s, heading 120" % km(44.37,33.31))
w("bridge, Mosul bridge, %s, heading 110" % km(43.13,36.34))
w("bridge, Ramadi bridge, %s, heading 0" % km(43.3,33.42))
w("bridge, Fallujah bridge, %s, heading 10" % km(43.78,33.35))
w("bridge, Tikrit bridge, %s, heading 70" % km(43.68,34.6))
for n, lo, la in [("Daura tank farm",44.45,33.23),("Kirkuk tank farm",44.3,35.43),("Baiji tank farm",43.58,34.98),
                  ("K-3 pumping station",42.52,34.05),("T-1 pumping station",41.0,33.7),("H-3 tank farm",39.62,32.98),
                  ("Mosul tank farm",43.2,36.28),("Ramadi tank farm",43.28,33.38),("Hillah tank farm",44.75,32.45)]:
    m.at("tanks", n, lo, la, "heading 60")

m.save("levant.map")
