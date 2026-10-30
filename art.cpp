#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <iostream>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <thread>
#include <chrono>
#include <fstream>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#ifdef _WIN32
  #include <windows.h>
#endif
using namespace std;

const long double pi = 3.14159265358979323846L;

// height / width of one terminal cell (usually about 2)
const double CELL_ASPECT = 2.0;

struct RGB { unsigned r, g, b; };
using color = vector<vector<RGB>>;
using gray  = vector<vector<unsigned>>;

const long double NA = 999.0L;      // "no angle": glyph has no preferred direction

// Effective signed intensity: sin(deg) acts as both the angular penalty and the sign (+ / -)
// + = ink grows downward / right (A, b, p, O, G, T, I ...)
// - = ink grows upward / left    (V, d, q, L, J ...)
long double W(long double I, long double deg) {
    return (deg == NA) ? I : I * sinl(deg * pi / 180.0L);
}

// ---------------- colour-space helpers ----------------
struct HSV { double h, s, v; };   // h in radians [0,2pi), s,v in [0,1]

HSV rgb2hsv(const RGB &c) {
    double r = c.r / 255.0, g = c.g / 255.0, b = c.b / 255.0;
    double mx = max({r, g, b}), mn = min({r, g, b}), d = mx - mn, h = 0;
    if (d > 1e-12) {
        if (mx == r)      h = fmod((g - b) / d, 6.0);
        else if (mx == g) h = (b - r) / d + 2.0;
        else              h = (r - g) / d + 4.0;
        h *= pi / 3.0;
        if (h < 0) h += 2 * pi;
    }
    return {h, mx > 0 ? d / mx : 0, mx};
}

RGB hsv2rgb(const HSV &c) {
    double h = c.h / (pi / 3.0);
    double C = c.v * c.s, X = C * (1 - fabs(fmod(h, 2.0) - 1)), m = c.v - C;
    double r = 0, g = 0, b = 0;
    int i = (int)floor(h) % 6;
    if (i == 0) { r = C; g = X; }
    else if (i == 1) { r = X; g = C; }
    else if (i == 2) { g = C; b = X; }
    else if (i == 3) { g = X; b = C; }
    else if (i == 4) { r = X; b = C; }
    else { r = C; b = X; }
    auto f = [](double v) { return (unsigned)lround(min(255.0, max(0.0, v * 255.0))); };
    return {f(r + m), f(g + m), f(b + m)};
}

// ---------------- loading / rotating / terminal ----------------
color loadImage(const string &path) {
    int iw, ih, n;
    unsigned char *data = stbi_load(path.c_str(), &iw, &ih, &n, 3);
    if (!data) { cerr << "Cannot load image: " << path << "\n"; exit(1); }
    color c(ih, vector<RGB>(iw));
    for (int y = 0; y < ih; y++)
        for (int x = 0; x < iw; x++) {
            unsigned char *p = data + 3 * (y * iw + x);
            c[y][x] = {p[0], p[1], p[2]};
        }
    stbi_image_free(data);
    return c;
}

color rotate90(const color &s) {            // clockwise
    int h = s.size(), w = s[0].size();
    color d(w, vector<RGB>(h));
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            d[x][h - 1 - y] = s[y][x];
    return d;
}

// ---------------- EXIF orientation (stb_image ignores it) ----------------
// returns 1..8 (1 = upright). Anything unreadable -> 1.
int readExifOrientation(const string &path) {
    ifstream f(path, ios::binary);
    if (!f) return 1;
    int a = f.get(), b = f.get();
    if (a != 0xFF || b != 0xD8) return 1;                 // not a JPEG

    while (f) {
        int m1 = f.get();
        if (m1 != 0xFF) return 1;
        int m2 = f.get();
        while (m2 == 0xFF) m2 = f.get();
        if (m2 == 0xDA || m2 == 0xD9 || m2 == EOF) return 1;   // image data starts: no EXIF
        int hi = f.get(), lo = f.get();
        int len = (hi << 8) | lo;
        if (len < 2) return 1;

        if (m2 == 0xE1 && len >= 16) {                    // APP1
            vector<unsigned char> d(len - 2);
            f.read((char *)d.data(), d.size());
            if (!f) return 1;
            if (d[0]!='E' || d[1]!='x' || d[2]!='i' || d[3]!='f') return 1;
            const unsigned char *t = d.data() + 6;        // TIFF header
            size_t tsz = d.size() - 6;
            if (tsz < 8) return 1;
            bool le = (t[0] == 'I');
            auto rd16 = [&](size_t o) -> unsigned {
                return le ? (t[o] | (t[o+1] << 8)) : ((t[o] << 8) | t[o+1]);
            };
            auto rd32 = [&](size_t o) -> unsigned {
                return le ? (t[o] | (t[o+1] << 8) | (t[o+2] << 16) | ((unsigned)t[o+3] << 24))
                          : (((unsigned)t[o] << 24) | (t[o+1] << 16) | (t[o+2] << 8) | t[o+3]);
            };
            size_t ifd = rd32(4);
            if (ifd + 2 > tsz) return 1;
            unsigned n = rd16(ifd);
            for (unsigned i = 0; i < n; i++) {
                size_t e = ifd + 2 + 12 * (size_t)i;
                if (e + 12 > tsz) return 1;
                if (rd16(e) == 0x0112) {                  // Orientation tag
                    unsigned o = rd16(e + 8);
                    return (o >= 1 && o <= 8) ? (int)o : 1;
                }
            }
            return 1;
        }
        f.seekg(len - 2, ios::cur);
    }
    return 1;
}

// make the pixels match what a normal image viewer shows
color applyOrientation(const color &s, int o) {
    if (o <= 1 || o > 8) return s;
    int h = s.size(), w = s[0].size();
    bool sw = (o >= 5);
    color d(sw ? w : h, vector<RGB>(sw ? h : w));
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int nx = x, ny = y;
            switch (o) {
                case 2: nx = w - 1 - x; ny = y;         break;   // mirror
                case 3: nx = w - 1 - x; ny = h - 1 - y; break;   // 180
                case 4: nx = x;         ny = h - 1 - y; break;   // flip vertical
                case 5: nx = y;         ny = x;         break;   // transpose
                case 6: nx = h - 1 - y; ny = x;         break;   // 90 clockwise
                case 7: nx = h - 1 - y; ny = w - 1 - x; break;   // transverse
                case 8: nx = y;         ny = w - 1 - x; break;   // 90 counter-clockwise
            }
            d[ny][nx] = s[y][x];
        }
    return d;
}

void enableAnsi() {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(h, &mode);
    SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
}

// block-average resize. mode 2 averages in HSV (circular hue), otherwise in RGB
color resizeImage(const color &src, int dw, int dh, int mode) {
    int sh = src.size(), sw = src[0].size();
    color dst(dh, vector<RGB>(dw));
    for (int y = 0; y < dh; y++) {
        int y0 = (long long)y * sh / dh;
        int y1 = max(y0 + 1, (int)((long long)(y + 1) * sh / dh));
        for (int x = 0; x < dw; x++) {
            int x0 = (long long)x * sw / dw;
            int x1 = max(x0 + 1, (int)((long long)(x + 1) * sw / dw));
            double r = 0, g = 0, b = 0, hx = 0, hy = 0, S = 0, V = 0, wsum = 0;
            long long cnt = 0;
            for (int yy = y0; yy < y1 && yy < sh; yy++)
                for (int xx = x0; xx < x1 && xx < sw; xx++) {
                    const RGB &p = src[yy][xx];
                    cnt++;
                    if (mode == 2) {
                        HSV c = rgb2hsv(p);
                        hx += c.s * cos(c.h); hy += c.s * sin(c.h);   // hue weighted by saturation
                        wsum += c.s; S += c.s; V += c.v;
                    } else { r += p.r; g += p.g; b += p.b; }
                }
            if (mode == 2) {
                HSV c;
                c.h = (wsum > 1e-12) ? atan2(hy, hx) : 0;
                if (c.h < 0) c.h += 2 * pi;
                c.s = S / cnt; c.v = V / cnt;
                dst[y][x] = hsv2rgb(c);
            } else
                dst[y][x] = {(unsigned)lround(r / cnt), (unsigned)lround(g / cnt), (unsigned)lround(b / cnt)};
        }
    }
    return dst;
}

// ---------------- image class ----------------
class image {
    protected:
        int h, w;
        color img;
        gray bw;
        gray gradient;
        vector<vector<long double>> angle;
    public:
        image(color &path) {
            img = path;
            h = path.size();
            w = path[0].size();
            bw       = gray(h, vector<unsigned>(w, 0));
            gradient = gray(h, vector<unsigned>(w, 0));
            angle    = vector<vector<long double>>(h, vector<long double>(w, 0.0L));
        }

        // useValue = true  -> brightness = max(R,G,B)  (needed for the colour rule)
        // useValue = false -> brightness = luminance   (plain grey output)
        void convert(bool useValue) {
            for (int x = 0; x < h; x++)
                for (int y = 0; y < w; y++) {
                    const RGB &p = img[x][y];
                    bw[x][y] = useValue
                        ? max({p.r, p.g, p.b})
                        : (unsigned)(0.299 * p.r + 0.587 * p.g + 0.114 * p.b);
                }
        }

        void grad() {
            int Gx[3][3] = {{-1, 0, 1}, {-2, 0, 2}, {-1, 0, 1}};
            int Gy[3][3] = {{-1,-2,-1}, { 0, 0, 0}, { 1, 2, 1}};
            for (int y = 1; y < h - 1; ++y)
                for (int x = 1; x < w - 1; ++x) {
                    float gradx = 0, grady = 0;
                    for (int ky = -1; ky <= 1; ++ky)
                        for (int kx = -1; kx <= 1; ++kx) {
                            unsigned p = bw[y + ky][x + kx];
                            gradx += p * Gx[ky + 1][kx + 1];
                            grady += p * Gy[ky + 1][kx + 1];
                        }
                    float m = sqrt(gradx * gradx + grady * grady);
                    gradient[y][x] = static_cast<unsigned>(min(255.0f, max(0.0f, m)));
                    angle[y][x] = (long double)atan2(grady, gradx);
                }
            // Sobel skips the 1-pixel border; copy the nearest inner value so border cells are not treated as flat
            if (h > 2 && w > 2) {
                for (int x = 0; x < w; x++) {
                    int xi = min(w - 2, max(1, x));
                    gradient[0][x] = gradient[1][xi];     angle[0][x] = angle[1][xi];
                    gradient[h-1][x] = gradient[h-2][xi]; angle[h-1][x] = angle[h-2][xi];
                }
                for (int y = 0; y < h; y++) {
                    int yi = min(h - 2, max(1, y));
                    gradient[y][0] = gradient[yi][1];     angle[y][0] = angle[yi][1];
                    gradient[y][w-1] = gradient[yi][w-2]; angle[y][w-1] = angle[yi][w-2];
                }
            }
        }

        // Returns signed assignment values in [-255.0, +255.0]
        vector<vector<long double>> flexer(float wi, float wg) {
            vector<vector<long double>> S(h, vector<long double>(w, 0.0L));
            for (int x = 0; x < h; x++)
                for (int y = 0; y < w; y++) {
                    long double th  = angle[x][y];
                    long double sy  = sinl(th);
                    long double cx  = cosl(th);
                    // Magnitude in [0, 255] using |sin(angle)| as the edge penalty
                    long double mag = wi * bw[x][y] + wg * gradient[x][y] * fabsl(sy);
                    // Negative when Gy < 0 (sy < 0), or when Gy == 0 and Gx < 0 (cx < 0 on vertical edge)
                    long double sgn = 1.0L;
                    if (gradient[x][y] > 0) {
                        if (sy < -1e-6L || (fabsl(sy) <= 1e-6L && cx < -1e-6L))
                            sgn = -1.0L;
                    }
                    S[x][y] = sgn * mag;   // in [-255.0, +255.0]
                }
            return S;
        }
};

struct table {
    string s;
    long double intensity;   // W(I, deg) : signed effective value in [-178, +178]
};
table E(const string &c, long double I, long double deg) { return {c, W(I, deg)}; }

// ---------------- art class ----------------
class art : private image {
    private:
        vector<table> T = {
            // Neutral (0.00)
            E(" ",   0.0,  NA),

            // ---------------- Positive Ladder (45 glyphs: +6.93 .. +178.00) ----------------
            //             I     deg       W(I, deg)
            E(".",   8.0,  60),  //   +6.93
            E(",",  12.5,  65),  //  +11.33
            E("_",  16.5,  70),  //  +15.50
            E("-",  21.0,  70),  //  +19.73
            E("^",  27.5,  60),  //  +23.82
            E(":",  28.0,  80),  //  +27.57
            E("!",  32.0,  80),  //  +31.51
            E("/",  49.5,  45),  //  +35.00
            E("i",  39.5,  80),  //  +38.90
            E("+",  45.0,  70),  //  +42.29
            E("(",  46.0,  85),  //  +45.82
            E("1",  50.0,  80),  //  +49.24
            E("I",  53.0,  90),  //  +53.00  (I is +)
            E("t",  58.5,  75),  //  +56.51
            E("[",  60.0,  85),  //  +59.77
            E("O",  66.5,  70),  //  +62.49  (O is hollow, +, moderate/not high)
            E("T",  67.0,  80),  //  +65.98  (T is +)
            E("{",  69.5,  85),  //  +69.24
            E("x", 102.5,  45),  //  +72.48
            E("u",  77.0,  80),  //  +75.83
            E("s",  84.0,  70),  //  +78.93
            E("o",  88.0,  70),  //  +82.69
            E("A",  99.5,  60),  //  +86.17  (A is +)
            E("U",  91.0,  80),  //  +89.62
            E("2",  96.5,  75),  //  +93.21
            E("G",  98.0,  80),  //  +96.51  (G is +, non-zero, denser than O)
            E("Z", 141.5,  45),  // +100.06
            E("5", 110.0,  70),  // +103.37
            E("e", 114.0,  70),  // +107.12
            E("a", 114.5,  75),  // +110.60
            E("P", 114.0,  90),  // +114.00
            E("b", 117.5,  90),  // +117.50  (b is +)
            E("p", 125.5,  75),  // +121.22  (p is +)
            E("6", 129.0,  75),  // +124.60
            E("g", 132.5,  75),  // +127.99
            E("E", 132.0,  85),  // +131.50
            E("H", 135.5,  90),  // +135.50
            E("R", 144.0,  75),  // +139.09
            E("0", 145.5,  80),  // +143.29
            E("Q", 152.5,  75),  // +147.30
            E("w", 167.5,  65),  // +151.81
            E("W", 172.5,  65),  // +156.34
            E("B", 162.0,  90),  // +162.00
            E("#", 176.5,  75),  // +170.49
            E("@", 178.0,  90),  // +178.00

            // ---------------- Negative Ladder (45 glyphs: -8.66 .. -175.30) ----------------
            //             I     deg       W(I, deg)
            E("'",  10.0, -60),  //   -8.66
            E("`",  14.0, -65),  //  -12.69
            E("\"", 18.5, -70),  //  -17.38
            E("~",  43.0, -30),  //  -21.50
            E(";",  26.5, -75),  //  -25.60
            E("=",  35.0, -60),  //  -30.31
            E("\\", 47.5, -45),  //  -33.59
            E("|",  37.0, -90),  //  -37.00
            E("l",  41.0, -85),  //  -40.84
            E("*",  51.0, -60),  //  -44.17
            E(")",  47.5, -85),  //  -47.32
            E("7",  72.0, -45),  //  -50.91
            E("J",  54.5, -90),  //  -54.50  (J is -)
            E("j",  60.0, -75),  //  -57.96
            E("]",  61.5, -85),  //  -61.27
            E("r",  68.5, -70),  //  -64.37
            E("L",  67.5, -90),  //  -67.50  (L is -)
            E("}",  71.0, -85),  //  -70.73
            E("c",  75.0, -80),  //  -73.86
            E("v",  89.0, -60),  //  -77.08
            E("z", 113.5, -45),  //  -80.26
            E("f",  84.0, -85),  //  -83.68
            E("V",  96.0, -65),  //  -87.01  (V is -)
            E("Y", 104.5, -60),  //  -90.50
            E("4",  97.5, -75),  //  -94.18
            E("C",  99.0, -80),  //  -97.50
            E("X", 143.0, -45),  // -101.12
            E("3", 111.5, -70),  // -104.78
            E("n", 110.0, -80),  // -108.33
            E("y", 123.5, -65),  // -111.93
            E("F", 116.0, -85),  // -115.56
            E("d", 119.0, -90),  // -119.00  (d is -)
            E("q", 127.0, -75),  // -122.67  (q is -)
            E("9", 130.5, -75),  // -126.05
            E("h", 130.0, -85),  // -129.51
            E("k", 138.0, -75),  // -133.30
            E("S", 146.0, -70),  // -137.20
            E("K", 143.0, -80),  // -140.83
            E("D", 145.0, -90),  // -145.00
            E("N", 172.0, -60),  // -148.96
            E("m", 163.5, -70),  // -153.64
            E("M", 182.5, -60),  // -158.05
            E("%", 231.0, -45),  // -163.34
            E("8", 172.0, -80),  // -169.39
            E("&", 178.0, -80)   // -175.30
        };
        vector<vector<double>> cover;   // ink coverage (0..1) of the glyph chosen for each cell

    public:
        art(color &c) : image(c) {}

        vector<vector<string>> draw(float wi, float wg, bool colorOn) {
            this->convert(colorOn);
            this->grad();
            vector<vector<long double>> flex = this->flexer(wi, wg);

            int rows = flex.size();
            int cols = rows ? flex[0].size() : 0;
            vector<vector<string>> out(rows, vector<string>(cols));
            cover.assign(rows, vector<double>(cols, 1.0));

            long double maxFlex = 0, maxT = 0;
            for (int x = 0; x < rows; x++)
                for (int y = 0; y < cols; y++)
                    maxFlex = max(maxFlex, fabsl(flex[x][y]));
            for (size_t i = 0; i < T.size(); i++)
                maxT = max(maxT, fabsl(T[i].intensity));
            long double scale = (maxFlex > 0) ? maxT / maxFlex : 0;

            for (int x = 0; x < rows; x++)
                for (int y = 0; y < cols; y++) {
                    // Keep the sign of flex[x][y] so + selects positive glyphs and - selects negative glyphs
                    long double v  = flex[x][y] * scale;
                    long double Vn = max({img[x][y].r, img[x][y].g, img[x][y].b}) / 255.0L;

                    // colour mode: coverage k must satisfy k >= Vn (colour <= 255)
                    // and stay close to Vn, so colour = block / k keeps full brightness
                    long double kmax = 1e9L, kfloor = Vn;
                    if (colorOn) {
                        long double kmin = 1e9L, maxSignK = 0.0L;
                        for (size_t i = 0; i < T.size(); i++) {
                            if (v * T[i].intensity < 0) continue;
                            long double k = fabsl(T[i].intensity) / maxT;
                            maxSignK = max(maxSignK, k);
                            if (k + 1e-9L >= Vn) kmin = min(kmin, k);
                        }
                        if (kmin > 1e8L) {
                            kmin = maxSignK;
                            kfloor = maxSignK - 1e-9L;
                        }
                        kmax = kmin * 1.15L + 0.02L;
                    }

                    size_t best = 0;
                    long double bestDist = 1e30L;
                    for (size_t i = 0; i < T.size(); i++) {
                        long double k = fabsl(T[i].intensity) / maxT;
                        if (colorOn && (v * T[i].intensity < 0 || k + 1e-9L < kfloor || k > kmax)) continue;
                        long double d = fabsl(v - T[i].intensity);
                        if (d < bestDist) { bestDist = d; best = i; }
                    }
                    out[x][y] = T[best].s;
                    cover[x][y] = (double)(fabsl(T[best].intensity) / maxT);
                }
            return out;
        }

        // glyph colour * glyph coverage = block colour  ->  glyph colour = block colour / coverage
        RGB cellColor(int x, int y) const {
            const RGB &m = img[x][y];
            double k = max(1e-3, cover[x][y]);          // only guards divide-by-zero
            auto f = [&](unsigned c) { return (unsigned)lround(min(255.0, c / k)); };
            return {f(m.r), f(m.g), f(m.b)};
        }

        void print(const vector<vector<string>> &out, int colorMode, int delayMs) {
            for (int x = 0; x < (int)out.size(); x++) {
                string line;
                for (int y = 0; y < (int)out[x].size(); y++) {
                    if (colorMode != 0 && out[x][y] != " ") {
                        RGB p = cellColor(x, y);
                        line += "\x1b[38;2;" + to_string(p.r) + ";" + to_string(p.g) + ";" + to_string(p.b) + "m";
                    }
                    line += out[x][y];
                }
                line += "\x1b[0m\n";
                cout << line << flush;                   // one row at a time
                this_thread::sleep_for(chrono::milliseconds(delayMs));
            }
        }
};
int main(int argc, char **argv) {
    if (argc < 5) {
        cerr << "Usage: art <image> <max_rows> <max_columns> <colour_mode 0|1|2> [wi] [wg] [r] [f]\n"
             << "  rows/columns = exact size of the art\n"
             << "  colour_mode  = 0 (grayscale), 1 (RGB), 2 (HSV)\n"
             << "  wi, wg       = optional weights (default: 0.85 0.15)\n"
             << "  r            = rotate portrait to landscape\n"
             << "  f            = keep image aspect ratio inside box\n";
        return 1;
    }
    enableAnsi();

    int rows = atoi(argv[2]);
    int cols = atoi(argv[3]);
    int mode = atoi(argv[4]);

    float wi = 0.85f;
    float wg = 0.15f;
    bool rotateFlag = false, fitFlag = false;

    // Parse optional arguments starting at index 5
    for (int i = 5; i < argc; i++) {
        string arg = argv[i];
        if (arg == "r") {
            rotateFlag = true;
        } else if (arg == "f") {
            fitFlag = true;
        } else {
            // If it's a number, assume it's wi, then wg
            float val = atof(argv[i]);
            if (i == 5) wi = val;
            else if (i == 6) wg = val;
        }
    }

    if (rows < 1 || cols < 1) { cerr << "rows and columns must be >= 1\n"; return 1; }
    if (mode < 0 || mode > 2) mode = 0;

    color src = loadImage(argv[1]);
    int exif = readExifOrientation(argv[1]);
    cerr << "EXIF orientation tag: " << exif << " (1 = upright)\n";
    src = applyOrientation(src, exif);
    if (rotateFlag && src.size() > src[0].size())
        src = rotate90(src);

    int R = rows, C = cols;
    if (fitFlag) {
        double sw = src[0].size(), sh = src.size();
        double r  = min((double)rows, cols * sh / (CELL_ASPECT * sw));
        R = max(1, (int)lround(r));
        C = min(cols, max(1, (int)lround(R * CELL_ASPECT * sw / sh)));
    }

    color small = resizeImage(src, C, R, mode == 2 ? 2 : 1);
    art a(small);
    
    // Pass the tuned weights into draw()
    auto out = a.draw(wi, wg, mode != 0);
    a.print(out, mode, 20);
    return 0;
}