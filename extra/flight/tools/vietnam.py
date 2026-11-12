# Writes maps/vietnam.map: North and South Vietnam in the Rolling Thunder
# and Linebacker years, with Laos, Cambodia and eastern Thailand, about a
# third of real size, from rough longitudes and latitudes.
# Run: python3 tools/vietnam.py
from maptool import Map

m = Map(105.4, 19.4, 3.4)
w, km = m.w, m.km

coast = [  # From the Chinese border down to Da Nang, and on round the Mekong delta to Ca Mau
 (108.0,21.55),(107.6,21.3),(107.35,21.05),(107.1,20.95),(106.8,20.8),(106.7,20.62),(106.6,20.32),(106.35,20.1),
 (106.1,19.95),(105.92,19.72),(105.82,19.35),(105.75,19.0),(105.72,18.75),(105.92,18.42),(106.25,18.15),(106.45,17.85),
 (106.6,17.48),(106.85,17.2),(107.1,17.02),(107.35,16.8),(107.65,16.52),(107.95,16.35),(108.2,16.1),(108.3,15.6),
 (108.65,15.4),(108.88,15.1),(109.05,14.5),(109.2,13.9),(109.25,13.6),(109.32,13.1),(109.22,12.6),(109.25,12.25),
 (109.2,11.9),(109.05,11.6),(108.7,11.25),(108.3,11.0),(108.0,10.85),(107.6,10.6),(107.25,10.4),(107.05,10.35),
 (106.8,10.38),(106.72,10.2),(106.6,9.9),(106.3,9.6),(105.9,9.3),(105.5,8.95),(105.1,8.7),(104.75,8.58),
]
west_coast = [  # Ca Mau up the Gulf of Thailand to Ha Tien, Cambodia and Trat
 (104.75,8.58),(104.8,9.0),(104.85,9.6),(105.0,10.0),(104.8,10.25),(104.48,10.4),(104.18,10.58),(103.75,10.5),
 (103.5,10.62),(103.25,10.9),(103.05,11.3),(102.9,11.65),(102.65,12.0),(102.45,12.2),(102.25,12.35),
]
cambodia_vn = [(107.55,14.7),(107.5,13.9),(107.6,13.4),(107.45,12.4),(106.8,12.05),(106.4,11.7),(106.1,11.05),
               (105.85,11.0),(105.2,10.9),(104.95,10.55),(104.48,10.4)]
laos_south = [(107.2,16.15),(107.45,15.6),(107.6,15.3),(107.55,14.7),(106.9,14.35),(106.5,14.5),(106.0,13.95),(105.2,14.35),
              (105.5,15.1),(105.55,15.6)]
thai_cambodia = [(105.2,14.35),(104.5,14.4),(103.5,14.35),(102.9,14.2),(102.55,13.6),(102.35,13.0),(102.45,12.2)]
laos = [
 (102.1,22.45),(102.6,21.8),(103.0,21.05),(103.25,20.85),(104.1,20.95),(104.6,20.65),(104.4,20.3),(104.0,19.6),
 (104.8,19.2),(105.2,18.75),(105.65,18.25),(106.15,17.65),(106.55,16.95)] + laos_south + [
 (105.4,15.85),(105.0,15.95),(104.75,16.55),(104.78,17.4),(104.25,17.95),(103.65,18.38),(103.3,18.42),(102.6,17.97),
 (102.1,17.9),
]
thailand = [(102.1,17.9),(102.6,17.97),(103.3,18.42),(103.65,18.38),(104.25,17.95),(104.78,17.4),(104.75,16.55),
            (105.0,15.95),(105.4,15.85),(105.55,15.6),(105.5,15.1)] + thai_cambodia + [(102.25,12.35),(102.1,12.35)]
cambodia = cambodia_vn[:1] + laos_south[3:8][::-1][:0] + [(106.9,14.35),(106.5,14.5),(106.0,13.95),(105.2,14.35)] + \
           thai_cambodia[1:] + west_coast[::-1][1:10] + cambodia_vn[::-1][1:]
china = [(102.1,22.45),(102.5,22.75),(103.1,22.55),(103.65,22.8),(104.0,22.52),(104.8,22.85),(105.35,23.25),(106.2,22.95),
         (106.7,22.85),(106.65,22.35),(107.0,22.0),(107.35,21.65),(108.0,21.55),(108.5,21.65),(108.9,21.6),(109.3,21.5),(109.7,21.55),(110.2,21.4),(111.2,21.45),
         (111.2,23.4),(102.1,23.4)]
south = [(106.55,16.95),(106.85,17.05)] + coast[coast.index((107.1,17.02)):] + west_coast[1:6] + cambodia_vn[::-1][1:] + \
        [(107.6,15.3),(107.45,15.6),(107.2,16.15)]
delta = [(105.3,21.45),(105.85,21.35),(106.4,21.25),(106.8,20.95),(106.7,20.62),(106.6,20.32),(106.1,19.95),(105.85,20.05),
         (105.75,20.4),(105.45,20.75),(105.25,21.1)]
red_river = [(103.95,22.52),(104.4,22.1),(104.87,21.7),(105.15,21.45),(105.42,21.3),(105.7,21.15),(105.86,21.04),(106.0,20.85),
             (106.1,20.6),(106.3,20.4),(106.55,20.25)]
black_river = [(103.15,22.05),(103.6,21.4),(104.3,21.15),(104.9,20.95),(105.33,20.82),(105.4,21.05),(105.42,21.3)]
ma_river = [(104.5,20.4),(105.0,20.15),(105.45,20.0),(105.8,19.85),(105.92,19.75)]
ca_river = [(104.3,19.3),(104.9,19.0),(105.4,18.8),(105.72,18.75)]
mekong = [(102.1,17.92),(102.6,17.99),(103.3,18.44),(103.65,18.4),(104.25,17.97),(104.8,17.4),(104.76,16.55),(105.0,15.97),
          (105.4,15.87),(105.55,15.6),(105.8,15.12),(105.9,14.3),(105.97,13.52),(106.0,12.48),(105.46,12.0),(104.92,11.57),
          (105.2,11.0),(105.35,10.8),(105.6,10.4),(106.0,10.05),(106.4,9.75),(106.62,9.6)]
bassac = [(104.92,11.57),(105.05,11.0),(105.12,10.7),(105.45,10.35),(105.78,10.03),(106.0,9.8),(106.25,9.55)]
ben_hai = [(106.55,16.98),(106.8,17.0),(107.1,17.03)]

w("# Vietnam, 1965-72, about a third of real size: the Red River delta,")
w("# Hanoi and Haiphong in the north, Route Package 1 down to the DMZ, and")
w("# South Vietnam down through the Central Highlands and Saigon to the")
w("# Mekong delta, with Laos, Cambodia and the bases in Thailand. North")
w("# Vietnam (and its trail and sanctuaries) is the enemy; you start at")
w("# Udorn. Its MiGs are 17s, 19s and 21s. maps/README.txt describes every")
w("# line.")
w("#")
w("# Run: flight maps/vietnam.map    (written by tools/vietnam.py)")
w()
w("map, Vietnam")
w("units, km                   # Positions in km, x east and y north of Vinh-ish")
m.bounds(102.25, 8.4, 111.0, 23.05)
w("color, jungle, 58 104 54")
w("color, paddy, 120 150 70")
w("color, tonkin, 46 98 132")
w("color, muddy, 128 112 80")
w("color, khorat, 150 150 92")
w("color, karst, 120 130 110")
w("ground, jungle")
w("outside, 70 96 62")
w("fighters, MiG-17, MiG-19, MiG-21")
w()
w("#")
w("# The countries: each a named zone, so the HUD says where you are")
w("#")
w()
m.poly("zone, Laos", "jungle", laos)
m.poly("zone, Thailand", "khorat", thailand)
m.poly("zone, China", "55 98 58", china)
m.poly("zone, Cambodia", "70 110 60", cambodia)
m.poly("zone, South Vietnam", "jungle", south)
m.poly("zone, Central Highlands", "64 100 56", [(107.6,15.2),(108.3,15.0),(108.6,13.8),(108.7,12.4),(108.2,11.6),(107.5,11.9),(107.45,12.4),
                                                  (107.6,13.4),(107.5,13.9),(107.55,14.7)])
m.poly("zone, Mekong Delta", "paddy", [(106.4,10.9),(106.72,10.2),(106.6,9.9),(106.3,9.6),(105.9,9.3),(105.5,8.95),(105.1,8.7),
                                       (104.8,9.0),(104.85,9.6),(105.0,10.0),(104.8,10.25),(104.95,10.55),(105.2,10.9),(105.85,11.0)])
m.poly("zone, Iron Triangle", "64 108 56", [(106.45,11.25),(106.65,11.2),(106.6,11.05),(106.45,11.08)])
m.poly("zone, A Shau Valley", "grass", [(107.25,16.35),(107.35,16.3),(107.2,16.0),(107.1,16.05)])
m.poly("zone, Tonle Sap", "paddy", [(103.4,13.3),(104.4,13.0),(104.7,12.4),(104.2,12.3),(103.6,12.8)])
m.poly("zone, Khorat Plateau", "khorat", [(102.25,15.6),(105.0,15.5),(104.9,14.6),(102.25,14.8)])
m.poly("zone, Red River Delta", "paddy", delta)
m.poly("zone, Thanh Hoa", "paddy", [(105.5,20.05),(105.9,20.0),(105.92,19.72),(105.82,19.45),(105.5,19.6)])
m.poly("zone, Plain of Jars", "grass", [(103.0,19.6),(103.5,19.65),(103.5,19.25),(103.0,19.2)])
m.poly("zone, Mu Gia Pass", "karst", [(105.6,18.0),(105.9,17.95),(105.95,17.65),(105.65,17.6)])
m.poly("zone, DMZ", "grass", [(106.55,16.9),(107.15,16.95),(107.15,17.1),(106.6,17.08)])
w()
w("#")
w("# Cities, then the sea over their seaward part")
w("#")
w()
cities = [
 ("Hanoi",105.85,21.03,3000),("Haiphong",106.68,20.86,2200),("Nam Dinh",106.17,20.42,1400),("Thanh Hoa",105.78,19.8,1300),
 ("Vinh",105.68,18.67,1400),("Dong Hoi",106.6,17.48,900),("Hue",107.58,16.46,1600),("Da Nang",108.2,16.05,2000),
 ("Lang Son",106.76,21.85,900),("Thai Nguyen",105.84,21.59,1300),("Viet Tri",105.4,21.32,1000),("Bac Giang",106.2,21.27,900),
 ("Hon Gai",107.08,20.96,1000),("Ninh Binh",105.97,20.25,900),("Phu Ly",105.92,20.54,700),("Yen Bai",104.87,21.71,800),
 ("Lao Cai",103.97,22.49,800),("Dien Bien Phu",103.02,21.38,700),("Son La",103.9,21.33,700),("Hoa Binh",105.33,20.82,800),
 ("Udon Thani",102.79,17.41,1500),("Nakhon Phanom",104.78,17.4,900),("Vientiane",102.6,17.97,1800),("Sakon Nakhon",104.15,17.16,900),
 ("Savannakhet",104.75,16.55,900),("Thakhek",104.8,17.4,600),("Quang Tri",107.19,16.75,800),
]
cities += [  # The South, Cambodia and southern Laos and Thailand
 ("Saigon",106.7,10.78,3600),("Bien Hoa",106.82,10.95,1400),("Vung Tau",107.1,10.38,900),("Can Tho",105.78,10.03,1500),
 ("My Tho",106.36,10.36,1000),("Long Xuyen",105.43,10.38,1000),("Rach Gia",105.1,10.0,800),("Ca Mau",105.15,9.18,900),
 ("Soc Trang",105.97,9.6,800),("Tay Ninh",106.1,11.31,900),("Da Lat",108.44,11.94,1100),("Nha Trang",109.17,12.24,1300),
 ("Phan Rang",108.97,11.57,800),("Phan Thiet",108.08,10.95,800),("Qui Nhon",109.2,13.77,1100),("Pleiku",108.0,13.98,1000),
 ("Kontum",108.0,14.35,800),("Ban Me Thuot",108.04,12.67,1100),("Quang Ngai",108.8,15.12,900),("Tuy Hoa",109.28,13.08,800),
 ("An Khe",108.65,13.95,600),("Hoi An",108.33,15.88,700),("Tam Ky",108.48,15.57,700),("Loc Ninh",106.58,11.84,500),
 ("Phnom Penh",104.92,11.56,2600),("Kampong Cham",105.46,12.0,800),("Siem Reap",103.86,13.36,900),("Battambang",103.2,13.1,1000),
 ("Sihanoukville",103.52,10.65,900),("Kampot",104.18,10.62,600),("Stung Treng",105.97,13.52,500),("Kratie",106.02,12.49,500),
 ("Pakse",105.8,15.12,900),("Ubon",104.86,15.24,1300),("Surin",103.49,14.88,800),("Buriram",103.1,14.99,800),
 ("Attapeu",106.83,14.8,400),("Tchepone",106.22,16.69,400),
]
for n, lo, la, r in cities:
    m.at("city", n, lo, la, str(r))
w()
w("# The Gulf of Tonkin")
m.poly("water", "tonkin", coast + [(104.75,8.2),(111.2,8.2),(111.2,21.45)] +
       [(110.2,21.4),(109.7,21.55),(109.3,21.5),(108.9,21.6),(108.5,21.65)])
w("# The Leizhou peninsula and Hainan, over the sea")
m.poly("zone, China", "55 98 58", [(109.7,21.55),(109.9,21.3),(109.75,20.8),(109.95,20.3),(110.3,20.22),(110.5,20.45),
                                   (110.4,21.0),(110.55,21.2),(111.2,21.45),(110.2,21.4)])
m.poly("zone, Hainan", "55 98 58", [(108.62,19.2),(108.7,18.5),(109.2,18.25),(109.6,18.2),(110.05,18.4),(110.45,18.8),
                                    (110.6,19.2),(111.0,19.65),(110.9,20.0),(110.6,20.1),(110.2,20.05),(109.6,20.0),
                                    (109.25,19.9),(108.9,19.6)])
m.mountain(109.75,18.95, 9000, 1400)                                  # Wuzhi Shan
w("# The Gulf of Thailand, and the Tonle Sap")
m.poly("water", "tonkin", west_coast + [(102.1,12.35),(102.1,8.2),(104.75,8.2)])
m.poly("water", "muddy", [(103.65,13.15),(104.2,12.95),(104.4,12.6),(104.15,12.5),(103.7,12.8)])
w("# Ha Long Bay's islands")
for lo, la in [(107.15,20.82),(107.3,20.85),(107.45,20.9),(107.25,20.72),(107.55,21.0),(107.0,20.75)]:
    m.mountain(lo, la, 900, 220, "color karst")
m.poly("area", "jungle", [(106.95,20.78),(107.1,20.8),(107.08,20.7),(106.97,20.7)])    # Cat Ba
w()
w("# Rivers")
m.poly("river, muddy", "450", red_river)
m.poly("river, muddy", "250", black_river)
m.poly("river, muddy", "250", ma_river)
m.poly("river, muddy", "220", ca_river)
m.poly("river, muddy", "500", mekong)
m.poly("river, muddy", "350", bassac)
m.poly("river, muddy", "200", [(106.4,11.4),(106.55,11.1),(106.7,10.78),(106.8,10.55),(106.85,10.4)])     # Saigon River
m.poly("river, muddy", "150", [(104.15,12.55),(104.5,12.0),(104.92,11.57)])                              # Tonle Sap River
m.poly("river, muddy", "120", ben_hai)
w()
w("# Roads and the Ho Chi Minh Trail")
m.poly("path, road", "25", [(106.76,21.85),(106.2,21.27),(105.92,21.08),(105.85,21.03),(105.92,20.54),(105.97,20.25),(105.78,19.8),
                            (105.68,18.67),(106.0,18.3),(106.6,17.48),(107.19,16.75),(107.58,16.46),(108.2,16.05)])
m.poly("path, road", "20", [(105.85,21.03),(106.2,20.95),(106.68,20.86)])
m.poly("path, road", "20", [(105.85,21.03),(105.4,21.32),(104.87,21.71),(103.97,22.49)])
m.poly("path, 140 120 84", "15", [(105.68,18.67),(105.75,18.0),(105.8,17.75),(105.6,17.3),(106.0,16.8),(106.4,16.2),(106.7,15.7)])
m.poly("path, road", "25", [(102.79,17.41),(103.5,17.3),(104.15,17.16),(104.78,17.4)])
m.poly("path, road", "20", [(102.6,17.97),(102.79,17.41)])
m.poly("path, road", "25", [(108.2,16.05),(108.48,15.57),(108.8,15.12),(109.2,13.77),(109.28,13.08),(109.17,12.24),(108.97,11.57),
                            (108.08,10.95),(107.3,10.9),(106.82,10.95),(106.7,10.78),(106.36,10.36),(105.78,10.03),(105.15,9.18)])   # Highway 1
m.poly("path, road", "20", [(109.2,13.77),(108.65,13.95),(108.0,13.98),(108.0,14.35)])
m.poly("path, road", "20", [(108.0,13.98),(108.04,12.67),(108.44,11.94),(108.97,11.57)])
m.poly("path, road", "20", [(106.7,10.78),(106.1,11.31),(105.46,12.0),(104.92,11.56),(104.18,10.62),(103.52,10.65)])
m.poly("path, road", "20", [(104.92,11.56),(103.86,13.36),(103.2,13.1)])
m.poly("path, 140 120 84", "15", [(106.7,15.7),(106.83,14.8),(106.9,14.0),(106.6,13.0),(106.5,12.2),(106.3,11.6)])   # The trail, on south
w()
w("#")
w("# Mountains: the Hoang Lien Son in the northwest, the hills of the")
w("# northeast, and the Annamites down the border with Laos")
w("#")
w()
m.ridge((103.4,22.75),(104.6,21.7), 14, 5000, 1900)
m.ridge((103.1,21.9),(104.6,20.9), 14, 4500, 1400)
m.ridge((102.7,21.3),(103.6,20.4), 10, 4500, 1300)
m.ridge((105.0,22.6),(105.6,22.0), 8, 4000, 1100)
m.ridge((106.3,22.5),(107.6,21.4), 14, 3500, 900)
m.ridge((105.2,22.4),(106.1,21.8), 10, 3500, 800, "color karst")
m.ridge((104.2,20.7),(105.0,20.3), 8, 4000, 1200)
m.ridge((103.6,20.0),(104.6,19.4), 10, 4500, 1500)
m.ridge((104.6,19.0),(105.6,18.1), 12, 4000, 1400)
m.ridge((105.5,18.0),(106.4,17.1), 12, 3500, 1100, "color karst")
m.ridge((106.4,16.9),(107.4,15.9), 12, 4000, 1300)
m.ridge((102.4,19.6),(103.0,18.8), 8, 4000, 1300)
m.mountain(105.95,20.3, 2500, 450, "color karst")
m.mountain(105.85,20.2, 2000, 380, "color karst")
m.mountain(106.0,21.45, 3500, 700, "color karst")                    # Thud Ridge
m.mountain(105.95,21.55, 3500, 650, "color karst")
m.ridge((107.65,15.35),(108.2,14.2), 9, 4500, 1600)                  # Central Highlands
m.ridge((107.9,13.4),(108.35,12.5), 6, 4000, 1300)
m.ridge((108.1,12.25),(108.7,12.05), 5, 4500, 1900)                  # Langbiang, round Da Lat
m.ridge((107.35,11.95),(107.9,11.55), 4, 4000, 900)
m.ridge((106.75,15.35),(107.2,14.75), 5, 4000, 1300)                 # Bolovens and Laos' south
m.ridge((105.9,15.35),(106.35,15.0), 4, 4000, 1100)
m.ridge((103.05,12.4),(103.7,11.45), 7, 4500, 1400)                  # The Cardamoms
m.ridge((103.95,11.15),(104.3,10.75), 3, 3500, 900)                  # The Elephants
m.ridge((103.3,14.35),(104.6,14.42), 6, 3500, 500)                   # Dangrek escarpment
m.mountain(106.17,11.38, 3000, 980)                                   # Nui Ba Den, the Black Virgin
m.mountain(105.0,10.45, 1500, 600)                                    # The Seven Mountains
w()
w("#")
w("# Friendly")
w("#")
w()
w("side, friendly")
w("start, Udorn")
w()
m.at("airbase", "Udorn", 102.79,17.39, "heading 120")
m.at("airbase", "Nakhon Phanom", 104.64,17.38, "heading 150")
m.at("airbase", "Da Nang", 108.2,16.04, "heading 170")
m.at("radar", "Lima Site 85", 103.65,20.43)
m.at("radar", "Udorn", 102.85,17.45)
m.at("sam", "Da Nang", 108.15,16.1, "launchers 4")
m.at("depot", "Nakhon Phanom depot", 104.7,17.35)
w()
w("# The South and Thailand's east")
for n, lo, la, h in [("Bien Hoa",106.82,10.98,90),("Tan Son Nhut",106.6,10.8,70),("Cam Ranh Bay",109.15,12.0,20),
                     ("Phan Rang",108.93,11.63,40),("Pleiku",108.02,14.0,90),("Nha Trang",109.15,12.23,120),
                     ("Binh Thuy",105.65,10.25,60),("Chu Lai",108.55,15.42,140),("Phu Cat",109.02,13.95,170),
                     ("Tuy Hoa",109.25,13.05,30),("Ubon",104.87,15.25,40)]:
    m.at("airbase", n, lo, la, "heading %d" % h)
for n, lo, la in [("Saigon",106.75,10.85),("Pleiku",108.05,14.05),("Monkey Mountain",108.15,16.12)]:
    m.at("radar", n, lo, la)
for n, lo, la in [("Nha Be tank farm",106.76,10.68),("Da Nang tank farm",108.15,16.0),("Cam Ranh tank farm",109.12,11.92),
                  ("Vung Tau tank farm",107.12,10.42),("Udorn fuel farm",102.84,17.35),("Qui Nhon tank farm",109.15,13.8)]:
    m.at("tanks", n, lo, la, "heading 30")
m.at("port", "Cam Ranh Bay port", 109.18,11.9, "heading 0")
m.at("depot", "Long Binh depot", 106.9,10.95)
w()
w("# Task Force 77: carriers at Yankee Station off the North and Dixie")
w("# Station off the South, each with its escorts, a destroyer on the gun")
w("# line and an oiler. Start on one with: start, USS Kitty Hawk")
m.at("carrier", "USS Kitty Hawk", 108.3,18.3, "heading 20")
m.at("carrier", "USS Constellation", 108.8,17.3, "heading 200")
m.at("carrier", "USS Enterprise", 109.6,16.4, "heading 340, escorts 4")
m.at("carrier", "USS Midway", 109.9,11.2, "heading 180, escorts 4")
m.at("destroyer", "USS Higbee", 107.5,17.9, "heading 330")
m.at("tanker", "USNS Taluga", 109.1,17.0, "heading 200")
w()
w("#")
w("# Enemy")
w("#")
w()
w("side, enemy")
w()
for n, lo, la, h in [("Phuc Yen",105.8,21.22,110),("Kep",106.38,21.39,90),("Gia Lam",105.93,21.06,110),("Hoa Lac",105.5,21.0,160),
                     ("Kien An",106.6,20.8,60),("Bai Thuong",105.38,19.9,130),("Vinh",105.63,18.87,170),("Yen Bai",104.87,21.67,130),
                     ("Dong Hoi",106.52,17.5,170)]:
    m.at("airbase", n, lo, la, "heading %d" % h)
w()
for n, lo, la in [("Hanoi",105.9,21.1),("Haiphong",106.6,20.9),("Thanh Hoa",105.75,19.85),("Vinh",105.6,18.7),("Dong Hoi",106.55,17.55),
                  ("Lang Son",106.7,21.8),("Yen Bai",104.75,21.6)]:
    m.at("radar", n, lo, la)
w()
for n, lo, la, l in [("Hanoi North",105.85,21.15,6),("Hanoi West",105.7,21.0,6),("Hanoi South",105.88,20.92,6),("Hanoi East",106.0,21.05,6),
                     ("Phuc Yen",105.75,21.26,5),("Kep",106.42,21.43,5),("Thai Nguyen",105.8,21.62,5),("Viet Tri",105.5,21.35,4),
                     ("Bac Giang",106.25,21.22,4),("Haiphong North",106.65,20.95,6),("Haiphong South",106.7,20.78,5),
                     ("Hon Gai",107.05,21.0,4),("Nam Dinh",106.15,20.48,5),("Ninh Binh",105.95,20.3,4),("Thanh Hoa",105.82,19.85,6),
                     ("Bai Thuong",105.42,19.95,4),("Vinh",105.7,18.62,6),("Ha Tinh",105.9,18.35,4),("Mu Gia",105.8,17.85,4),
                     ("Dong Hoi",106.62,17.42,5),("DMZ",106.85,17.15,4),("Yen Bai",104.78,21.82,4)]:
    m.at("sam", n, lo, la, "launchers %d" % l)
w()
w("bridge, Paul Doumer Bridge, %s, heading 45" % km(105.86,21.045))
w("bridge, Thanh Hoa Bridge, %s, heading 30         # The Dragon's Jaw" % km(105.81,19.85))
w("bridge, Viet Tri bridge, %s, heading 0" % km(105.42,21.3))
w("bridge, Phu Ly bridge, %s, heading 90" % km(105.91,20.55))
w("bridge, Hai Duong bridge, %s, heading 90" % km(106.33,20.94))
m.at("factory", "Thai Nguyen steel works", 105.86,21.62, "heading 30")
m.at("depot", "Yen Vien rail yard", 105.92,21.08, "heading 60")
m.at("powerplant", "Hanoi power plant", 105.82,21.0)
m.at("bunker", "Hanoi command post", 105.84,21.04)
m.at("depot", "Hanoi fuel depot", 105.82,20.96)
m.at("port", "Haiphong port", 106.7,20.86, "heading 70")
m.at("depot", "Haiphong fuel depot", 106.66,20.83)
m.at("powerplant", "Haiphong power plant", 106.62,20.87)
m.at("port", "Hon Gai port", 107.08,20.94, "heading 80")
m.at("factory", "Nam Dinh textile mill", 106.18,20.43)
m.at("factory", "Viet Tri chemical plant", 105.35,21.37)
m.at("depot", "Kep rail yard", 106.35,21.36)
m.at("depot", "Lang Son depot", 106.74,21.83)
m.at("powerplant", "Thanh Hoa power plant", 105.76,19.79)
m.at("port", "Vinh port", 105.72,18.68, "heading 100")
m.at("depot", "Vinh depot", 105.64,18.7)
m.at("depot", "Mu Gia Pass depot", 105.78,17.8)
m.at("depot", "Ban Karai depot", 106.15,17.2)
m.at("depot", "Dong Hoi depot", 106.58,17.46)
m.at("bunker", "Hoa Binh command post", 105.3,20.84)
m.at("depot", "Dien Bien Phu depot", 103.04,21.4)
w()
w("# Komar and Osa missile boats in the Gulf of Tonkin")
for n, lo, la, h in [("Komar boats",106.95,20.55,120),("Osa boats",107.4,20.6,200),("Komar boat",106.4,19.75,90),
                     ("Osa boat",106.15,18.6,160)]:
    m.at("missileboat", n, lo, la, "heading %d%s" % (h, ", escorts 2" if n.endswith("s") else ""))
for n, lo, la in [("Haiphong tank farm",106.74,20.88),("Hanoi tank farm",105.95,20.97),("Nam Dinh tank farm",106.2,20.38),
                  ("Vinh tank farm",105.62,18.62),("Thanh Hoa tank farm",105.73,19.76)]:
    m.at("tanks", n, lo, la, "heading 60")
w()
w("# The trail and the sanctuaries")
m.at("depot", "Tchepone depot", 106.24,16.66)
m.at("depot", "A Shau Valley depot", 107.25,16.2)
m.at("depot", "Ban Laboy depot", 106.1,17.4)
m.at("depot", "Attapeu depot", 106.85,14.78)
m.at("depot", "Parrot's Beak depot", 105.95,11.1)
m.at("bunker", "COSVN command post", 106.25,11.7)
m.at("depot", "Fishhook depot", 106.35,11.75)
m.at("depot", "Iron Triangle depot", 106.55,11.15)
m.at("depot", "U Minh depot", 105.05,9.4)
m.at("port", "Sihanoukville port", 103.5,10.63, "heading 90")
m.at("depot", "Sihanouk trail depot", 104.2,11.3)
for n, lo, la, l in [("Tchepone",106.2,16.72,4),("Ban Karai",106.1,17.25,4),("A Shau",107.2,16.25,4)]:
    m.at("sam", n, lo, la, "launchers %d" % l)

m.save("vietnam.map")
