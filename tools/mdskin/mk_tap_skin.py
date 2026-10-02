#!/usr/bin/env python3
"""Builds the MPC skin for Machinemodule Tap / Machinemodule Tap FX: one LCD page of on/off cells, one per source (the 16 tracks,
the reverb send, the delay send, and on the FX build "THRU": pass the plugin's own input through). Several can be on at once:
the tap outputs their sum, so one MPC track (or submix) can receive any set of Machinedrum channels. Same LCD look as
mk_skin.py (the Module's skin generator: fonts and toggle icon from art.json (tools/mdskin/mdartdump), drawing code copied, not
imported: mk_skin.py is a script). Per-user build output, never committed or distributed.

    mk_tap_skin.py <art.json> <params.json> <out-dir> <tap|fx> [skin=... ink=RRGGBB paper=RRGGBB]
"""
import json
import os
import sys

from PIL import Image, ImageDraw, ImageOps

TOOLS = os.environ.get("MPC_VST_TOOLS") or os.path.join(os.path.expanduser("~"), "mpc-vst", "tools")
sys.path.insert(0, TOOLS)
import shadow_skin as ss  # noqa: E402

FX = sys.argv[4] == "fx"
args = dict(a.split("=", 1) for a in sys.argv[5:])
PRESETS = {"md": ("5e0c0c", "ff4836"), "default": ("000000", "ffffff")}
_ink, _paper = PRESETS.get(args.get("skin", "md"), PRESETS["md"])
_ink, _paper = args.get("ink", _ink), args.get("paper", _paper)
INK = tuple(int(_ink[i:i + 2], 16) for i in (0, 2, 4))
PAPER = tuple(int(_paper[i:i + 2], 16) for i in (0, 2, 4))

S = 3
SKIN_W, SKIN_H = 1280, 628
CELL, LABEL_Y, CONTENT_Y, CONTENT_H, VALUE_Y, VALUE_H = 32, 3, 9, 14, 23, 9
TITLE_H, GRID_Y = 10, 11
CW = 40
COLS, ROWS = 5, 4
LCD_W = COLS * CW + 1
LCD_H = GRID_Y + ROWS * CELL + 1
BEZEL_W, LCD_EDGE = 12, 8
BEZEL = (6, 5, 6)


class Bmp:
    def __init__(self, d):
        self.w, self.h = d["w"], d["h"]
        self.rows = [int(r, 16) for r in d["rows"]]


class Font:
    def __init__(self, d):
        self.h, self.adv = d["h"], d["adv"]
        self.g = {int(k): Bmp(v) for k, v in d["glyphs"].items()}


art = json.load(open(sys.argv[1]))
F = {k: Font(v) for k, v in art["fonts"].items()}
TOGGLE = [Bmp(b) for b in art["bitmaps"]["toggle"]]


class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = bytearray(w * h)

    def set(self, x, y, on=True):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y * self.w + x] = 1 if on else 0

    def blit(self, b, x, y, on=True):
        for r in range(b.h):
            for c in range(b.w):
                if (b.rows[r] >> (63 - c)) & 1:
                    self.set(x + c, y + r, on)

    def fill(self, x, y, w, h, on=True):
        for r in range(h):
            for c in range(w):
                self.set(x + c, y + r, on)

    def dots_h(self, x0, x1, y):
        for x in range(x0, x1 + 1, 2):
            self.set(x, y)

    def dots_v(self, x, y0, y1):
        for y in range(y0, y1 + 1, 2):
            self.set(x, y)

    def text(self, font, s, x, y, on=True):
        for ch in s:
            g = font.g.get(ord(ch))
            if g:
                self.blit(g, x, y, on)
                x += g.w + 1
            else:
                x += font.adv + 1

    def text_centred(self, font, s, x0, w, y, on=True):
        self.text(font, s, x0 + (w - text_width(font, s)) // 2, y, on)

    def image(self):
        im = Image.frombytes("L", (self.w, self.h), bytes(0 if p else 255 for p in self.px))
        im = im.resize((self.w * S, self.h * S), Image.NEAREST)
        return ImageOps.colorize(im, black=INK, white=PAPER)


def text_width(font, s):
    w = sum((font.g[ord(c)].w if ord(c) in font.g else font.adv) + 1 for c in s)
    return max(0, w - 1)


# cells: (label, params.json key), in grid order
CELLS = [("TRK %d" % (i + 1), "src%d" % (i + 1)) for i in range(16)] + [("REV", "src_rev"), ("DEL", "src_del")]
if FX:
    CELLS.append(("THRU", "through"))
TITLE = "MACHINEMODULE TAP FX" if FX else "MACHINEMODULE TAP"
NAME, VENDOR = ("Machinemodule Tap FX" if FX else "Machinemodule Tap"), "sd88me"
OUT = os.path.join(sys.argv[3], "%s - VST - %s" % (VENDOR, NAME))
SKIN = os.path.join(OUT, "Plugin Skins")
os.makedirs(SKIN, exist_ok=True)
for f in os.listdir(SKIN):
    os.remove(os.path.join(SKIN, f))
PIDX = {p["key"]: i for i, p in enumerate(json.load(open(sys.argv[2]))["params"])}


def save_png(name, im):
    im.save(os.path.join(SKIN, name + ".png"), optimize=True)
    return name + ".png"


FR_X, FR_Y, FR_W, FR_H = 1, CONTENT_Y, CW - 1, CELL - CONTENT_Y


def cell_origin(k):
    return (k % COLS) * CW, GRID_Y + (k // COLS) * CELL


def static_page():
    cv = Canvas(LCD_W, LCD_H)
    cv.fill(0, 0, LCD_W, TITLE_H, True)
    cv.text(F["bold8"], TITLE, 2, 1, False)
    for k in range(COLS * ROWS):
        x0, y0 = cell_origin(k)
        cv.dots_h(x0, x0 + CW, y0)
        cv.dots_v(x0, y0, y0 + CELL - 1)
        if k < len(CELLS):
            cv.text_centred(F["tiny3x5"], CELLS[k][0], x0 + 1, CW - 1, y0 + LABEL_Y)
    cv.dots_v(LCD_W - 1, GRID_Y, LCD_H - 1)
    cv.dots_h(0, LCD_W - 1, LCD_H - 1)
    return cv


def toggle_image(on):
    cv = Canvas(FR_W, FR_H)
    ic = TOGGLE[1 if on else 0]
    cv.blit(ic, (FR_W - ic.w) // 2, (CONTENT_H - ic.h) // 2)
    cv.text_centred(F["tiny3x5"], "ON" if on else "OFF", 0, FR_W, VALUE_Y - CONTENT_Y + (VALUE_H - F["tiny3x5"].h) // 2)
    return cv.image()


def chassis(lcd_rect):
    im = Image.new("RGB", (SKIN_W, SKIN_H))
    px = im.load()
    for y in range(SKIN_H):
        v = int(182 - 22 * y / SKIN_H)
        for x in range(SKIN_W):
            px[x, y] = (v, v, v - 2)
    dr = ImageDraw.Draw(im)
    x0, y0, x1, y1 = lcd_rect
    dr.rounded_rectangle([x0 - BEZEL_W, y0 - BEZEL_W, x1 + BEZEL_W, y1 + BEZEL_W], radius=12, fill=BEZEL, outline=(80, 80, 82))
    dr.rectangle([x0 - 1, y0 - 1, x1, y1], fill=(0, 0, 0))
    edge = tuple(int(c * 0.80) for c in PAPER)
    for d in range(LCD_EDGE):
        f = d / LCD_EDGE
        dr.rectangle([x0 + d, y0 + d, x1 - 1 - d, y1 - 1 - d], outline=tuple(int(e + (p_ - e) * f) for e, p_ in zip(edge, PAPER)))
    dr.rectangle([x0 + LCD_EDGE, y0 + LCD_EDGE, x1 - 1 - LCD_EDGE, y1 - 1 - LCD_EDGE], fill=PAPER)
    return im


PW, PH = LCD_W * S, LCD_H * S
PAD = LCD_EDGE + 6
LCD_RECT = ((SKIN_W - PW) // 2 - PAD, (SKIN_H - PH) // 2 - PAD, (SKIN_W + PW) // 2 + PAD, (SKIN_H + PH) // 2 + PAD)
PX, PY = (SKIN_W - PW) // 2, (SKIN_H - PH) // 2
bg = chassis(LCD_RECT)
bg.paste(static_page().image(), (PX, PY))
save_png("bg", bg)
save_png("clear", Image.new("RGBA", (8, 8), (0, 0, 0, 0)))
on_png, off_png = save_png("tog_on", toggle_image(True)), save_png("tog_off", toggle_image(False))

TOUCH_INSET = 2
TOUCH_W, TOUCH_H = (CW - 2 * TOUCH_INSET) * S, (CELL - 2 * TOUCH_INSET) * S
comps = [ss._sub("Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": "bg.png"}, ss._bounds(0, 0, SKIN_W, SKIN_H, show="Show"), "Background")]
defs = {}
tkey, dkey = "mdtTouch", "mdtToggle"
defs[tkey] = ss._local(tkey, [ss._action("Mouse Down", "Q-Link"), ss._action("Enter Pressed", "Toggle Switch")],
                       [ss._focus(TOUCH_W, TOUCH_H), ss._button("clear.png", "clear.png", 1, 1, TOUCH_W, TOUCH_H)])
defs[dkey] = ss._local(dkey, [], [ss._button(on_png, off_png, 1, 1, FR_W * S, FR_H * S)])


def place(ctype, name, index, x, y, w, h, focus="No"):
    m = [{"key": "Data", "value": "Parameter %d" % index}]
    comps.append({"version": 2, "componentData": {"version": 1, "name": name, "type": ctype, "data": {"version": 1, "handleName": "Data"}},
                  "handle remapping": {"version": 1, "map": m}, "bounds": ss._bounds(x, y, w, h, focus=focus, show="Show")})


for k, (label, key) in enumerate(CELLS):
    cx, cy = cell_origin(k)
    place(dkey, label, PIDX[key], PX + (cx + FR_X) * S, PY + (cy + FR_Y) * S, FR_W * S, FR_H * S)
    place(tkey, label + " touch", PIDX[key], PX + (cx + TOUCH_INSET) * S, PY + (cy + TOUCH_INSET) * S, TOUCH_W, TOUCH_H)

comp_bg = {"version": 1, "colour": "ff%02x%02x%02x" % PAPER, "image": ""}
comp = "MACHINEMODULE TAP|MAIN"
defs[comp] = {"key": comp, "value": {"version": 4, "actions": [], "backgroundData": {"version": 1, "focussed": comp_bg, "unfocussed": comp_bg},
                                     "ignoreMousePresses": False, "disableCoarseDataWheel": False, "repeats": 1,
                                     "hideQLinkBounds": True, "componentsData": comps}}
# Q-Link pages: tracks 1-16, then the sends (and THRU)
sets = [[c[1] for c in CELLS[:16]], [c[1] for c in CELLS[16:]]]
pages, qmap = [], []
for sp, keys in enumerate(sets):
    ql = {"Q-Link %d" % (q + 1): -1 for q in range(16)}
    for s_, k in enumerate(keys):
        ql["Q-Link %d" % ss.qlink_for_slot(s_)] = PIDX[k]
    pages.append({"version": 3, "tabName": "TAP", "fnKeyIndex": 0, "fnKeySubIndex": sp, "qlinkBoundsData": ["0 0 0 0"],
                  "componentName": comp, "initialSize": "0 0 %d %d" % (SKIN_W, SKIN_H), "scale": 1.0})
    qmap.append({"Tab": 1, "SubTab": sp + 1, "Bank Direction": "Column", "Q-Links": ql})
tui = {"pageData": {"version": 1, "componentDefinitions": {"version": 2, "importFiles": [ss.AKAI + "Generic/Generic Knob Overlay.json",
                                                                                        ss.AKAI + "Generic/Generic Menu Overlay.json"],
                                                          "localComponentDefinitions": list(defs.values())},
                    "info": {"version": 1, "type": "CompleteDescription"}, "tabs": pages}}
qlinks = {"version": 4, "info": {"version": 1, "type": "CompleteDescription"}, "Screen Mode Q-Links": {"version": 4, "map": qmap},
          "Program Mode Q-Links": dict(qmap[0]["Q-Links"])}
open(os.path.join(OUT, "version.xml"), "w").write(
    "<?xml version='1.0' encoding='utf-8'?>\n<plugincontent version=\"1.0\">\n\t<identifier>%s.vst.%s</identifier>\n"
    "\t<version>1.0.0.0</version>\n</plugincontent>\n" % (VENDOR, NAME.lower().replace(" ", "")))
for f, obj in (("TUI.json", tui), ("Q-Links.json", qlinks), ("Q-Links - 8by1.json", qlinks)):
    json.dump(obj, open(os.path.join(SKIN, f), "w"), indent=None if f == "TUI.json" else 1, separators=(",", ":") if f == "TUI.json" else None)
print("tap skin: %s (%d files)" % (OUT, len(os.listdir(SKIN))))
