#!/usr/bin/env python3
"""Flamingo sprite sheet, authored as ASCII grids.

Why ASCII and not a paint program: every sprite here is under 32x44, and at that
size a character grid IS the pixel art — you can see the whole bird in the source
and change a wing by moving one letter. The builder can edit these without a drawing tool
and without touching any C.

'.' is transparent. Everything else is a key into PAL below.
Run this to emit the PNGs, then tools/png2lvgl.py to get the C arrays.
"""
import os
from PIL import Image

PAL = {
    'p': (240, 98, 146),   # flamingo pink
    'w': (255, 179, 201),  # wing highlight
    'd': (224, 86, 138),   # legs / shading
    'P': (216, 27, 96),    # deep pink
    'k': (32, 32, 32),     # eye / wheels
    'o': (255, 152, 0),    # beak
    'g': (46, 125, 50),    # tree dark green
    'G': (67, 160, 71),    # tree green
    'b': (109, 76, 65),    # trunk brown
    'c': (161, 130, 109),  # light twig brown — NOT 'B', which is shield blue
    'x': (55, 71, 79),     # mean bird
    'y': (38, 50, 56),     # mean bird dark
    'r': (229, 57, 53),    # heart red
    'B': (30, 136, 229),   # shield blue
    'W': (255, 255, 255),  # white
    'a': (255, 179, 0),    # amber
    's': (120, 144, 156),  # stone
    'S': (69, 90, 100),    # dark stone
    'n': (222, 228, 233),  # aircraft skin
    't': (211, 47, 47),    # tail / livery red
    'v': (176, 190, 197),  # wing grey
    'V': (100, 181, 246),  # cockpit / window
    'f': (255, 112, 0),    # flame
    'F': (255, 214, 0),    # flame hot
    'e': (255, 235, 59),   # duck yellow
    'h': (250, 160, 190),  # baby flamingo (paler than a grown one)
    'm': (109, 76, 65),    # mean bird, brown morph
    'M': (78, 52, 46),     #   ..its dark
    'q': (94, 53, 177),    # mean bird, violet morph
    'Q': (49, 27, 146),    #   ..its dark
}

FLAMINGO_1 = [                    # wings UP — drawn on the upbeat
    "................................",
    "........................pppp....",
    ".......................pppppp...",
    ".......................pppppPoo.",
    "......................ppkpppPooo",
    "......................pppppppoo.",
    ".....................pppppp.....",
    "....................ppppp.......",
    "...................pppp.........",
    "..................pppp..........",
    ".............ppppppppp..........",
    ".........ppppppppppppp..........",
    ".......wwwwwppppppppppp.........",
    "......wwwwwwwppppppppp..........",
    ".....pwwwwwwwwwppppppp..........",
    "....ppppppwwwwwppppp............",
    "ddddddppppppppppppp.............",
    "dddd...ppppppppp................",
    ".......ppppppppp................",
    "........ppppppp.................",
    ".........ppppp..................",
    "................................",
    "................................",
    "................................",
]

FLAMINGO_2 = [                    # wings DOWN — the downbeat
    "................................",
    "........................pppp....",
    ".......................pppppp...",
    ".......................pppppPoo.",
    "......................ppkpppPooo",
    "......................pppppppoo.",
    ".....................pppppp.....",
    "....................ppppp.......",
    "...................pppp.........",
    "..................pppp..........",
    ".............ppppppppp..........",
    ".........ppppppppppppp..........",
    ".......ppppppppppppppp..........",
    "......pppppppppppppppp.........."	,
    ".....ppppppppppppppppp..........",
    "....pppppwwwwwwwwwwww...........",
    "ddddddwwwwwwwwwwwwww............",
    "dddd..wwwwwwwwwwww..............",
    ".......wwwwwwwwww...............",
    "........wwwwwww.................",
    ".........wwwww..................",
    "................................",
    "................................",
    "................................",
]

TREE = [
    "....gggggg....",
    "..gggGGGGggg..",
    ".ggGGGGGGGGgg.",
    "ggGGGGGGGGGGgg",
    "ggGGGGGGGGGGgg",
    ".ggGGGGGGGGgg.",
    "..gggGGGGggg..",
    "...gggGGggg...",
    "....ggGGgg....",
    "......bb......",
    "......bb......",
    "......bb......",
    "......bb......",
    "......bb......",
    "......bb......",
    "......bb......",
    "......bb......",
    "......bb......",
    "......bb......",
    "......bb......",
    ".....bbbb.....",
    ".....bbbb.....",
]

CACTUS = [
    "....gggg....",
    "....gGGg....",
    "gg..gGGg..gg",
    "gGg.gGGg.gGg",
    "gGg.gGGg.gGg",
    "gGggggGggggg",
    "gGGGGGGGGGGg",
    ".gggGGgggg..",
    "....gGGg....",
    "....gGGg....",
    "....gGGg....",
    "....gGGg....",
    "....gGGg....",
    "....gGGg....",
    "....gGGg....",
    "....gggg....",
]

MEANBIRD = [
    "................",
    "...yy......yy...",
    "..yxxy....yxxy..",
    ".yxxxxy..yxxxxy.",
    "yxxxxxxyyxxxxxxy",
    ".yxxxxxxxxxxxxy.",
    "..yxxxWkxxxxy...",
    "...yxxxxxxyoo...",
    "....yxxxxyooo...",
    ".....yxxxy.oo...",
    "......yyy.......",
    "................",
]

ROCK = [
    "..ssss..",
    ".ssssss.",
    "ssssssss",
    "ssssssss",
    "ssssssss",
    ".ssssss.",
    "..ssss..",
    "........",
]

PU_LIFE = [
    "................",
    "..rrr....rrr....",
    ".rrrrr..rrrrr...",
    "rrrrrrrrrrrrrr..",
    "rrrrrrrrrrrrrr..",
    "rrrrrrrrrrrrrr..",
    ".rrrrrrrrrrrr...",
    "..rrrrrrrrrr....",
    "...rrrrrrrr.....",
    "....rrrrrr......",
    ".....rrrr.......",
    "......rr........",
    "................",
    "................",
    "................",
    "................",
]

PU_SHIELD = [
    "................",
    "..BBBBBBBBBB....",
    ".BBBBBBBBBBBB...",
    ".BBBWWWWWWBBB...",
    ".BBBWBBBBWBBB...",
    ".BBBWBBBBWBBB...",
    ".BBBWWWWWWBBB...",
    ".BBBBBBBBBBBB...",
    "..BBBBBBBBBB....",
    "...BBBBBBBB.....",
    "....BBBBBB......",
    ".....BBBB.......",
    "......BB........",
    "................",
    "................",
    "................",
]

PU_LIGHTER = [
    "................",
    ".........WW.....",
    "........WWWW....",
    ".......WWWWWW...",
    "......WWWWWWW...",
    ".....WWWWWWWW...",
    "....WWWWWWWW....",
    "...WWWWWWWWW....",
    "..WWWWWWWWW.....",
    "...WWWWWWW......",
    "....WWWWW.......",
    "...WWWWW........",
    "..WWW...........",
    ".WW.............",
    "W...............",
    "................",
]

PU_SLOW = [
    "................",
    "..aaaaaaaaaa....",
    "..aaaaaaaaaa....",
    "...aaaaaaaa.....",
    "....aaaaaa......",
    ".....aaaa.......",
    "......aa........",
    "......aa........",
    ".....aaaa.......",
    "....aaaaaa......",
    "...aaaaaaaa.....",
    "..aaaaaaaaaa....",
    "..aaaaaaaaaa....",
    "................",
    "................",
    "................",
]


# --- the villain -----------------------------------------------------------
# Flies LEFT, the way the world moves: nose at the left, tail fin at the right.
# Red tail so it reads as "the plane" at a glance and at 32px.
AIRPLANE = [
    "........................ttt.....",
    ".......................tttt.....",
    "......................ttttt.....",
    "...nnnnnnnnnnnnnnnnnnnttttt.....",
    ".nnnnVVnVVnVVnnnnnnnnnnnnnnn....",
    "nnnnnnnnnnnnnnnnnnnnnnnnnnnnn...",
    ".nnnnnnnnnnnnnnnnnnnnnnnnnnn....",
    "...nnnnnnvvvvvvvvnnnnnnnn.......",
    ".........vvvvvvvvvv.............",
    "..........vvvvvvvvvv............",
    "...........vvvvvvvv.............",
    "................................",
]

ASTEROID = [
    "....FF....",
    "...FffF...",
    "..fSSSSf..",
    ".fSssssSf.",
    "fSsssssssF",
    "fSssSssssf",
    ".fSssssSf.",
    "..fSSSSf..",
    "...ffff...",
    "....ff....",
]

# --- the pond ---------------------------------------------------------------
NEST = [
    "....bbbbbbbbbbbb....",
    "..bbccbbccbbccbbbb..",
    ".bccbbccbbccbbccbcb.",
    "bcbbccbbccbbccbbccbb",
    "bbccbbccbbccbbccbbcb",
    ".bbbccbbccbbccbbbbb.",
    "..bbbbbbbbbbbbbbbb..",
]

BABYFLAM = [
    "........hh..",
    ".......hhhh.",
    ".......hkhoo",
    "........hh..",
    "........hh..",
    "....hhhhhh..",
    "..hhhhhhhh..",
    "..hhhhhhh...",
    "...hhhhh....",
    "....dd.d....",
]

DUCK = [
    "........eee...",
    ".......eekeoo.",
    ".......eeee...",
    "....eeeeeee...",
    "..eeeeeeeeee..",
    ".eeeeeeeeeeee.",
    ".eeeeeeeeeee..",
    "..eeeeeeee....",
    "...ooo.ooo....",
    "..............",
]

TURTLE = [
    "..............",
    "...gggggggg...",
    "..gGGGGGGGGg..",
    ".gGGgGGgGGgGg.",
    "gGGGGGGGGGGGGg",
    "gggggggggggggg",
    ".gg..gg..gg.GG",
    "..............",
]

CATTAIL = [
    "..bb..",
    "..bb..",
    "..bb..",
    "..bb..",
    "..bb..",
    "..gg..",
    "..gg..",
    ".ggg..",
    "..gg..",
    "..ggg.",
    "..gg..",
    "..gg..",
    "..gg..",
    "..gg..",
    "..gg..",
    "..gg..",
]

# --- size variants ---------------------------------------------------------
# Distinct silhouettes rather than a scaled bitmap: pixel art scaled by a
# non-integer ratio goes mushy, and flash is free here (a few hundred bytes).
TREE_S = [
    "...gggg...",
    ".ggGGGGgg.",
    "ggGGGGGGgg",
    "ggGGGGGGgg",
    ".ggGGGGgg.",
    "..ggGGgg..",
    "...gGGg...",
    "....bb....",
    "....bb....",
    "....bb....",
    "....bb....",
    "....bb....",
    "....bb....",
    "....bb....",
    "....bb....",
    "...bbbb...",
]

TREE_L = [
    "......gggggg......",
    "....ggGGGGGGgg....",
    "...ggGGGGGGGGgg...",
    "..ggGGGGGGGGGGgg..",
    ".ggGGGGGGGGGGGGgg.",
    "ggGGGGGGGGGGGGGGgg",
    "ggGGGGGGGGGGGGGGgg",
    ".ggGGGGGGGGGGGGgg.",
    "..ggGGGGGGGGGGgg..",
    "...ggGGGGGGGGgg...",
    "....ggGGGGGGgg....",
    "......gGGGGg......",
    ".......bbbb.......",
    ".......bbbb.......",
    ".......bbbb.......",
    ".......bbbb.......",
    ".......bbbb.......",
    ".......bbbb.......",
    ".......bbbb.......",
    ".......bbbb.......",
    ".......bbbb.......",
    ".......bbbb.......",
    ".......bbbb.......",
    ".......bbbb.......",
    ".......bbbb.......",
    ".......bbbb.......",
    ".......bbbb.......",
    ".......bbbb.......",
    "......bbbbbb......",
    "......bbbbbb......",
]

CACTUS_S = [
    "...gggg...",
    "...gGGg...",
    "g..gGGg..g",
    "gg.gGGg.gg",
    "gGggGGggGg",
    "gGGGGGGGGg",
    ".ggGGGGgg.",
    "...gGGg...",
    "...gGGg...",
    "...gGGg...",
    "...gGGg...",
    "...gggg...",
]

CACTUS_L = [
    ".....gggg.....",
    ".....gGGg.....",
    "ggg..gGGg..ggg",
    "gGg..gGGg..gGg",
    "gGg..gGGg..gGg",
    "gGg..gGGg..gGg",
    "gGgggGGGgggGGg",
    "gGGGGGGGGGGGGg",
    ".gggGGGGGGgg..",
    ".....gGGg.....",
    ".....gGGg.....",
    ".....gGGg.....",
    ".....gGGg.....",
    ".....gGGg.....",
    ".....gGGg.....",
    ".....gGGg.....",
    ".....gGGg.....",
    ".....gGGg.....",
    ".....gGGg.....",
    ".....gGGg.....",
    ".....gggg.....",
    ".....gggg.....",
]

# --- the banner plane ------------------------------------------------------
# NOT the villain. The villain is a jet; this is a friendly biplane that tows
# the title across the sky before the story starts.
BIPLANE = [
    "................................",
    "vvvvvvvvvvvvvvvvvvvvvvvvvv......",
    "......nn........nn..............",
    "......nn........nn..............",
    ".....................ttt........",
    "W..nnnnnnnnnnnnnnnnntttt........",
    "WnnnnVVnnnnnnnnnnnnntttt........",
    "W..nnnnnnnnnnnnnnnnnnnn.........",
    "......nn........nn..............",
    "vvvvvvvvvvvvvvvvvvvvvvvvvv......",
    ".......k.........k..............",
    "......kkk.......kkk............."
]

SHEET = {
    "flamingo1": FLAMINGO_1, "flamingo2": FLAMINGO_2,
    "tree": TREE, "cactus": CACTUS, "meanbird": MEANBIRD, "rock": ROCK,
    "pu_life": PU_LIFE, "pu_shield": PU_SHIELD,
    "pu_lighter": PU_LIGHTER, "pu_slow": PU_SLOW,
    "airplane": AIRPLANE, "asteroid": ASTEROID,
    "nest": NEST, "babyflam": BABYFLAM, "duck": DUCK, "turtle": TURTLE,
    "cattail": CATTAIL,
    "tree_s": TREE_S, "tree_l": TREE_L,
    "cactus_s": CACTUS_S, "cactus_l": CACTUS_L,
    "biplane": BIPLANE,
}

# 🔑 The mean bird's colour morphs are the SAME grid with the palette letters
# swapped, so a change to the bird's SHAPE lands in every morph automatically
# and they can never drift apart.
def _recolour(grid, mapping):
    return ["".join(mapping.get(ch, ch) for ch in row) for row in grid]

SHEET["meanbird2"] = _recolour(MEANBIRD, {'x': 'm', 'y': 'M'})
SHEET["meanbird3"] = _recolour(MEANBIRD, {'x': 'q', 'y': 'Q'})

def build(grid):
    w = max(len(r) for r in grid)
    h = len(grid)
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    px = im.load()
    for y, row in enumerate(grid):
        for x, ch in enumerate(row):
            if ch != '.':
                if ch not in PAL:
                    raise SystemExit(f"unknown palette key {ch!r} at row {y}")
                px[x, y] = PAL[ch] + (255,)
    return im

if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    for name, grid in SHEET.items():
        widths = {len(r) for r in grid}
        if len(widths) > 1:
            raise SystemExit(f"{name}: rows are not all the same width: {sorted(widths)}")
        im = build(grid)
        im.save(os.path.join(here, name + ".png"))
        print(f"{name:12s} {im.size[0]:2d}x{im.size[1]:2d}  {im.size[0]*im.size[1]*2:5d} B in flash")
