# Writes maps/europe.map: the Cold War's front line in central Europe,
# 1985, from the Rhine-Main bases across the Fulda Gap to Berlin and
# Prague, at half real size, from rough longitudes and latitudes.
# Run: python3 tools/europe.py
from maptool import Map

m = Map(11.7, 50.8, 2.0)
w, km = m.w, m.km

igb = [  # The inner German border, north to south
 (11.0,52.75),(10.8,52.45),(10.95,52.1),(10.6,51.95),(10.55,51.6),(10.2,51.45),(9.95,51.25),(10.05,51.05),(9.95,50.85),
 (10.05,50.6),(10.4,50.4),(10.8,50.32),(11.25,50.4),(11.6,50.42),(11.9,50.38),(12.1,50.3),
]
czech_west = [(12.1,50.3),(12.35,50.2),(12.45,49.95),(12.4,49.75),(12.6,49.45),(12.9,49.35),(13.4,49.05),(13.8,48.75)]
czech_ddr = [(12.1,50.3),(12.5,50.4),(13.0,50.5),(13.5,50.62),(14.0,50.8),(14.3,50.88),(14.6,50.88),(14.82,50.87)]
oder_neisse = [(14.82,50.87),(15.0,51.1),(14.75,51.6),(14.6,52.0),(14.65,52.35),(14.25,52.75)]
poland_czech = [(14.82,50.87),(15.05,51.0),(15.3,50.95)]

w("# Central Europe, 1985, at half real size: the US and West German bases")
w("# around Frankfurt and up to Hannover, the Fulda Gap, East Germany and")
w("# West Berlin, and Czechoslovakia down to Prague and Pilsen. The Warsaw")
w("# Pact is the enemy; West Berlin's airfields are friendly, deep inside.")
w("# You start at Rhein-Main. maps/README.txt describes every line.")
w("#")
w("# Run: flight maps/europe.map    (written by tools/europe.py)")
w()
w("map, Central Europe 1985")
w("units, km                   # Positions in km, x east and y north of Gera-ish")
m.bounds(8.2, 48.85, 15.3, 52.75)
w()
w("color, fields, 118 140 76")
w("color, forest, 52 88 50")
w("color, eastfields, 140 140 80")
w("color, czfields, 150 146 92")
w("color, lake, 60 96 130")
w("color, flow, 70 104 132")
w("color, wall, 150 150 150")
w("ground, fields")
w("outside, 104 118 78")
w("fighters, MiG-21, MiG-23, MiG-29, Su-22")
w()
w("#")
w("# The countries, then forests over them")
w("#")
w()
m.poly("zone, East Germany", "eastfields", igb + czech_ddr[1:] + oder_neisse[1:] + [(11.0,52.75)])
m.poly("zone, Czechoslovakia", "czfields", czech_west[::-1] + czech_ddr[1:] + poland_czech[1:] + [(15.3,48.75),(13.8,48.75)])
m.poly("zone, Poland", "eastfields", oder_neisse[::-1] + poland_czech[1:] + [(15.3,52.75)])
m.poly("zone, West Berlin", "fields", [(13.1,52.6),(13.35,52.66),(13.48,52.6),(13.5,52.45),(13.38,52.38),(13.15,52.4),(13.1,52.5)])
w()
m.poly("zone, Fulda Gap", "fields", [(9.45,50.75),(9.9,50.8),(10.05,50.6),(9.85,50.4),(9.5,50.45)])
m.poly("zone, Harz", "forest", [(10.3,51.9),(10.8,51.85),(11.1,51.7),(10.75,51.6),(10.35,51.7)])
m.poly("zone, Thuringian Forest", "forest", [(10.3,50.95),(10.6,50.95),(11.3,50.6),(11.45,50.45),(11.2,50.4),(10.35,50.8)])
m.poly("zone, Ore Mountains", "forest", [(12.3,50.5),(12.9,50.62),(13.5,50.78),(13.9,50.85),(13.95,50.72),(13.4,50.58),(12.8,50.45),(12.3,50.35)])
m.poly("zone, Bohemian Forest", "forest", [(12.5,49.6),(12.95,49.5),(13.6,49.1),(13.9,48.9),(13.7,48.8),(13.3,49.0),(12.7,49.3)])
m.poly("zone, Bavarian Forest", "forest", [(12.4,49.45),(12.8,49.3),(13.3,49.0),(13.2,48.9),(12.6,49.15)])
m.poly("zone, Rhon", "forest", [(9.85,50.55),(10.1,50.55),(10.05,50.3),(9.85,50.35)])
m.poly("zone, Vogelsberg", "forest", [(9.1,50.6),(9.4,50.6),(9.4,50.4),(9.1,50.4)])
m.poly("zone, Spessart", "forest", [(9.2,50.2),(9.6,50.15),(9.6,49.9),(9.25,49.95)])
m.poly("zone, Taunus", "forest", [(8.2,50.35),(8.65,50.3),(8.6,50.18),(8.2,50.2)])
m.poly("zone, Odenwald", "forest", [(8.75,49.85),(9.15,49.75),(9.1,49.45),(8.75,49.55)])
m.poly("zone, Teutoburg Forest", "forest", [(8.5,52.15),(8.9,51.95),(9.0,51.75),(8.8,51.8),(8.4,52.05)])
m.poly("zone, Spreewald", "forest", [(13.85,52.0),(14.1,51.95),(14.1,51.8),(13.85,51.85)])
m.poly("zone, Krkonose", "forest", [(15.0,50.85),(15.3,50.8),(15.3,50.65),(15.0,50.7)])
m.poly("zone, Saxon Switzerland", "forest", [(13.95,50.98),(14.3,50.95),(14.3,50.85),(13.95,50.88)])
w()
w("#")
w("# Cities, then lakes and rivers")
w("#")
w()
cities = [
 ("Frankfurt",8.68,50.11,3200),("Wiesbaden",8.24,50.08,1800),("Darmstadt",8.65,49.87,1600),("Hanau",8.92,50.13,1100),
 ("Wurzburg",9.93,49.79,1600),("Kassel",9.48,51.31,2000),("Gottingen",9.93,51.53,1400),("Hannover",9.73,52.37,3000),
 ("Braunschweig",10.52,52.27,2000),("Wolfsburg",10.78,52.42,1300),("Hildesheim",9.95,52.15,1200),("Nuremberg",11.08,49.45,3000),
 ("Bamberg",10.89,49.89,1200),("Bayreuth",11.58,49.95,1200),("Regensburg",12.1,49.02,1600),("Fulda",9.68,50.55,1200),
 ("Giessen",8.68,50.58,1300),("Marburg",8.77,50.81,1100),("Schweinfurt",10.23,50.05,1100),("Hof",11.92,50.32,1000),
 ("Coburg",10.96,50.26,900),("Aschaffenburg",9.15,49.97,1100),("Bielefeld",8.53,52.02,2000),("Paderborn",8.75,51.72,1400),
 ("Erlangen",11.0,49.6,1200),("Amberg",11.86,49.44,900),("Weiden",12.16,49.68,900),
 ("East Berlin",13.45,52.52,3500),("West Berlin",13.3,52.5,3200),("Potsdam",13.06,52.4,1600),("Leipzig",12.37,51.34,3000),
 ("Dresden",13.74,51.05,3000),("Magdeburg",11.63,52.13,2400),("Halle",11.97,51.48,2200),("Erfurt",11.03,50.98,2000),
 ("Karl-Marx-Stadt",12.92,50.83,2300),("Gera",12.08,50.88,1500),("Jena",11.59,50.93,1400),("Zwickau",12.5,50.72,1400),
 ("Plauen",12.14,50.5,1100),("Cottbus",14.33,51.76,1500),("Dessau",12.24,51.83,1400),("Wittenberg",12.65,51.87,1000),
 ("Weimar",11.33,50.98,1100),("Gotha",10.7,50.95,1000),("Eisenach",10.32,50.98,1000),("Suhl",10.69,50.61,900),
 ("Brandenburg",12.56,52.41,1300),("Frankfurt an der Oder",14.55,52.34,1300),("Gorlitz",14.99,51.15,1100),("Bautzen",14.42,51.18,1000),
 ("Riesa",13.29,51.31,900),("Torgau",13.0,51.56,800),("Stendal",11.86,52.6,1000),
 ("Prague",14.42,50.08,3500),("Pilsen",13.38,49.75,2200),("Karlovy Vary",12.87,50.23,1300),("Usti nad Labem",14.04,50.66,1500),
 ("Liberec",15.06,50.77,1500),("Ceske Budejovice",14.47,48.97,1600),("Cheb",12.37,50.08,1000),("Most",13.64,50.5,1300),
 ("Kladno",14.1,50.15,1300),("Teplice",13.83,50.64,1100),("Mlada Boleslav",14.9,50.41,1000),("Pisek",14.15,49.31,900),
 ("Tabor",14.66,49.41,900),("Klatovy",13.29,49.4,800),
]
for n, lo, la, r in cities:
    m.at("city", n, lo, la, str(r))
w()
m.poly("water", "lake", [(13.15,52.47),(13.2,52.48),(13.18,52.4),(13.13,52.42)])        # Wannsee
m.poly("water", "lake", [(13.6,52.45),(13.68,52.45),(13.68,52.41),(13.6,52.41)])        # Muggelsee
m.poly("water", "lake", [(9.38,51.2),(9.48,51.2),(9.46,51.16),(9.38,51.17)])             # Edersee-ish
m.poly("water", "lake", [(14.05,48.85),(14.3,48.82),(14.25,48.75),(14.05,48.78)])        # Lipno
m.poly("water", "lake", [(14.4,49.6),(14.45,49.55),(14.42,49.4),(14.38,49.45)])          # Orlik
w()
m.poly("river, flow", "250", [(14.47,50.35),(14.3,50.5),(14.1,50.62),(14.2,50.78),(13.95,50.95),(13.74,51.06),(13.47,51.16),(13.25,51.33),
                              (13.0,51.56),(12.65,51.87),(12.25,51.86),(11.85,51.95),(11.63,52.13),(11.72,52.4),(11.95,52.7),(12.0,52.8)])   # Elbe
m.poly("river, flow", "200", [(14.47,50.35),(14.42,50.08),(14.4,49.8),(14.38,49.45),(14.4,49.2),(14.47,48.97),(14.3,48.78)])          # Vltava
m.poly("river, flow", "220", [(8.2,50.0),(8.45,50.02),(8.68,50.1),(8.92,50.13),(9.15,49.97),(9.5,49.8),(9.93,49.79),(10.23,50.05),
                              (10.6,49.98),(10.89,49.92),(11.3,50.05)])     # Main
m.poly("river, flow", "200", [(9.65,51.42),(9.5,51.7),(9.4,51.9),(9.25,52.1),(9.05,52.3),(8.95,52.5),(9.0,52.75)])                     # Weser
m.poly("river, flow", "160", [(11.85,51.95),(11.95,51.6),(11.97,51.48),(11.85,51.25),(11.6,50.93),(11.45,50.6),(11.75,50.35)])        # Saale
m.poly("river, flow", "250", [(10.9,48.85),(11.5,48.8),(12.1,49.02),(12.55,48.9)])                                                 # Danube
m.poly("river, flow", "160", [(14.75,51.6),(14.33,51.76),(14.0,52.0),(13.8,52.2),(13.6,52.45),(13.3,52.52),(13.2,52.53),(12.95,52.5),
                              (12.56,52.41),(12.3,52.6)])     # Spree and Havel
m.poly("river, flow", "200", [(14.82,50.87),(15.0,51.1),(14.75,51.6),(14.6,52.0),(14.65,52.35),(14.25,52.75)])                       # Neisse and Oder
m.poly("river, flow", "150", [(13.55,50.15),(13.8,50.2),(14.1,50.45),(14.04,50.66)])                                                # Ohre
w()
w("# The Wall, round West Berlin")
m.poly("path, wall", "60", [(13.1,52.5),(13.1,52.6),(13.35,52.66),(13.48,52.6),(13.5,52.45),(13.38,52.38),(13.15,52.4),(13.1,52.5)])
w("# Autobahns")
m.poly("path, road", "30", [(8.68,50.11),(8.68,50.58),(9.48,51.31),(9.93,51.53),(9.73,52.37)])
m.poly("path, road", "30", [(8.68,50.11),(9.15,49.97),(9.93,49.79),(10.6,49.6),(11.08,49.45),(11.58,49.95),(11.92,50.32),(12.08,50.88),
                            (12.37,51.34),(12.65,51.87),(13.0,52.2),(13.3,52.4)])
m.poly("path, road", "30", [(9.73,52.37),(10.52,52.27),(10.95,52.15),(11.63,52.13),(12.56,52.41),(13.06,52.4),(13.3,52.4)])
m.poly("path, road", "30", [(9.48,51.31),(10.32,50.98),(11.03,50.98),(11.97,51.48),(12.37,51.34),(13.74,51.05),(14.99,51.15)])
m.poly("path, road", "25", [(13.74,51.05),(14.04,50.66),(14.42,50.08),(13.38,49.75),(12.6,49.45),(12.1,49.02)])
w()
w("#")
w("# Mountains")
w("#")
w()
m.ridge((10.4,51.8),(10.95,51.72), 6, 4000, 1100)              # Harz
m.mountain(10.62,51.8, 5000, 1140)                              # Brocken
m.ridge((10.4,50.88),(11.35,50.48), 10, 4000, 950)              # Thuringian Forest
m.ridge((12.35,50.42),(13.85,50.78), 14, 4500, 1200)            # Ore Mountains
m.ridge((12.7,49.45),(13.75,48.85), 12, 4500, 1350)             # Bohemian Forest
m.ridge((12.55,49.3),(13.25,48.95), 8, 4000, 1300)              # Bavarian Forest
m.ridge((9.9,50.5),(10.0,50.35), 3, 4000, 950)                  # Rhon
m.mountain(9.25,50.5, 6000, 770)                                # Vogelsberg
m.ridge((8.25,50.25),(8.6,50.25), 4, 3500, 880)                 # Taunus
m.ridge((9.3,50.1),(9.5,49.95), 3, 3500, 580)                   # Spessart
m.ridge((8.8,49.75),(9.0,49.5), 3, 3500, 620)                   # Odenwald
m.ridge((8.5,52.1),(8.9,51.85), 4, 3000, 450)                   # Teutoburg Forest
m.ridge((11.6,50.15),(11.9,49.95), 4, 4000, 1050)               # Fichtel Mountains
m.ridge((15.0,50.78),(15.3,50.72), 3, 4500, 1600)               # Krkonose
m.ridge((14.6,50.8),(15.0,50.85), 3, 4000, 1000)                # Jizera
m.ridge((14.0,50.95),(14.3,50.9), 3, 3000, 550, "color rock")   # Elbe sandstone
m.ridge((13.6,49.95),(14.1,49.75), 4, 3500, 850)                # Brdy
m.ridge((13.4,50.5),(14.0,50.6), 3, 3000, 830)                  # Ceske Stredohori
w()
w("#")
w("# Friendly")
w("#")
w()
w("side, friendly")
w("start, Rhein-Main")
w()
for n, lo, la, h in [("Rhein-Main",8.6,50.03,70),("Wiesbaden",8.33,50.05,80),("Fritzlar",9.29,51.12,120),("Giebelstadt",9.97,49.65,80),
                     ("Ansbach",10.63,49.31,90),("Grafenwohr",11.94,49.7,90),("Wunstorf",9.43,52.46,80),("Buckeburg",9.08,52.28,80),
                     ("Gutersloh",8.31,51.92,90),("Tempelhof",13.4,52.47,90),("Tegel",13.29,52.56,80),("Gatow",13.14,52.47,80),
                     ("Fliegerhorst Fassberg",10.18,52.6,90)]:
    m.at("airbase", n, lo, la, "heading %d" % h)
w()
for n, lo, la in [("Wasserkuppe",9.94,50.5),("Schneeberg",11.85,50.05),("Teufelsberg",13.24,52.5),("Brocken watch",10.3,51.8)]:
    m.at("radar", n, lo, la)
for n, lo, la in [("Frankfurt",8.75,50.2),("Fulda",9.55,50.6),("Kassel",9.55,51.25),("Hannover",9.85,52.3),("Wurzburg",9.85,49.85),
                  ("Bayreuth",11.5,49.9),("Grafenwohr",12.0,49.65),("Braunschweig",10.4,52.2)]:
    m.at("sam", n, lo, la, "launchers 4")
m.at("depot", "Frankfurt depot", 8.75,50.05)
m.at("bunker", "Ramstein relay", 8.4,49.95)
w()
w("#")
w("# Enemy")
w("#")
w()
for n, lo, la in [("Rhein-Main fuel farm",8.6,50.02),("Ingolstadt tank farm",11.45,48.95),("Hanau fuel depot",8.95,50.12),
                  ("Kassel fuel depot",9.45,51.28)]:
    m.at("tanks", n, lo, la, "heading 20")
w()
w("side, enemy")
w()
for n, lo, la, h in [("Merseburg",12.0,51.36,80),("Altenburg",12.5,50.98,40),("Nohra",11.2,50.98,90),("Juterbog",13.08,51.97,100),
                     ("Brand",13.73,52.03,90),("Zerbst",12.15,51.98,80),("Kothen",12.0,51.72,90),("Grossenhain",13.55,51.31,110),
                     ("Schonefeld",13.52,52.38,70),("Sperenberg",13.36,52.14,90),("Allstedt",11.45,51.38,80),("Cochstedt",11.42,51.86,80),
                     ("Drewitz",14.53,51.89,70),("Prague Kbely",14.55,50.12,60),("Pilsen Line",13.33,49.68,60),
                     ("Ceske Budejovice",14.42,48.95,90),("Zatec",13.51,50.3,50),("Mimon",14.73,50.6,90)]:
    m.at("airbase", n, lo, la, "heading %d" % h)
w()
for n, lo, la in [("Inselsberg",10.47,50.85),("Fichtelberg",12.95,50.43),("Torgau",13.0,51.5),("Wuensdorf",13.45,52.15),
                  ("Klinovec",12.97,50.4),("Cerchov",12.78,49.38),("Magdeburg",11.7,52.05)]:
    m.at("radar", n, lo, la)
w()
for n, lo, la, l in [("Eisenach",10.4,50.95,5),("Erfurt",11.1,51.02,5),("Gotha",10.75,50.9,4),("Meiningen",10.4,50.6,5),("Suhl",10.75,50.55,4),
                     ("Plauen",12.2,50.48,4),("Magdeburg",11.55,52.2,6),("Halberstadt",11.05,51.9,4),("Halle",12.05,51.55,5),
                     ("Leipzig North",12.35,51.42,6),("Leipzig South",12.4,51.25,5),("Dresden North",13.75,51.15,6),
                     ("Dresden South",13.7,50.98,5),("Karl-Marx-Stadt",12.95,50.88,5),("Berlin North",13.45,52.7,6),
                     ("Berlin East",13.7,52.55,6),("Berlin South",13.5,52.3,6),("Potsdam",12.95,52.35,5),("Cottbus",14.3,51.8,5),
                     ("Wuensdorf",13.5,52.12,6),("Stendal",11.9,52.55,4),("Cheb",12.45,50.05,5),("Karlovy Vary",12.95,50.2,5),
                     ("Pilsen",13.45,49.8,6),("Prague North",14.45,50.2,6),("Prague South",14.4,49.98,6),("Usti",14.0,50.6,4),
                     ("Klatovy",13.3,49.45,4),("Ceske Budejovice",14.5,49.02,5),("Liberec",15.0,50.8,4)]:
    m.at("sam", n, lo, la, "launchers %d" % l)
w()
m.at("refinery", "Leuna refinery", 12.02,51.32, "heading 60")
m.at("factory", "Buna chemical works", 11.95,51.4)
m.at("powerplant", "Boxberg power plant", 14.57,51.41)
m.at("powerplant", "Lippendorf power plant", 12.37,51.18)
m.at("bunker", "Wuensdorf headquarters", 13.47,52.17)
m.at("bunker", "Strausberg command post", 13.88,52.58)
m.at("bunker", "Prague command post", 14.43,50.1)
w("bridge, Dresden bridge, %s, heading 30" % km(13.74,51.06))
w("bridge, Magdeburg bridge, %s, heading 90" % km(11.65,52.13))
w("bridge, Torgau bridge, %s, heading 50" % km(13.0,51.56))
w("bridge, Wittenberg bridge, %s, heading 0" % km(12.65,51.87))
w("bridge, Prague bridge, %s, heading 90" % km(14.41,50.09))
m.at("factory", "Leipzig factory", 12.42,51.32)
m.at("factory", "Karl-Marx-Stadt works", 12.95,50.82)
m.at("factory", "Eisenach car works", 10.3,50.99)
m.at("factory", "Skoda works", 13.36,49.74)
m.at("factory", "Usti chemical plant", 14.02,50.67)
m.at("refinery", "Zaluzi refinery", 13.6,50.53)
for n, lo, la in [("Leuna tank farm",12.06,51.3),("Bohlen tank farm",12.38,51.2),("Zaluzi tank farm",13.64,50.5),
                  ("Kralupy tank farm",14.3,50.25),("Magdeburg fuel depot",11.68,52.13)]:
    m.at("tanks", n, lo, la, "heading 50")
m.at("depot", "Erfurt depot", 11.05,51.0)
m.at("depot", "Gotha depot", 10.68,50.93)
m.at("depot", "Ohrdruf depot", 10.73,50.83)
m.at("depot", "Halle rail yard", 11.98,51.47)
m.at("depot", "Magdeburg depot", 11.6,52.1)
m.at("depot", "Dresden rail yard", 13.72,51.04)
m.at("depot", "Milovice depot", 14.88,50.23)
m.at("depot", "Cheb depot", 12.39,50.1)
m.at("powerplant", "Prunerov power plant", 13.25,50.42)

m.save("europe.map")
