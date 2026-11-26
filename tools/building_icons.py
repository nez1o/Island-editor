# Pixel-art pictures for the island editor's Buildings tab (HHP house menu, tab 7): one front view per
# building the tab can place, drawn here from simple shapes (no game art), plus the tab's own icon.
#   picture(i) -> 300x300 rows of RGBA (a 60x60 drawing scaled 5x, nearest), for the cell pictures
#                 (Model/Layout_HouseShapeIcon_H<i>, packed by bridges_tab.py)
#   tab_icon(size) -> 0-255 mask (the layout tints it, like the game's IconCat* tab icons)
# Order = k_BuildingCells in main.cpp: Nook's Cranny, Museum, Resident Services, Able Sisters, Campsite,
# Redd's boat, tent, studio, hotel, empty house.
#
# usage: python3 tools/building_icons.py <preview.png>   (all pictures side by side + the tab icon)
import math

N = 60          # drawing size in pixels
SCALE = 5       # -> 300x300

NAMES = ["Nook's Cranny", 'Museum', 'Resident Services', 'Able Sisters', 'Campsite',
         "Redd's boat", 'Tent', 'Studio', 'Hotel', 'Empty house']

OUT = (70, 50, 36, 255)        # outline
SHADOW = (0, 0, 0, 40)


def new():
    return [[(0, 0, 0, 0)] * N for _ in range(N)]


def put(g, x, y, c):
    if 0 <= x < N and 0 <= y < N: g[y][x] = c


def rect(g, x0, y0, x1, y1, c):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1): put(g, x, y, c)


def box(g, x0, y0, x1, y1, fill, line=OUT):
    rect(g, x0, y0, x1, y1, line)
    rect(g, x0 + 1, y0 + 1, x1 - 1, y1 - 1, fill)


def roof(g, x0, x1, y_top, y_bot, c, line=OUT, overhang=2):
    """A trapezoid roof: y_top row spans x0+k..x1-k, widening to x0-overhang..x1+overhang at y_bot."""
    h = y_bot - y_top
    for i, y in enumerate(range(y_top, y_bot + 1)):
        t = i / h if h else 1
        a = round(x0 + (1 - t) * 6 - t * overhang)
        b = round(x1 - (1 - t) * 6 + t * overhang)
        rect(g, a, y, b, y, c)
        put(g, a, y, line); put(g, b, y, line)
    rect(g, round(x0 + 6), y_top, round(x1 - 6), y_top, line)
    rect(g, x0 - overhang, y_bot, x1 + overhang, y_bot, line)


def gable(g, xc, y_top, half, y_bot, c, line=OUT):
    """A triangle roof with its peak at (xc, y_top)."""
    h = y_bot - y_top
    for i, y in enumerate(range(y_top, y_bot + 1)):
        w = round(half * i / h)
        rect(g, xc - w, y, xc + w, y, c)
        put(g, xc - w, y, line); put(g, xc + w, y, line)
    rect(g, xc - half, y_bot, xc + half, y_bot, line)


def ground(g, y=55):
    for x in range(4, N - 4):
        put(g, x, y, SHADOW); put(g, x, y + 1, SHADOW)


def window(g, x0, y0, w=7, h=7, glass=(170, 220, 240, 255), frame=(250, 245, 230, 255)):
    box(g, x0, y0, x0 + w, y0 + h, glass)
    rect(g, x0 + w // 2, y0 + 1, x0 + w // 2, y0 + h - 1, frame)
    rect(g, x0 + 1, y0 + h // 2, x0 + w - 1, y0 + h // 2, frame)


def door(g, x0, y0, w, h, c=(150, 95, 55, 255), knob=(240, 200, 80, 255)):
    box(g, x0, y0, x0 + w, y0 + h, c)
    put(g, x0 + w - 2, y0 + h // 2, knob)


# ---------------------------------------------------------------- the buildings
def cranny():
    g = new(); ground(g)
    box(g, 10, 28, 49, 54, (236, 200, 140, 255))                     # wooden walls
    for y in range(31, 54, 4): rect(g, 11, y, 48, y, (214, 172, 110, 255))
    roof(g, 10, 49, 12, 27, (70, 120, 200, 255))                     # blue roof
    for i, x in enumerate(range(8, 52, 4)):                           # striped awning
        rect(g, x, 28, x + 3, 32, (240, 140, 40, 255) if i % 2 == 0 else (250, 245, 230, 255))
    rect(g, 8, 33, 51, 33, OUT)
    window(g, 14, 37, 9, 8)
    door(g, 31, 38, 12, 16)
    box(g, 22, 15, 37, 21, (250, 245, 230, 255))                      # sign with a leaf
    rect(g, 28, 17, 31, 19, (90, 170, 80, 255))
    return g


def museum():
    g = new(); ground(g)
    stone, dark = (232, 222, 196, 255), (196, 184, 156, 255)
    gable(g, 29, 8, 24, 24, (90, 140, 90, 255))                       # green pediment roof
    box(g, 20, 14, 38, 23, stone)                                     # pediment face
    rect(g, 27, 16, 31, 20, (120, 90, 60, 255))                       # owl-ish emblem
    put(g, 28, 17, (250, 245, 230, 255)); put(g, 30, 17, (250, 245, 230, 255))
    box(g, 6, 25, 53, 28, dark)                                       # beam
    for x in (9, 18, 37, 46):                                         # columns
        box(g, x, 29, x + 4, 50, stone)
    box(g, 6, 29, 53, 50, (0, 0, 0, 0))
    rect(g, 7, 30, 52, 49, (180, 168, 140, 255))
    for x in (9, 18, 37, 46): box(g, x, 29, x + 4, 50, stone)
    door(g, 24, 34, 11, 16, (120, 80, 50, 255))
    box(g, 4, 50, 55, 54, dark)                                       # steps
    rect(g, 5, 52, 54, 52, stone)
    return g


def services():
    g = new(); ground(g)
    brick, mortar = (214, 120, 70, 255), (190, 100, 56, 255)
    box(g, 6, 26, 53, 54, brick)
    for y in range(29, 54, 3): rect(g, 7, y, 52, y, mortar)
    roof(g, 6, 53, 16, 26, (130, 110, 200, 255))                     # purple roof
    box(g, 22, 6, 37, 20, (250, 245, 230, 255))                       # clock tower
    gable(g, 29, 1, 9, 7, (130, 110, 200, 255))
    box(g, 25, 9, 34, 18, (255, 255, 255, 255))                       # clock face
    rect(g, 29, 11, 29, 14, OUT); rect(g, 30, 14, 32, 14, OUT)
    window(g, 10, 33, 7, 9); window(g, 42, 33, 7, 9)
    box(g, 20, 30, 23, 54, (250, 245, 230, 255)); box(g, 36, 30, 39, 54, (250, 245, 230, 255))   # pillars
    door(g, 24, 36, 11, 18, (140, 85, 50, 255))
    return g


def sisters():
    g = new(); ground(g)
    box(g, 9, 26, 50, 54, (210, 70, 60, 255))                         # red walls
    roof(g, 9, 50, 10, 26, (70, 150, 110, 255))                       # green roof
    box(g, 12, 28, 47, 33, (40, 70, 130, 255))                        # sign board
    rect(g, 15, 30, 44, 31, (250, 245, 230, 255))
    rect(g, 10, 34, 49, 36, (150, 200, 90, 255))                      # green awning
    window(g, 12, 39, 14, 12, (200, 230, 240, 255))                   # display window
    rect(g, 16, 44, 20, 49, (240, 180, 200, 255))                     # a dress in it
    door(g, 33, 38, 11, 16, (250, 245, 230, 255), (210, 170, 60, 255))
    return g


def campsite():
    g = new()
    wood, dark = (222, 184, 130, 255), (190, 150, 100, 255)
    box(g, 4, 44, 55, 52, wood)                                       # wooden deck
    for x in range(9, 55, 6): rect(g, x, 45, x, 51, dark)
    for x in (6, 52):                                                 # posts + bunting
        box(g, x, 14, x + 2, 44, (150, 100, 60, 255))
    for i, x in enumerate(range(9, 52, 5)):
        c = [(240, 80, 70, 255), (250, 210, 60, 255), (80, 170, 220, 255)][i % 3]
        y = 16 + round(4 * math.sin(math.pi * (x - 8) / 44))
        gable(g, x + 2, y, 2, y + 4, c)
    gable(g, 29, 18, 17, 44, (240, 150, 60, 255))                     # tent
    gable(g, 29, 26, 6, 44, (120, 70, 40, 255))                       # its opening
    rect(g, 29, 18, 29, 25, OUT)
    return g


def redd():
    g = new()
    water, foam = (90, 160, 210, 255), (220, 240, 250, 255)
    rect(g, 0, 46, N - 1, 56, water)
    for x in range(2, N, 6): rect(g, x, 46, x + 2, 46, foam)
    hull = (120, 70, 140, 255)
    for i, y in enumerate(range(34, 46)):                             # purple hull, narrower at the bottom
        rect(g, 6 + i // 2, y, 53 - i // 2, y, hull)
        put(g, 6 + i // 2, y, OUT); put(g, 53 - i // 2, y, OUT)
    rect(g, 6, 34, 53, 34, OUT)
    rect(g, 8, 37, 51, 38, (240, 200, 80, 255))                       # gold stripe
    box(g, 20, 20, 39, 34, (236, 200, 140, 255))                      # cabin
    window(g, 23, 23, 6, 6); window(g, 31, 23, 6, 6)
    gable(g, 29, 12, 12, 20, (200, 60, 70, 255))                      # red roof
    box(g, 44, 8, 46, 34, (150, 100, 60, 255))                        # mast + flag
    gable(g, 50, 8, 4, 14, (240, 200, 80, 255))
    return g


def tent():
    g = new(); ground(g)
    gable(g, 29, 14, 24, 54, (120, 170, 90, 255))                     # green tent
    gable(g, 29, 30, 8, 54, (70, 50, 36, 255))                        # opening
    rect(g, 29, 14, 29, 29, OUT)
    box(g, 4, 50, 8, 54, (150, 100, 60, 255)); box(g, 51, 50, 55, 54, (150, 100, 60, 255))   # pegs
    return g


def studio():
    g = new(); ground(g)
    box(g, 8, 24, 51, 54, (245, 240, 230, 255))                       # white walls
    roof(g, 8, 51, 14, 24, (60, 60, 70, 255))                         # dark roof
    box(g, 20, 30, 39, 44, (40, 40, 50, 255))                         # camera body
    box(g, 25, 33, 34, 42, (120, 170, 220, 255))                      # lens
    rect(g, 28, 36, 31, 39, (40, 40, 50, 255))
    box(g, 34, 27, 38, 30, (220, 60, 60, 255))                        # flash
    door(g, 42, 40, 7, 14, (90, 90, 100, 255))
    return g


def hotel():
    g = new(); ground(g)
    box(g, 12, 10, 47, 54, (240, 220, 180, 255))                      # tall building
    roof(g, 12, 47, 4, 10, (200, 90, 60, 255), overhang=1)
    for y in (14, 24, 34):
        for x in (16, 26, 36): window(g, x, y, 6, 6)
    box(g, 21, 44, 38, 54, (200, 90, 60, 255))                        # entrance awning
    door(g, 25, 46, 9, 8, (150, 95, 55, 255))
    return g


def empty_house():
    g = new(); ground(g)
    box(g, 12, 28, 47, 54, (230, 210, 180, 255))                      # plain walls
    gable(g, 29, 10, 22, 28, (170, 150, 130, 255))                    # plain roof
    door(g, 24, 38, 11, 16, (170, 130, 90, 255))
    box(g, 38, 40, 50, 48, (250, 245, 230, 255))                      # "for sale" sign
    rect(g, 40, 43, 48, 44, (200, 60, 60, 255))
    box(g, 43, 48, 45, 54, (150, 100, 60, 255))
    return g


DRAW = [cranny, museum, services, sisters, campsite, redd, tent, studio, hotel, empty_house]


def picture(i):
    g = DRAW[i]()
    return [[g[y // SCALE][x // SCALE] for x in range(N * SCALE)] for y in range(N * SCALE)]


def tab_icon(size=128, ss=4):
    """The Buildings tab's icon: a white shop-front silhouette (roof, walls, awning, door gap)."""
    M = size * ss
    def inside(x, y):
        u, v = x / M, y / M
        if 0.12 <= u <= 0.88 and 0.45 <= v <= 0.90:                   # walls
            if 0.56 <= u <= 0.76 and v >= 0.62: return False          # door
            if 0.22 <= u <= 0.46 and 0.60 <= v <= 0.76: return False  # window
            return True
        top = 0.12
        if top <= v <= 0.42:                                          # roof (trapezoid)
            t = (v - top) / (0.42 - top)
            return 0.26 - 0.20 * t <= u <= 0.74 + 0.20 * t
        return False
    img = []
    for y in range(size):
        row = []
        for x in range(size):
            c = sum(inside(x * ss + i + 0.5, y * ss + j + 0.5) for i in range(ss) for j in range(ss))
            row.append(round(255 * c / (ss * ss)))
        img.append(row)
    return img


if __name__ == '__main__':
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import render_icons
    pics = [picture(i) for i in range(len(DRAW))]
    gap = 10
    W = len(pics) * (300 + gap) + 128 + gap
    bg = (205, 225, 200, 255)
    icon = tab_icon(128)
    rows = []
    for y in range(300):
        row = []
        for p in pics:
            row += [p[y][x] if p[y][x][3] else bg for x in range(300)] + [bg] * gap
        row += [(icon[y][x],) * 3 + (255,) if y < 128 else bg for x in range(128)] + [bg] * gap
        rows.append(row)
    render_icons.write_png(sys.argv[1], rows)
    print('wrote', sys.argv[1])
