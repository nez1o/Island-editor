/* Gyroid pixel art for "Gyroid's menu" (header watermark + phone icon).
   Hand-drawn placeholder; regenerate from a picture with tools/gyroid_art.py.
   One char per pixel: "." transparent, 1-f = k_GyroidPalette[n - 1]. */
static const uint8_t k_GyroidPalette[][3] = {
    { 92, 58, 34 },
    { 214, 162, 98 },
    { 240, 204, 148 },
    { 182, 130, 74 },
    { 58, 36, 22 },
    { 196, 140, 80 },
    { 160, 110, 60 },
};
static const char* const k_Gyroid[] = {
    "......1111......",
    "....11222211....",
    "...1233222221...",
    "..123222222241..",
    "..122522225241..",
    "..122522225241..",
    "..122222222241..",
    "..122225522241..",
    "...1222552241...",
    "....11222211....",
    "...1666666671...",
    "..166666666771..",
    "1216666666677121",
    "..166666666771..",
    ".16666666667771.",
    ".11111111111111.",
};
constexpr int GyroidW = 16, GyroidH = 16;
