# Shared by the map scripts (vietnam.py, korea.py, ...): turns rough
# longitudes and latitudes into a map's km, squeezed to fit, and writes
# the lines. maps/README.txt describes the file.
import math, os

class Map:
    def __init__(self, lon0, lat0, squeeze):
        self.lon0, self.lat0, self.squeeze = lon0, lat0, squeeze
        self.lines = []

    def xy(self, lon, lat):
        x = (lon - self.lon0) * 111.32 * math.cos(math.radians(self.lat0)) / self.squeeze
        y = (lat - self.lat0) * 110.6 / self.squeeze
        return x, y

    def km(self, lon, lat):
        return "%.1f, %.1f" % self.xy(lon, lat)

    def pts(self, lst, per=4):
        out = []
        for i in range(0, len(lst), per):
            out.append(",  ".join(self.km(*p) for p in lst[i:i + per]))
        return ",\n    ".join(out)

    def w(self, s=""):
        self.lines.append(s)

    # Bounds from the corners, in degrees
    def bounds(self, lon_w, lat_s, lon_e, lat_n):
        x0, y0 = self.xy(lon_w, lat_s)
        x1, y1 = self.xy(lon_e, lat_n)
        self.w("bounds, %.0f, %.0f, %.0f, %.0f" % (x0, y0, x1, y1))

    def poly(self, key, head, lst):
        self.w("%s, %s,\n    %s" % (key, head, self.pts(lst)))

    def at(self, key, name, lon, lat, rest=""):
        self.w("%s, %s, %s%s" % (key, name, self.km(lon, lat), (", " + rest) if rest else ""))

    def ridge(self, a, b, count, width, height, rest=""):
        self.w("ridge, %s, %s, %d, %d, %d%s" % (self.km(*a), self.km(*b), count, width, height,
                                                  (", " + rest) if rest else ""))

    def mountain(self, lon, lat, width, height, rest=""):
        self.w("mountain, %s, %d, %d%s" % (self.km(lon, lat), width, height, (", " + rest) if rest else ""))

    def save(self, name):
        here = os.path.dirname(os.path.abspath(__file__))
        path = os.path.join(here, "..", "maps", name)
        open(path, "w").write("\n".join(self.lines) + "\n")
        print("wrote", os.path.normpath(path))
