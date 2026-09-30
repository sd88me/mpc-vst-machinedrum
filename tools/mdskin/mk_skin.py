#!/usr/bin/env python3
"""Builds the MPC skin for Machinedrum Module: mpc-vst-monomodule's own vst/skin/mk_skin.py (at 56cb8e0, its "2x2"
layout), adapted to the Machinedrum - the same LCD look, drawing code, knob cells, machine bar, picker and LEV
column, not a new design. Upstream-of-this differences are only what the Machinedrum needs:

  * 16 tabs, one per track (the Monomodule has one voice). Each tab is Monomodule's 2x2: SYN | AMP/EFX over
    ROUTE | the track's LFO page (the MD's own: TRACK PARAM SHP1 SHP2 / UPDTE SPEED DEPTH SHMIX). A 17th tab, GLOBAL:
    a 16-track LEV mixer (Monomodule's LEV column, 16 across) and VOICES.
  * Machines, labels: the MD OS's own machine table (tools/mdmachine's listing), 131 machines in 9 groups; the
    picker's columns are the groups (ROM split into columns of 16 so no column scrolls).
  * The machine param is a raw OS machine id (0-191), so IndexedEnabling / button ids use the id, n = 192.
  * SYN cells: one set of dials per track (not per machine - 131 x 16 would be ~33k components); the per-machine
    label grid is an overlay drawn over them, opaque except over its own live cells, so an unused cell is blank.
  * No preset/bank strip (the MD port has no presets) and no Shnolk logo (the vendor's own; "MD" in its place).

The LCD artwork (fonts, dial ring and dot) comes from art.json, which tools/mdskin/mdartdump makes from the
user's own Machinedrum OS (the same Elektron LCD font family as Monomodule's). Per-user build output, never committed or distributed.

    mk_skin.py <art.json> <mdmachine listing> <params.json> <out-dir> [skin=... ink=RRGGBB paper=RRGGBB]
"""
import json
import os
import re
import sys

from PIL import Image, ImageDraw, ImageOps

TOOLS = os.environ.get("MPC_VST_TOOLS") or os.path.join(os.path.expanduser("~"), "mpc-vst", "tools")
sys.path.insert(0, TOOLS)
import shadow_skin as ss  # noqa: E402  (TUI.json helpers shared with the other ports)

args = dict(a.split("=", 1) for a in sys.argv[5:])
PRESETS = {"default": ("000000", "ffffff"), "inverted": ("ffffff", "000000"), "lowcontrast": ("5c5c5c", "c4c4c4"),
           # the Machinedrum's own LCD (this port's default)
           "md": ("5e0c0c", "ff4836"),   # sampled from a photo of the real LCD: maroon pixels on the red backlight
           "red": ("ff3b2e", "1c0403"), "blue": ("5ab0ff", "04112b"), "green": ("52ff70", "031608"), "orange": ("ffa11f", "1e1000")}
_skin = args.get("skin", "md")
_swap = _skin.endswith("-inverted") and _skin != "-inverted"
_ink, _paper = PRESETS.get(_skin[:-9] if _swap else _skin, PRESETS["default"])
if _swap:
    _ink, _paper = _paper, _ink
_ink, _paper = args.get("ink", _ink), args.get("paper", _paper)

# Monomodule's "2x2" geometry, unchanged
S = int(args.get("scale", "3"))
SKIN_W, SKIN_H = 1280, 628
CELL, LABEL_Y, CONTENT_Y, CONTENT_H, VALUE_Y, VALUE_H = 32, 3, 9, 14, 23, 9
TITLE_H, GRID_Y = 10, 11
MARG, GAPX, PAGE_GAP = 10, 8, 12
BAR_ROWS = 26
ROW_GAP = PAGE_GAP
CW = int(args.get("cellw", "40"))
LCD_W = 4 * CW + 1
PAGE_GAP = MARG = int(args.get("gap", (SKIN_W - 2 * LCD_W * S) // 3))
PAGE_LCD_H = GRID_Y + 2 * CELL + 1
PAGE_ROWS = 2
LEV_W = 19 * S
PAGES_W = 2 * LCD_W * S + PAGE_GAP
BAR_X_W = MARG   # (Monomodule: + LEV_W + GAPX, room for its logo) the machine bar lines up with the pages
TOP = 8 + BAR_ROWS * S + 8
PAGES_X0 = MARG
WIN_W = PAGES_X0 + PAGES_W + MARG
WIN_H = TOP + PAGE_ROWS * PAGE_LCD_H * S + (PAGE_ROWS - 1) * ROW_GAP + 8
OX, OY = (SKIN_W - WIN_W) // 2, (SKIN_H - WIN_H) // 2
FRAMES = 128
TRACKS = 16
NMACH = 192   # the machine param's range (raw OS ids 0-191)

INK = tuple(int(_ink[i:i + 2], 16) for i in (0, 2, 4))
PAPER = tuple(int(_paper[i:i + 2], 16) for i in (0, 2, 4))


# ---------------------------------------------------------------- art (Monomodule, verbatim) -----------------------
class Bmp:
    def __init__(self, d):
        self.w, self.h = d["w"], d["h"]
        self.rows = [int(r, 16) for r in d["rows"]]

    def lit(self, x, y):
        return (self.rows[y] >> (63 - x)) & 1


class Font:
    def __init__(self, d):
        self.h, self.adv = d["h"], d["adv"]
        self.g = {int(k): Bmp(v) for k, v in d["glyphs"].items()}


art = json.load(open(sys.argv[1]))
F = {k: Font(v) for k, v in art["fonts"].items()}
B = art["bitmaps"]
DIAL_RING, GROUP_TIE, RING_PLAIN = Bmp(B["dialRing"]), Bmp(B["groupTie"]), Bmp(B["ringPlain"])
DIAL_DOT = [Bmp(b) for b in B["dialDot"]]
TOGGLE = [Bmp(b) for b in B["toggle"]]   # the LCD's OFF/ON toggle icon (Monomodule's randomise cells)


class Canvas:
    """1-bit LCD canvas: on = ink."""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = bytearray(w * h)

    def set(self, x, y, on=True):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y * self.w + x] = 1 if on else 0

    def blit(self, b, x, y, on=True):
        for r in range(b.h):
            row = b.rows[r]
            for c in range(b.w):
                if (row >> (63 - c)) & 1:
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

    def image(self, scale=S):
        im = Image.frombytes("L", (self.w, self.h), bytes(0 if p else 255 for p in self.px))
        im = im.resize((self.w * scale, self.h * scale), Image.NEAREST)
        return ImageOps.colorize(im, black=INK, white=PAPER)


def text_width(font, s):
    w = 0
    for ch in s:
        g = font.g.get(ord(ch))
        w += (g.w if g else font.adv) + 1
    return max(0, w - 1)


class P:
    """a knob cell (Monomodule's spec::Param, reduced to what the MD's cells use: dials, no list icons)"""

    def __init__(self, label, display="numeric", default=0, fmt=None):
        self.label, self.display, self.default = label, display if label else "blank", default
        self.tie, self.max = 0, 127
        self.fmt = fmt   # optional raw -> text, for a param whose range isn't 0-127 (as Monomodule's extra globals)


def value_text(p, raw):
    raw = max(0, min(p.max, raw))
    if p.fmt:
        return p.fmt(raw)
    if p.display == "bipolar":
        b = raw - 64
        return "+%d" % b if b > 0 else str(b)
    return str(raw)


def cell_static(cv, x0, y0, p):
    """dotted top/left border and label: the parts of a knob cell that never change."""
    cv.dots_h(x0, x0 + CW, y0)
    cv.dots_v(x0, y0, y0 + CELL - 1)
    if p.display == "blank":
        return
    cv.text_centred(F["tiny3x5"], p.label, x0 + 1, CW - 1, y0 + LABEL_Y)


def scaled(raw, mx):
    """a knob frame (0-127) -> the value of a 0..mx param at that position (the wrapper's normalisation)"""
    return int(round(raw * mx / 127.0))


def line(cv, x0, y0, x1, y1):
    n = max(abs(x1 - x0), abs(y1 - y0), 1)
    for i in range(n + 1):
        cv.set(x0 + round((x1 - x0) * i / n), y0 + round((y1 - y0) * i / n))


def draw_shape(cv, x, y, k):
    """the MD's 6 LFO shapes (captured from its LFO page: triangle, saw, square, ramp, exponential, random), 17x9"""
    import math
    if k == 0:
        line(cv, x, y + 8, x + 8, y); line(cv, x + 8, y, x + 16, y + 8)
    elif k == 1:
        line(cv, x, y + 8, x + 7, y); line(cv, x + 7, y, x + 7, y + 8); line(cv, x + 8, y + 8, x + 15, y); line(cv, x + 15, y, x + 15, y + 8)
    elif k == 2:
        line(cv, x, y + 8, x + 3, y + 8); line(cv, x + 3, y + 8, x + 3, y); line(cv, x + 3, y, x + 11, y)
        line(cv, x + 11, y, x + 11, y + 8); line(cv, x + 11, y + 8, x + 16, y + 8)
    elif k == 3:
        line(cv, x, y, x + 3, y); line(cv, x + 3, y, x + 11, y + 8); line(cv, x + 11, y + 8, x + 16, y + 8)
    elif k == 4:
        line(cv, x, y + 8, x, y)
        for c in range(1, 17):
            cv.set(x + c, y + int(round(8 * (1 - math.exp(-c / 2.5)))))
    else:
        hs = [6, 2, 5, 0, 8, 3]
        for i, hgt in enumerate(hs):
            line(cv, x + i * 3, y + hgt, x + i * 3 + 2, y + hgt)
            if i:
                line(cv, x + i * 3, y + hs[i - 1], x + i * 3, y + hgt)


def cell_dynamic(cv, x0, y0, p, raw):
    """dial plus the value row (upstream drawKnobCell minus border/label); (x0, y0) = the cell origin. The LFO page's
    other kinds, as the MD draws them: "text" (the value as text, no dial), "icon" (an LFO shape), "native" (empty:
    MPC's own value label draws the text)."""
    inner_x, inner_w = x0 + 1, CW - 1
    if p.display == "native":
        return
    if p.display == "text":
        font = F["tiny3x5"]
        cv.text_centred(font, p.fmt(raw), inner_x, inner_w, y0 + CONTENT_Y + (CONTENT_H + VALUE_H - font.h) // 2)
        return
    if p.display == "toggle":   # Monomodule's two-state cell: the toggle icon and OFF/ON
        ic = TOGGLE[1 if raw >= 64 else 0]
        cv.blit(ic, inner_x + (inner_w - ic.w) // 2, y0 + CONTENT_Y + (CONTENT_H - ic.h) // 2)
        font = F["tiny3x5"]
        cv.text_centred(font, "ON" if raw >= 64 else "OFF", inner_x, inner_w, y0 + VALUE_Y + (VALUE_H - font.h) // 2)
        return
    if p.display == "icon":
        draw_shape(cv, inner_x + (inner_w - 17) // 2, y0 + CONTENT_Y + (CONTENT_H + VALUE_H - 9) // 2, scaled(raw, 5))
        return
    rx, ry = inner_x + (inner_w - DIAL_RING.w) // 2, y0 + CONTENT_Y + (CONTENT_H - DIAL_RING.h) // 2
    cv.blit(DIAL_RING, rx, ry)
    cv.blit(DIAL_DOT[raw], rx + 2, ry + 4)
    font = F["tiny3x5"]
    bx, by, bw, bh = inner_x, y0 + VALUE_Y, CW - 1, VALUE_H
    cv.text_centred(font, value_text(p, raw), bx, bw, by + (bh - font.h) // 2)


FR_X, FR_Y, FR_W, FR_H = 1, CONTENT_Y, CW - 1, CELL - CONTENT_Y


# ------------------------------------------------------------------ output ----------------------------------------
NAME, VENDOR = "Machinedrum Module", "sd88me"
OUT = os.path.join(sys.argv[4], "%s - VST - %s" % (VENDOR, NAME))
SKIN = os.path.join(OUT, "Plugin Skins")
os.makedirs(SKIN, exist_ok=True)
for f in os.listdir(SKIN):
    os.remove(os.path.join(SKIN, f))
params = json.load(open(sys.argv[3]))["params"]
PIDX = {p["key"]: i for i, p in enumerate(params)}


def save_png(name, im):
    fn = name + ".png"
    im.save(os.path.join(SKIN, fn), optimize=True)
    return fn


def strip_image(p, cache={}):
    """128 frames of the dynamic part of a cell of this kind, stacked down."""
    k = p.display + ("_" + p.label.lower() if p.fmt and p.display != "icon" else "")
    if k in cache:
        return cache[k]
    frames = []
    for raw in range(FRAMES):
        cv = Canvas(FR_W, FR_H)
        cell_dynamic(cv, -FR_X, -FR_Y, p, raw)
        frames.append(cv.image())
    fw, fh = frames[0].size
    st = Image.new("RGB", (fw, fh * FRAMES))
    for i, f in enumerate(frames):
        st.paste(f, (0, i * fh))
    cache[k] = (save_png("st_%s" % k, st), fw, fh)
    return cache[k]


defs = {}
TABK = [[] for _ in range(TRACKS + 1)]   # components per tab: one per track, then GLOBAL
PREVIEW = []   # (image file, x, y, w, h, condition, frame index or None, tab), in draw order, for the offline composite


def knob_def(fn, w, h, orient="Vertical", interactive=True):
    """interactive=False: draws the filmstrip only; a separate touch overlay (touch_knob) owns the Q-Link binding
    (two Knob components on one Parameter confuse MPC's Q-Link handling - see Monomodule's HANDOFF/NOTES)."""
    key = "mdKnob_%s%s" % (fn[:-4], "" if interactive else "_d")
    if key not in defs:
        actions = [ss._action("Mouse Down", "Q-Link"), ss._action("Double Click", "Show Overlay", "knob overlay"),
                   ss._action("Enter Pressed", "Show Overlay", "knob overlay")] if interactive else []
        children = ([ss._focus(w, h)] if interactive else []) + [
            ss._sub("Knob", {"version": 5, "knobType": "FilmStrip", "filmStrip": fn, "numFrames": FRAMES - 1,
                             "invert": False, "dragOrientation": orient, "handleName": "Data"},
                    ss._bounds(0, 0, w, h), "Knob")]
        defs[key] = ss._local(key, actions, children)
    return key


def place(ctype, name, index, x, y, w, h, tab, focus="No", cond=None, extra=None, img=None, raw=None):
    if img:
        PREVIEW.append((img, x, y, w, h, cond, raw, tab))
    m = [{"key": "Data", "value": "Parameter %d" % index}]
    for hn, hi in (extra or {}).items():
        m.append({"key": hn, "value": "Parameter %d" % hi})
    b = ss._bounds(x, y, w, h, focus=focus, show="Show" if cond else "Hide")
    if cond:
        b["additionalInvalidatingHandles"] = [cond]
    TABK[tab].append({"version": 2, "componentData": {"version": 1, "name": name, "type": ctype, "data": {"version": 1, "handleName": "Data"}},
                      "handle remapping": {"version": 1, "map": m}, "bounds": b})


def image_comp(name, fn, x, y, w, h, tab, cond=None):
    PREVIEW.append((fn, x, y, w, h, cond, None, tab))
    b = ss._bounds(x, y, w, h, show="Show")
    if cond:
        b["additionalInvalidatingHandles"] = [cond]
    TABK[tab].append(ss._sub("Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": fn}, b, name))


def enabling(key, i, n):
    return "IndexedEnabling/%d/%d/Parameter %d" % (i, n, PIDX[key])


# ------------------------------------------------------------ page geometry (window coords -> skin px) --------------
PAGE_POS = {"SYN": (0, 0), "AMP": (1, 0), "ROUTE": (0, 1), "TRACK": (1, 1)}
PAGE_TITLE = {"SYN": "SYN", "AMP": "AMP/EFX", "ROUTE": "ROUTE", "TRACK": "LFO"}   # TRACK: the track's LFO page


def page_origin(name):
    col, row = PAGE_POS[name]
    return OX + PAGES_X0 + col * (LCD_W * S + PAGE_GAP), OY + TOP + row * (PAGE_LCD_H * S + ROW_GAP)


def page_canvas(name, cells, title=None):
    """A page's static drawing: title bar, dotted cell borders + labels, grid right/bottom edge. cells: 8 P."""
    cv = Canvas(LCD_W, PAGE_LCD_H)
    cv.fill(0, 0, LCD_W, TITLE_H, True)
    cv.text(F["bold8"], title or PAGE_TITLE[name], 2, 1, False)
    for k, p in enumerate(cells):
        cell_static(cv, (k % 4) * CW, GRID_Y + (k // 4) * CELL, p)
    cv.dots_v(LCD_W - 1, GRID_Y, PAGE_LCD_H - 1)
    cv.dots_h(0, LCD_W - 1, PAGE_LCD_H - 1)
    return cv


def px_text(im, font, s, x, y, scale, colour):
    """LCD-font text at screen resolution (any integer scale) onto an RGB image."""
    dr = ImageDraw.Draw(im)
    for ch in s:
        g = font.g.get(ord(ch))
        if not g:
            x += (font.adv + 1) * scale
            continue
        for r in range(g.h):
            for c in range(g.w):
                if g.lit(c, r):
                    dr.rectangle([x + c * scale, y + r * scale, x + (c + 1) * scale - 1, y + (r + 1) * scale - 1], fill=colour)
        x += (g.w + 1) * scale


# ------------------------------------------------------------------ machines -----------------------------------------
def parse_machines(path):
    """tools/mdmachine's listing: '%3d %-6s fn=$%06x' then the 8 SYN labels joined by single spaces (blank = empty)."""
    out = []
    for line in open(path):
        m = re.match(r"\s*(\d+) (\S+)\s+fn=\$[0-9a-f]+ (.*)$", line.rstrip("\n"))
        if not m:
            continue
        labels = (m.group(3).split(" ") + [""] * 8)[:8] if m.group(3) else [""] * 8
        name = m.group(2)
        out.append({"id": int(m.group(1)), "name": name, "group": name[:3], "short": name[3:], "labels": labels})
    return out


MACHINES = parse_machines(sys.argv[2])
# Every machine's bar and SYN grid is a component on every track page, and MPC builds all pages up front (memory and
# UI lag grow with the component count: measured +590 MB in MPC with all 135). So machines that can't sound here get none
# (a kit using one shows a blank bar and grid): MID/CTR (no audio), INP (no audio input), RAM (no sampling) and the
# ROM slots the factory image leaves empty (ROM33-48).
def drawn(m):
    return m["group"] not in ("MID", "CTR", "INP", "RAM") and not (m["group"] == "ROM" and int(m["short"]) > 32)


ALL_DRAWN = [m for m in MACHINES if drawn(m)]
DRAWN_MACHINES = [] if args.get("nomach") else ALL_DRAWN   # nomach=1: an experiment - no per-machine images at all
GROUP_TITLE = {"GND": "GND", "P-I": "P-I", "TRX": "TRX", "EFM": "EFM", "E12": "E12", "INP": "INP", "MID": "MID", "CTR": "CTR", "ROM": "ROM", "RAM": "RAM"}
BAR_H, LOGO_X, LOGO_GAP, ARROW_GAP, ARROW_W, PAD_R = 26, 4, 6, 4, 5, 4


def machine_bar(m):
    """Monomodule's machine_bar (upstream MachineBar::paint) with the group name in place of a group logo."""
    bold = F["bold8"]
    grp = GROUP_TITLE.get(m["group"], m["group"])
    logo_w = text_width(bold, grp)
    name_x = LOGO_X + logo_w + LOGO_GAP
    w = name_x + text_width(bold, m["short"]) + ARROW_GAP + ARROW_W + PAD_R
    cv = Canvas(w, BAR_H)
    cv.fill(0, 0, w, BAR_H, True)
    cv.text(bold, grp, LOGO_X, (BAR_H - bold.h) // 2, False)
    cv.text(bold, m["short"], name_x, (BAR_H - bold.h) // 2, False)
    ax, ay = name_x + text_width(bold, m["short"]) + ARROW_GAP, BAR_H // 2 - 1
    for r in range(3):
        half = 2 - r
        for c in range(2 - half, 2 + half + 1):
            cv.set(ax + c, ay + r, False)
    return cv.image()


PREVIEW_MACHINE = int(args.get("machine", "16"))   # TRXBD
# ------------------------------------------------------------------ cells ---------------------------------------------
AMP_CELLS = [P("AMD"), P("AMF"), P("EQF", default=64), P("EQG", "bipolar", 64), P("FLTF"), P("FLTW", default=127), P("FLTQ"), P("SRR")]
AMP_KEYS = ["amd", "amf", "eqf", "eqg", "fltf", "fltw", "fltq", "srr"]
ROUTE_CELLS = [P("DIST"), P("VOL", default=100), P("PAN", "bipolar", 64), P("DEL"), P("REV"), P("LFOS"), P("LFOD"), P("LFOM")]
ROUTE_KEYS = ["dist", "vol", "pan", "del", "rev", "lfos", "lfod", "lfom"]
BLANK8 = [P("") for _ in range(8)]
TRACK_NAMES = ["%02d-%s" % (i + 1, n) for i, n in enumerate("BD SD HT MT LT CP RS CB CH OH RC CC M1 M2 M3 M4".split())]
LFO_CELLS = [P("TRACK", "text", fmt=lambda raw: TRACK_NAMES[scaled(raw, 15)]), P("PARAM", "native"),
             P("SHP1", "icon", fmt=True), P("SHP2", "icon", fmt=True),
             P("UPDTE", "text", fmt=lambda raw: ("FREE", "TRIG", "HOLD")[scaled(raw, 2)]), P("SPEED"), P("DEPTH"), P("SHMIX")]
LFO_KEYS = ["lfo_track", "lfo_param", "lfo_shp1", "lfo_shp2", "lfo_type", "lfos", "lfod", "lfom"]


def syn_cells(m):
    return [P(l) for l in m["labels"]]


default_machine = next(m for m in MACHINES if m["id"] == PREVIEW_MACHINE)

# ------------------------------------------------------------------ chassis -------------------------------------------
# The hardware's face (after a photo of the MD): a brushed-aluminium faceplate, a thin glossy black bezel around the edge,
# and one big LCD inside it holding everything - the machine bar, the nameplate and the pages - with the backlight's
# darker edges. Components only ever sit well inside it, where it's flat PAPER, so the strips' opaque backgrounds match.
LCD_MARGIN_X = int(args.get("lcd_margin", 28))  # the LCD's left/right edge sits this far outside the page panels
ALU_V, BEZEL_W = 8, 12                          # faceplate border (top/bottom) and bezel width (skin px)
ALU_H = OX + PAGES_X0 - LCD_MARGIN_X - BEZEL_W  # faceplate border (left/right): whatever leaves the LCD just around the pages
LCD_RECT = (ALU_H + BEZEL_W, ALU_V + BEZEL_W, SKIN_W - ALU_H - BEZEL_W, SKIN_H - ALU_V - BEZEL_W)
LCD_PAD = 6                                     # the open picker also covers this much around the pages
LCD_EDGE = 8                                    # the backlight's darkening towards the LCD's edge
BEZEL = (6, 5, 6)


def chassis():
    import random
    from PIL import ImageFilter
    rnd = random.Random(7)
    # brushed aluminium: horizontal streaks (noise stretched along x) over a soft vertical gradient
    noise = Image.new("L", (SKIN_W // 16, SKIN_H))
    noise.putdata([rnd.randint(0, 255) for _ in range(noise.size[0] * noise.size[1])])
    noise = noise.resize((SKIN_W, SKIN_H), Image.BILINEAR).filter(ImageFilter.GaussianBlur(0.6))
    im = Image.new("RGB", (SKIN_W, SKIN_H))
    px, nz = im.load(), noise.load()
    for y in range(SKIN_H):
        base = 182 - 22 * y / SKIN_H
        for x in range(SKIN_W):
            v = int(base + (nz[x, y] - 128) * 0.09)
            px[x, y] = (v, v, v - 2)
    dr = ImageDraw.Draw(im)
    # the bezel: glossy black, a faint highlight towards the top right, a bevelled outer edge
    x0, y0, x1, y1 = LCD_RECT
    bx0, by0, bx1, by1 = x0 - BEZEL_W, y0 - BEZEL_W, x1 + BEZEL_W, y1 + BEZEL_W
    mask = Image.new("L", (SKIN_W, SKIN_H), 0)
    ImageDraw.Draw(mask).rounded_rectangle([bx0, by0, bx1, by1], radius=12, fill=255)
    bez = Image.new("RGB", (SKIN_W, SKIN_H), BEZEL)
    bp = bez.load()
    for y in range(by0, by1 + 1):
        for x in range(bx0, bx1 + 1):
            g = max(0.0, (x - bx0) / (bx1 - bx0) - (y - by0) / (by1 - by0) * 0.9 - 0.25)   # diagonal sheen
            a = int(40 * g)
            bp[x, y] = (BEZEL[0] + a, BEZEL[1] + a, BEZEL[2] + a + 1)
    im.paste(bez, (0, 0), mask)
    dr.rounded_rectangle([bx0, by0, bx1, by1], radius=12, outline=(80, 80, 82), width=1)
    dr.rounded_rectangle([bx0 + 1, by0 + 1, bx1 - 1, by1 - 1], radius=11, outline=(24, 24, 26), width=1)
    # the LCD: a dark rim, the backlight darkening over its last few pixels, flat inside
    dr.rectangle([x0 - 1, y0 - 1, x1, y1], fill=(0, 0, 0))
    edge = tuple(int(c * 0.80) for c in PAPER)
    for d in range(LCD_EDGE):
        f = d / LCD_EDGE
        dr.rectangle([x0 + d, y0 + d, x1 - 1 - d, y1 - 1 - d], outline=tuple(int(e + (p_ - e) * f) for e, p_ in zip(edge, PAPER)))
    dr.rectangle([x0 + LCD_EDGE, y0 + LCD_EDGE, x1 - 1 - LCD_EDGE, y1 - 1 - LCD_EDGE], fill=PAPER)
    return im


TRACK_CHASSIS = chassis()

# ---------------------------------------------------------- kit picker (GLOBAL tab, KITS page) -----------------------
# Monomodule's BANK / PRESET strip (upstream PresetStrip minus the library parts), as BANK / KIT: PREV, the name (live
# text: the engine's bank_name / kit_name), NEXT - on the GLOBAL tab's spare quadrant, as a KITS page.
def frame_box(cv, x, y, w, h, on=True):
    cv.fill(x, y, w, 1, on); cv.fill(x, y + h - 1, w, 1, on); cv.fill(x, y, 1, h, on); cv.fill(x + w - 1, y, 1, h, on)


def arrow_h(cv, cx, cy, left, on):
    for c in range(4):
        x = cx - 2 + c
        h = 2 * c + 1 if left else 7 - 2 * c
        cv.fill(x, cy - h // 2, 1, h, on)


STRIP_H, STRIP_GAP, STRIP_W, ARROW_W = 15, 4, LCD_W - 8, 12   # LCD px
kits_x, kits_y = page_origin("TRACK")
strip_x = kits_x + 4 * S
strip_y0 = kits_y + (GRID_Y + 6) * S
row_w = STRIP_W - 2 * (ARROW_W - 1)
KIT_ROWS = []
KITS_PAGE = Canvas(LCD_W, PAGE_LCD_H)   # its title bar and outline (the rows are pasted in by build_row)
KITS_PAGE.fill(0, 0, LCD_W, TITLE_H, True)
KITS_PAGE.text(F["bold8"], "KITS", 2, 1, False)
KITS_PAGE.dots_h(0, LCD_W - 1, GRID_Y); KITS_PAGE.dots_h(0, LCD_W - 1, PAGE_LCD_H - 1)
KITS_PAGE.dots_v(0, GRID_Y, PAGE_LCD_H - 1); KITS_PAGE.dots_v(LCD_W - 1, GRID_Y, PAGE_LCD_H - 1)
KITS_IMG = KITS_PAGE.image()


def build_row(label, y, prev_key, next_key, name_key):
    prev_r, mid_r, next_r = (0, ARROW_W), (ARROW_W - 1, row_w), (ARROW_W - 1 + row_w - 1, ARROW_W)
    row = Canvas(STRIP_W, STRIP_H)
    for (rx, rw), left in ((prev_r, True), (next_r, False)):
        frame_box(row, rx, 0, rw, STRIP_H)
        arrow_h(row, rx + rw // 2, STRIP_H // 2, left, True)
    frame_box(row, mid_r[0], 0, mid_r[1], STRIP_H)
    row.text(F["tiny3x5"], label, mid_r[0] + 4, (STRIP_H - F["tiny3x5"].h) // 2)
    row_img = row.image()
    KITS_IMG.paste(row_img, (strip_x - kits_x, y - kits_y))
    name_x = mid_r[0] + 4 + text_width(F["tiny3x5"], label) + 4
    name_w = mid_r[0] + mid_r[1] - 4 - name_x
    for key_, (rx, rw), left, pk_ in (("md%sPrev" % label, prev_r, True, prev_key), ("md%sNext" % label, next_r, False, next_key)):
        off = row_img.crop((rx * S, 0, (rx + rw) * S, STRIP_H * S))
        onc = Canvas(rw, STRIP_H)
        onc.fill(0, 0, rw, STRIP_H, True)
        arrow_h(onc, rw // 2, STRIP_H // 2, left, False)
        fo, fn_ = save_png(key_ + "_off", off), save_png(key_ + "_on", onc.image())
        defs[key_] = ss._local(key_, [ss._action("Mouse Down", "Q-Link"), ss._action("Enter Pressed", "Toggle Switch")],
                               [ss._focus(rw * S, STRIP_H * S), ss._button(fn_, fo, 1, 1, rw * S, STRIP_H * S)])
        KIT_ROWS.append((key_, pk_, strip_x + rx * S, y, rw * S, STRIP_H * S, fo))
    nkey = "md%sName" % label
    defs[nkey] = ss._local(nkey, [], [ss._value_label(0, 0, name_w * S, (STRIP_H - 2) * S, 25.0, "%02x%02x%02x" % INK, "left verticallyCentred")])
    KIT_ROWS.append((nkey, name_key, strip_x + name_x * S, y + S, name_w * S, (STRIP_H - 2) * S, None))


build_row("BANK", strip_y0, "bank_prev", "bank_next", "bank_name")
build_row("KIT", strip_y0 + (STRIP_H + STRIP_GAP) * S, "kit_prev", "kit_next", "kit_name")
BAR_W_MAX = max(machine_bar(m).size[0] for m in MACHINES)   # the machine picker's tap field: the widest bar
# the nameplate, on the LCD in its own bold font, right-aligned with the pages, level with the machine bar
_np = "MACHINEDRUM MODULE"
px_text(TRACK_CHASSIS, F["bold8"], _np, OX + PAGES_X0 + PAGES_W - text_width(F["bold8"], _np) * 3,
        OY + 8 + (BAR_ROWS * S - F["bold8"].h * 3) // 2, 3, INK)

# backgrounds: one per track (the TRACK quadrant's title names it); SYN uses the default machine's labels here, the
# per-machine overlay repaints its grid
EMPTY_MACHINE = next(m for m in MACHINES if m["id"] == 0)   # GND--: a new track's machine
bgs = []
for t in range(TRACKS):
    b_ = TRACK_CHASSIS.copy()
    # the 4th quadrant: the track's LFO page (the MD's own, FUNCTION + SYN/EFX/ROUTE)
    for name, cells in (("SYN", syn_cells(EMPTY_MACHINE)), ("AMP", AMP_CELLS), ("ROUTE", ROUTE_CELLS), ("TRACK", LFO_CELLS)):
        cv = page_canvas(name, cells, "LFO" if name == "TRACK" else None)
        b_.paste(cv.image(), page_origin(name))
    bgs.append(b_)
bg_track = save_png("bg_track", bgs[0])   # every track's background is the same now (the LFO title doesn't name the track)
for t in range(TRACKS):
    image_comp("Background", bg_track, 0, 0, SKIN_W, SKIN_H, tab=t)

BAR_X, BAR_Y = OX + BAR_X_W, OY + 8

# knob cells
TOUCH_INSET = 2
TOUCH_W, TOUCH_H = (CW - 2 * TOUCH_INSET) * S, (CELL - 2 * TOUCH_INSET) * S
_touch_png = []


def touch_file():
    if not _touch_png:
        _touch_png.append(save_png("touch", Image.new("RGBA", (8, 8 * FRAMES), (0, 0, 0, 0))))
    return _touch_png[0]


def touch_knob(name, index, x0, y0, tab):
    kd = knob_def(touch_file(), TOUCH_W, TOUCH_H)
    place(kd, "%s touch" % name, index, x0 + TOUCH_INSET * S, y0 + TOUCH_INSET * S, TOUCH_W, TOUCH_H, tab)


def cell_dials(page, cells, keys, t):
    """the dial strips of a page's live cells (display only); the preview shows each param's declared default"""
    x0, y0 = page_origin(page)
    for k, p in enumerate(cells):
        if p.display == "blank":
            continue
        fn, fw, fh = strip_image(p)
        kx = x0 + ((k % 4) * CW + FR_X) * S
        ky = y0 + (GRID_Y + (k // 4) * CELL + FR_Y) * S
        pd_ = params[PIDX[keys[k]]]
        raw = int(round((pd_["default"] - pd_["min"]) * 127.0 / (pd_["max"] - pd_["min"]))) if pd_["max"] > pd_["min"] else 0
        place(knob_def(fn, fw, fh, interactive=False), "%s %s" % (page, p.label or k), PIDX[keys[k]], kx, ky, fw, fh, t, img=fn, raw=raw)


def cell_touch(page, cells, keys, t):
    x0, y0 = page_origin(page)
    for k, p in enumerate(cells):
        if p.display != "blank":
            touch_knob("%s %d" % (page, k + 1), PIDX[keys[k]], x0 + (k % 4) * CW * S, y0 + (GRID_Y + (k // 4) * CELL) * S, t)


# SYN: every dial numeric; the per-machine overlay (below) blanks the cells a machine doesn't use
SYN_ANY = [P("SYN%d" % (k + 1)) for k in range(8)]


def syn_overlay(m, cache={}):
    """The SYN grid for a machine (labels, borders), transparent over its live cells' dial areas so the dials under it
    show; opaque over unused cells, which hides their dials. Shared by every machine with the same label set."""
    key = tuple(m["labels"])
    if key in cache:
        return cache[key]
    cells = syn_cells(m)
    cv = page_canvas("SYN", cells)
    im = cv.image().crop((0, GRID_Y * S, LCD_W * S, PAGE_LCD_H * S)).convert("RGBA")
    dr = ImageDraw.Draw(im)
    for k, p in enumerate(cells):
        if p.display == "blank":
            continue
        x = ((k % 4) * CW + FR_X) * S
        y = ((k // 4) * CELL + FR_Y) * S
        dr.rectangle([x, y, x + FR_W * S - 1, y + FR_H * S - 1], fill=(0, 0, 0, 0))
    cache[key] = save_png("syn_%02d" % len(cache), im)
    return cache[key]


defs["mdLfoParamText"] = ss._local("mdLfoParamText", [], [ss._value_label(0, 0, FR_W * S, (CONTENT_H + VALUE_H - 2) * S, 26.0,
                                                                         "%02x%02x%02x" % INK)])
for t in range(TRACKS):
    tk = lambda k: "track%d_%s" % (t, k)
    syn_keys = [tk("syn%d" % (k + 1)) for k in range(8)]
    cell_dials("SYN", SYN_ANY, syn_keys, t)
    sx, sy = page_origin("SYN")
    for m in DRAWN_MACHINES:
        image_comp("SYN grid %s" % m["name"], syn_overlay(m), sx, sy + GRID_Y * S, LCD_W * S, (PAGE_LCD_H - GRID_Y) * S, t,
                   cond=enabling(tk("machine"), m["id"], NMACH))
    cell_touch("SYN", SYN_ANY, syn_keys, t)
    cell_dials("AMP", AMP_CELLS, [tk(k) for k in AMP_KEYS], t)
    cell_touch("AMP", AMP_CELLS, [tk(k) for k in AMP_KEYS], t)
    cell_dials("ROUTE", ROUTE_CELLS, [tk(k) for k in ROUTE_KEYS], t)
    cell_touch("ROUTE", ROUTE_CELLS, [tk(k) for k in ROUTE_KEYS], t)
    cell_dials("TRACK", LFO_CELLS, [tk(k) for k in LFO_KEYS], t)
    lx, ly = page_origin("TRACK")   # PARAM's text: MPC's own value label (the destination's live name, "dynamic_display")
    place("mdLfoParamText", "LFO PARAM text", PIDX[tk("lfo_param")], lx + (CW + FR_X) * S, ly + (GRID_Y + CONTENT_Y) * S,
          FR_W * S, (CONTENT_H + VALUE_H - 2) * S, t)
    cell_touch("TRACK", LFO_CELLS, [tk(k) for k in LFO_KEYS], t)

    # machine bar, one image per machine, shown while it's the track's machine
    for m in DRAWN_MACHINES:
        fn = "mb_%d.png" % m["id"]
        if not os.path.exists(os.path.join(SKIN, fn)):
            save_png("mb_%d" % m["id"], machine_bar(m))
        w_, h_ = Image.open(os.path.join(SKIN, fn)).size
        image_comp("Machine %s" % m["name"], fn, BAR_X, BAR_Y, w_, h_, t, cond=enabling(tk("machine"), m["id"], NMACH))

fn_t = save_png("touch_lev", Image.new("RGBA", (8, 8 * FRAMES), (0, 0, 0, 0)))   # an invisible strip for touch columns

# machine picker: field over the machine bar toggles machine__open; panel + one image button per machine
pk_x, pk_y, pk_w, pk_h = OX + PAGES_X0, OY + TOP, PAGES_W, WIN_H - 8 - TOP
# over the page windows' margins too, so the bezel between them doesn't show through the open picker
pk_x, pk_y, pk_w, pk_h = pk_x - LCD_PAD, pk_y - LCD_PAD, pk_w + 2 * LCD_PAD, pk_h + 2 * LCD_PAD
COL_ROWS = 16
# not offered: MID/CTR (no audio - MIDI/control machines), INP (no audio input into this instrument) and RAM (no sampling
# here). Their bars stay, so a kit that uses one still shows it.
PICK_MACHINES = ALL_DRAWN
cols = []
for m in PICK_MACHINES:
    col = next((c for c in cols if c["group"] == m["group"] and len(c["ms"]) < COL_ROWS), None)
    if not col:
        col = {"group": m["group"], "ms": []}
        cols.append(col)
    col["ms"].append(m)
cols.sort(key=lambda c: min(x["id"] for x in c["ms"]) if c["group"] != "ROM" else 1000 + min(x["id"] for x in c["ms"]))
COL_GAP, BORDER, HEADER_H, PAD, NAME_Y = 6, 3, 36, 6, 6
col_w = (pk_w - (len(cols) - 1) * COL_GAP) // len(cols)
ROW_H = (pk_h - 2 * BORDER - HEADER_H - 6) // COL_ROWS
panel = Image.new("RGB", (pk_w, pk_h), PAPER)
pd = ImageDraw.Draw(panel)


def dotted_h(dr, x0, x1, y):
    for x in range(x0, x1, 4):
        dr.rectangle([x, y, x + 1, y + 1], fill=INK)


rows = {}
x = 0
for col in cols:
    cx0 = x
    x += col_w + COL_GAP
    pd.rectangle([cx0, 0, cx0 + col_w - 1, pk_h - 1], outline=INK, width=BORDER)
    hx, hy, hw = cx0 + BORDER, BORDER, col_w - 2 * BORDER
    pd.rectangle([hx, hy, hx + hw - 1, hy + HEADER_H - 1], fill=INK)
    title = GROUP_TITLE.get(col["group"], col["group"])
    tw = text_width(F["bold8"], title) * S
    px_text(panel, F["bold8"], title, hx + hw // 2 - tw // 2, hy + HEADER_H // 2 - F["bold8"].h * S // 2, S, PAPER)
    ry = hy + HEADER_H + 6
    dotted_h(pd, hx, hx + hw, ry - 1)
    for i, m in enumerate(col["ms"]):
        rows[m["id"]] = (hx, ry + i * ROW_H, hw, ROW_H)
        dotted_h(pd, hx, hx + hw, ry + (i + 1) * ROW_H - 1)
fn_panel = save_png("pk_panel", panel)
pick_imgs = {}
for m in PICK_MACHINES:
    rx, ry, rw, rh = rows[m["id"]]
    imgs = {}
    for state in ("on", "off"):
        im = Image.new("RGB", (rw, rh), INK if state == "on" else PAPER)
        px_text(im, F["bold8"], m["short"], PAD, (rh - F["bold8"].h * 2) // 2, 2, PAPER if state == "on" else INK)
        imgs[state] = save_png("pk_%d_%s" % (m["id"], state), im)
    pick_imgs[m["id"]] = imgs
    key = "mdPickOpt_%d" % m["id"]
    defs[key] = ss._local(key, [ss._action("Mouse Down", "Q-Link")], [ss._button(imgs["on"], imgs["off"], m["id"], NMACH, rw, rh)])
pk = "mdPickPanel"
defs[pk] = ss._local(pk, [], [ss._sub("Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": fn_panel}, ss._bounds(0, 0, pk_w, pk_h), "Image")])
key = "mdPickField"
defs[key] = ss._local(key, [ss._action("Mouse Down", "Toggle Switch"), ss._action("Enter Pressed", "Toggle Switch")], [ss._focus(BAR_W_MAX, BAR_ROWS * S)])
for t in range(TRACKS):   # last, so the picker draws over everything on the tab
    mk, ok = "track%d_machine" % t, "track%d_machine__open" % t
    place("mdPickField", "Machine picker", PIDX[ok], BAR_X, BAR_Y, BAR_W_MAX, BAR_ROWS * S, t, focus="Yes", extra={"Text": PIDX[mk]})
    open_c = enabling(ok, 1, 2)
    place(pk, "Machine list", PIDX[ok], pk_x, pk_y, pk_w, pk_h, t, cond=open_c, img=fn_panel)
    for m in PICK_MACHINES:
        rx, ry, rw, rh = rows[m["id"]]
        place("mdPickOpt_%d" % m["id"], "Machine %s" % m["name"], PIDX[mk], pk_x + rx, pk_y + ry, rw, rh, t, cond=open_c,
              img=pick_imgs[m["id"]]["on" if m["id"] == PREVIEW_MACHINE else "off"])

# ------------------------------------------------------------------ GLOBAL tab ----------------------------------------
# MIXER (the top row, both pages wide): every track's LEV as Monomodule's LEV column, 16 across. GLOBAL (bottom left):
# VOICES (max_voices, 1-16). Tempo follows MPC's own (vst.json HAS_LFO_BPM), so it has no control here.
GT = TRACKS
MW = PAGES_W // S                      # the mixer page's width in LCD px
MCW = (MW - 1) // TRACKS               # a channel column
mcv = Canvas(MW, PAGE_LCD_H)
mcv.fill(0, 0, MW, TITLE_H, True)
mcv.text(F["bold8"], "MIXER", 2, 1, False)
mfx, mfy, mfw = (MCW - 11) // 2, GRID_Y + 9, 11     # each channel's dotted bar frame
mfh = PAGE_LCD_H - 2 - mfy
for c in range(TRACKS):
    x0 = c * MCW
    mcv.dots_v(x0, GRID_Y, PAGE_LCD_H - 1)
    mcv.text_centred(F["tiny3x5"], str(c + 1), x0 + 1, MCW - 1, GRID_Y + LABEL_Y)
    mcv.dots_h(x0 + mfx, x0 + mfx + mfw - 1, mfy); mcv.dots_h(x0 + mfx, x0 + mfx + mfw - 1, mfy + mfh - 1)
    mcv.dots_v(x0 + mfx, mfy, mfy + mfh - 1); mcv.dots_v(x0 + mfx + mfw - 1, mfy, mfy + mfh - 1)
mcv.dots_h(0, MW - 1, GRID_Y); mcv.dots_v(TRACKS * MCW, GRID_Y, PAGE_LCD_H - 1); mcv.dots_h(0, MW - 1, PAGE_LCD_H - 1)
mx0, my0 = page_origin("SYN")
gbg = TRACK_CHASSIS.copy()
gbg.paste(mcv.image(), (mx0, my0))
GLOBAL_CELLS = [P("VOICES", default=25, fmt=lambda raw: str(1 + int(round(raw * 15 / 127.0)))),
                P("ROM", "toggle", default=0), P(""), P(""),
                P("RND ALL", "toggle"), P("RND 1-8", "toggle"), P("RND 9-16", "toggle"), P("RND KIT", "toggle")]
# top row: VOICES and ROM (the settings); bottom row: the four randomise toggles
GLOBAL_KEYS = ["max_voices", "rom_enabled", None, None, "randomize_all", "randomize_1_8", "randomize_9_16", "randomize_kit"]
gcv = page_canvas("ROUTE", GLOBAL_CELLS, "GLOBAL")
gbg.paste(gcv.image(), page_origin("ROUTE"))
gbg.paste(KITS_IMG, (kits_x, kits_y))
image_comp("Background", save_png("bg_global", gbg), 0, 0, SKIN_W, SKIN_H, tab=GT)
for key_, pk_, x_, y_, w_, h_, img_ in KIT_ROWS:   # after the background: later components draw (and take clicks) on top
    place(key_, pk_, PIDX[pk_], x_, y_, w_, h_, GT, img=img_)
# the bars: Monomodule's LEV strips (bar rows 1..7 of a 9-wide strip), in segments to keep each image short
m_inner_y0, m_seg_n = mfy + 2, 3
m_seg_h = (mfh - 4) // m_seg_n
m_inner_h = m_seg_h * m_seg_n
mseg = []
for sgm in range(m_seg_n):
    st = Image.new("RGB", ((mfw - 2) * S, m_seg_h * S * FRAMES))
    for raw in range(FRAMES):
        cv = Canvas(mfw - 2, m_seg_h)
        lvl = int(round(raw / 127.0 * m_inner_h))
        for r in range(m_seg_h):
            if m_inner_y0 + sgm * m_seg_h + r >= m_inner_y0 + m_inner_h - lvl:
                for cc in range(1, mfw - 3):
                    cv.set(cc, r)
        st.paste(cv.image(), (0, raw * m_seg_h * S))
    mseg.append(save_png("mix_%d" % sgm, st))
mt_w, mt_h = (MCW - 2) * S, (PAGE_LCD_H - GRID_Y - 3) * S
for c in range(TRACKS):
    key = "track%d_level" % c
    for sgm, fn in enumerate(mseg):
        place(knob_def(fn, (mfw - 2) * S, m_seg_h * S, interactive=False), "MIX %d %d" % (c + 1, sgm + 1), PIDX[key],
              mx0 + (c * MCW + mfx + 1) * S, my0 + (m_inner_y0 + sgm * m_seg_h) * S, (mfw - 2) * S, m_seg_h * S, GT, img=fn, raw=100)
    place(knob_def(fn_t, mt_w, mt_h), "MIX %d touch" % (c + 1), PIDX[key], mx0 + (c * MCW + 1) * S, my0 + (GRID_Y + 1) * S, mt_w, mt_h, GT)
cell_dials("ROUTE", GLOBAL_CELLS[:1], GLOBAL_KEYS[:1], GT)
cell_touch("ROUTE", GLOBAL_CELLS[:1], GLOBAL_KEYS[:1], GT)
# the randomise toggles: Monomodule's - a display-only two-state button (OFF/ON cell images) and a transparent button
# over the whole cell that owns the tap (the params are momentary: the wrapper holds ON 1.5 s, then springs back)
save_png("clear", Image.new("RGBA", (8, 8), (0, 0, 0, 0)))
tkey = "mdToggleTouch"
defs[tkey] = ss._local(tkey, [ss._action("Mouse Down", "Q-Link"), ss._action("Enter Pressed", "Toggle Switch")],
                       [ss._focus(TOUCH_W, TOUCH_H), ss._button("clear.png", "clear.png", 1, 1, TOUCH_W, TOUCH_H)])
gx0, gy0 = page_origin("ROUTE")
for k_ in (1, 4, 5, 6, 7):   # ROM on/off (the only one that stays set), then the four randomise toggles
    p_, key_p = GLOBAL_CELLS[k_], GLOBAL_KEYS[k_]
    imgs = {}
    for state, raw in (("on", 127), ("off", 0)):
        cv = Canvas(FR_W, FR_H)
        cell_dynamic(cv, -FR_X, -FR_Y, p_, raw)
        imgs[state] = save_png("tog_%s_%s" % (key_p, state), cv.image())
    key = "mdToggle_%s" % key_p
    defs[key] = ss._local(key, [], [ss._button(imgs["on"], imgs["off"], 1, 1, FR_W * S, FR_H * S)])
    place(key, p_.label, PIDX[key_p], gx0 + ((k_ % 4) * CW + FR_X) * S, gy0 + (GRID_Y + (k_ // 4) * CELL + FR_Y) * S, FR_W * S, FR_H * S,
          GT, img=imgs["off"])
    place(tkey, "%s touch" % p_.label, PIDX[key_p], gx0 + ((k_ % 4) * CW + TOUCH_INSET) * S, gy0 + (GRID_Y + (k_ // 4) * CELL + TOUCH_INSET) * S,
          TOUCH_W, TOUCH_H, GT)

# ------------------------------------------------------------------ assemble ----------------------------------------
pages, qmap = [], []
comp_bg = {"version": 1, "colour": "ff%02x%02x%02x" % PAPER, "image": ""}
gsets = [("MIXER", ["track%d_level" % c for c in range(TRACKS)]),
         ("GLOBAL", GLOBAL_KEYS)]   # Q-Links in the same places as the cells
for sp, (title, keys) in enumerate(gsets):   # GLOBAL is the first tab
    ql = {"Q-Link %d" % (q + 1): -1 for q in range(16)}
    for s_, k in enumerate(keys):
        if k:
            ql["Q-Link %d" % ss.qlink_for_slot(s_)] = PIDX[k]
    pages.append({"version": 3, "tabName": "GLOBAL", "fnKeyIndex": 0, "fnKeySubIndex": sp, "qlinkBoundsData": ["0 0 0 0"],
                  "componentName": "MACHINEDRUM|GLOBAL", "initialSize": "0 0 %d %d" % (SKIN_W, SKIN_H), "scale": 1.0})
    qmap.append({"Tab": 1, "SubTab": sp + 1, "Bank Direction": "Column", "Q-Links": ql})
defs["MACHINEDRUM|GLOBAL"] = {"key": "MACHINEDRUM|GLOBAL", "value": {"version": 4, "actions": [], "backgroundData": {"version": 1, "focussed": comp_bg, "unfocussed": comp_bg},
                                                                  "ignoreMousePresses": False, "disableCoarseDataWheel": False, "repeats": 1,
                                                                  "hideQLinkBounds": True, "componentsData": TABK[GT]}}
for t in range(TRACKS):
    tk = lambda k: "track%d_%s" % (t, k)
    sets = [("T%d SYN / EFX" % (t + 1), [tk("syn%d" % (k + 1)) for k in range(8)] + [tk(k) for k in AMP_KEYS]),
            ("T%d ROUTE / LFO" % (t + 1), [tk(k) for k in ROUTE_KEYS] + [tk(k) for k in LFO_KEYS[:5]] + [tk("level"), tk("machine")])]
    comp = "MACHINEDRUM|TRACK %d" % (t + 1)
    for sp, (title, keys) in enumerate(sets):
        ql = {"Q-Link %d" % (q + 1): -1 for q in range(16)}
        for s_, k in enumerate(keys):
            ql["Q-Link %d" % ss.qlink_for_slot(s_)] = PIDX[k]
        pages.append({"version": 3, "tabName": "TRACK %d" % (t + 1), "fnKeyIndex": t + 1, "fnKeySubIndex": sp, "qlinkBoundsData": ["0 0 0 0"],
                      "componentName": comp, "initialSize": "0 0 %d %d" % (SKIN_W, SKIN_H), "scale": 1.0})
        qmap.append({"Tab": t + 2, "SubTab": sp + 1, "Bank Direction": "Column", "Q-Links": ql})
    defs[comp] = {"key": comp, "value": {"version": 4, "actions": [], "backgroundData": {"version": 1, "focussed": comp_bg, "unfocussed": comp_bg},
                                         "ignoreMousePresses": False, "disableCoarseDataWheel": False, "repeats": 1,
                                         "hideQLinkBounds": True, "componentsData": TABK[t]}}
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
size = sum(os.path.getsize(os.path.join(SKIN, f)) for f in os.listdir(SKIN))
print("skin: %s (%d files, %.1f MB, %d components per tab)" % (OUT, len(os.listdir(SKIN)), size / 1e6, len(TABK[0])))


def preview(state, out, tab=0):
    """Composite of the skin for one state {param key: value}: what MPC would draw, at the given values."""
    im = Image.new("RGB", (SKIN_W, SKIN_H), PAPER)
    for fn, x, y, w, h, cond, raw, ptab in PREVIEW:
        if ptab is not None and ptab != tab:
            continue
        if cond:
            m = re.match(r"IndexedEnabling/(\d+)/(\d+)/Parameter (\d+)", cond)
            if state.get(params[int(m.group(3))]["key"], 0) != int(m.group(1)):
                continue
        src = Image.open(os.path.join(SKIN, fn)).convert("RGBA")
        if raw is not None:
            fh = src.size[1] // FRAMES
            src = src.crop((0, raw * fh, src.size[0], (raw + 1) * fh))
        im.paste(src, (x, y), src)
    im.save(out)


preview({"track0_machine": PREVIEW_MACHINE}, os.path.join(sys.argv[4], "preview_track1.png"), 0)
preview({"track1_machine": 20}, os.path.join(sys.argv[4], "preview_track2_rs.png"), 1)
preview({}, os.path.join(sys.argv[4], "preview_track3_new.png"), 2)
preview({}, os.path.join(sys.argv[4], "preview_global.png"), GT)
preview({"track0_machine": PREVIEW_MACHINE, "track0_machine__open": 1}, os.path.join(sys.argv[4], "preview_open.png"), 0)
