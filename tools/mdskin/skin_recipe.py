#!/usr/bin/env python3
"""The skin as a recipe without Elektron's art in it, for the Gen2 build that is released as a download: every skin pixel is either our
own (the same whatever the art) or one bit of the LCD art (fonts, dials) that tools/mdskin/mdartdump reads from the user's own OS file.
The plugin fills the recipe in on the device from the user's OS (src: vst/skin_recipe.h), so the skin it writes is the one mk_skin.py /
mk_tap_skin.py draw, pixel for pixel, and the download carries no art.

How the map is found: the art is walked in a fixed order (art_bits()), giving every bit an index. The generators are run on probe art with
the real glyph/bitmap sizes (so nothing moves) and the bits set by pattern: all off, all on, and once per binary digit of the index. A
pixel that changes follows one bit: off/on give its two colours and the digit runs spell the index. Then the recipe is checked: filled in
with the real art and with random art it must give the generators' own output exactly (any pixel that depends on more than one bit, or
on art any other way, fails it).

  skin_recipe.py make   <art.json> <machines.txt> <out dir>     (run in the skin build's Python image, with Pillow; see build_skin.sh)

<out dir>/recipe/: files/<skin dir>/... (every file whose content does not depend on the art, as is) and art.bin (the rest, see write_bin).
"""
import io
import json
import os
import random
import shutil
import struct
import subprocess
import sys
import zlib

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
# the two skins, as build_skin.sh makes them: (generator, its arguments after art.json, the output dir relative to <run dir>)
SKINS = [("mk_skin.py", ["{machines}", "vst/params.json"], "main", ["rom=0"]),
         ("mk_tap_skin.py", ["vst/tapfx/params.json"], "tapfx", ["fx"])]


def art_bits(art):
    """Every art bitmap (a dict with w, h, rows) in a fixed order: fonts by name, glyphs by code, then bitmaps by name (lists in order)."""
    out = []

    def walk(v):
        if isinstance(v, dict) and "rows" in v and "w" in v:
            out.append(v)
        elif isinstance(v, list):
            for x in v:
                walk(x)
        elif isinstance(v, dict):
            for k in sorted(v):
                walk(v[k])
    for name in sorted(art["fonts"]):
        g = art["fonts"][name]["glyphs"]
        for code in sorted(g, key=int):
            walk(g[code])
    for name in sorted(art["bitmaps"]):
        walk(art["bitmaps"][name])
    return out


def set_bits(art, fn):
    """A copy of art whose bit n (in art_bits order, row-major in each bitmap) is fn(n)."""
    a = json.loads(json.dumps(art))
    n = 0
    for b in art_bits(a):
        rows = []
        for y in range(b["h"]):
            r = 0
            for x in range(b["w"]):
                if fn(n):
                    r |= 1 << (63 - x)
                n += 1
            rows.append("%016x" % r)
        rows += b["rows"][b["h"]:]
        b["rows"] = rows
    return a, n


def get_bits(art):
    bits = []
    for b in art_bits(art):
        for y in range(b["h"]):
            r = int(b["rows"][y], 16)
            for x in range(b["w"]):
                bits.append((r >> (63 - x)) & 1)
    return bits


def run_generators(art, machines, out):
    """The skins from this art, into out/<skin>/ (as build_skin.sh runs them)."""
    os.makedirs(out, exist_ok=True)
    ap = os.path.join(out, "art.json")
    json.dump(art, open(ap, "w"))
    for s in SKINS:
        gen, args, name = s[0], s[1], s[2]
        extra = s[3] if len(s) > 3 else []
        dst = os.path.join(out, name)
        shutil.rmtree(dst, ignore_errors=True)
        a = [x.replace("{machines}", machines) for x in args]
        subprocess.run([sys.executable, os.path.join(HERE, gen), ap] + a + [dst] + extra, cwd=ROOT, check=True, stdout=subprocess.DEVNULL)


def files(d):
    out = []
    for r, _, fs in os.walk(d):
        for f in fs:
            out.append(os.path.relpath(os.path.join(r, f), d))
    return sorted(x for x in out if x != "art.json")


def rgba(path):
    im = Image.open(path)
    return im.mode, im.size, im.convert("RGBA").tobytes()


RANDOM_RUNS = 40   # resolve the pixels that two bits light (a dial's ring and its dot): see decode()


def make(art_path, machines, out):
    import numpy as np
    art = json.load(open(art_path))
    real_bits = get_bits(art)
    nbits = len(real_bits)
    digits = max(1, (nbits - 1).bit_length())
    work = os.path.join(out, "probe")
    rng = random.Random(1)
    rand = [[1 if rng.random() < 0.3 else 0 for _ in range(nbits)] for _ in range(RANDOM_RUNS)]
    probes = [("off", lambda n: 0), ("on", lambda n: 1)]
    probes += [("d%d" % k, (lambda k: lambda n: (n >> k) & 1)(k)) for k in range(digits)]
    probes += [("e%d" % k, (lambda k: lambda n: 1 - ((n >> k) & 1))(k)) for k in range(digits)]
    probes += [("r%d" % j, (lambda j: lambda n: rand[j][n])(j)) for j in range(RANDOM_RUNS)]
    print("art bits %d, %d probe runs" % (nbits, len(probes)), flush=True)
    runs = {}
    for name, fn in probes:
        a, _ = set_bits(art, fn)
        run_generators(a, machines, os.path.join(work, name))
        runs[name] = os.path.join(work, name)
    print("  probes done", flush=True)
    real = os.path.join(work, "real")
    run_generators(art, machines, real)
    randbits = np.array(rand, dtype=bool)   # [run][bit]
    idx = np.arange(nbits)

    def decode(sig):
        """sig: on/off per digit run (d then e) and per random run -> (a, b) with b = -1 for one bit"""
        dk, ek, rr = sig[:digits], sig[digits:2 * digits], np.array(sig[2 * digits:], dtype=bool)
        fixed, mask = 0, 0
        for k in range(digits):
            if dk[k] and not ek[k]:
                fixed |= 1 << k
            elif dk[k] and ek[k]:
                mask |= 1 << k
            elif not dk[k] and not ek[k]:
                raise ValueError("no bit at all")
        if not mask:
            if fixed >= nbits or not np.array_equal(randbits[:, fixed], rr):
                raise ValueError("one bit, but the random runs disagree")
            return fixed, -1
        # two bits: a and a ^ mask, a having the fixed digits; keep the pairs the random runs allow
        notfixed = ((1 << digits) - 1) & ~mask
        cand = idx[((idx & notfixed) == fixed) & ((idx ^ mask) < nbits)]
        ok = np.ones(len(cand), dtype=bool)
        for j in range(RANDOM_RUNS):
            ok &= (randbits[j, cand] | randbits[j, cand ^ mask]) == rr[j]
        found = sorted({min(int(c), int(c) ^ mask) for c in cand[ok]})
        if len(found) != 1:
            raise ValueError("%d pairs fit" % len(found))
        return found[0], found[0] ^ mask

    rec = os.path.join(out, "recipe")
    shutil.rmtree(rec, ignore_errors=True)
    os.makedirs(os.path.join(rec, "files"))
    images = []   # (relpath, mode, w, h, base rgba, [(pixel, bit a, bit b or -1, colour)])
    names = [n for n, _ in probes]
    for rel in files(runs["off"]):
        if os.path.basename(rel).startswith("preview"):
            continue   # the generators' offline previews: not installed
        paths = {k: os.path.join(v, rel) for k, v in runs.items()}
        blobs = {k: open(p, "rb").read() for k, p in paths.items()}
        if len(set(blobs.values())) == 1:
            dst = os.path.join(rec, "files", rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(paths["off"], dst)
            continue
        if not rel.lower().endswith(".png"):
            raise SystemExit("%s depends on the art and is not an image: cannot be made a recipe" % rel)
        mode, size, off = rgba(paths["off"])
        px = {k: np.frombuffer(rgba(paths[k])[2], dtype=np.uint32) for k in names}
        c0, c1 = px["off"], px["on"]
        rest = np.stack([px[k] for k in names[2:]])   # [run][pixel]
        changing = np.nonzero(c0 != c1)[0]
        if np.any((rest != c0) & (rest != c1)):
            raise SystemExit("%s: a third colour (blended art)" % rel)
        if np.any(np.any(rest[:, c0 == c1] != c0[c0 == c1], axis=0)):
            raise SystemExit("%s: a pixel changes with some art but not with all of it" % rel)
        on = rest[:, changing] == c1[changing]   # [run][changing pixel]
        cache, deps = {}, []
        for i, p in enumerate(changing):
            sig = tuple(bool(x) for x in on[:, i])
            if sig not in cache:
                try:
                    cache[sig] = decode(sig)
                except ValueError as e:
                    raise SystemExit("%s pixel %d: %s" % (rel, p, e))
            a, b = cache[sig]
            deps.append((int(p), a, b, int(c1[p]).to_bytes(4, "little")))
        images.append((rel, mode, size[0], size[1], bytes(off), deps))
        print("  %s: %d art pixels (%d of two bits)" % (rel, len(deps), sum(1 for d in deps if d[2] >= 0)), flush=True)
    write_bin(os.path.join(rec, "art.bin"), images, nbits)

    rng2 = random.Random(2)
    check_bits = [rng2.randint(0, 1) for _ in range(nbits)]
    check_art, _ = set_bits(art, lambda n: check_bits[n])
    check_dir = os.path.join(work, "check")
    run_generators(check_art, machines, check_dir)
    for label, bits, ref in (("real art", real_bits, real), ("random art", check_bits, check_dir)):
        bad = 0
        for rel, mode, w, h, base, deps in images:
            if fill(base, deps, bits) != rgba(os.path.join(ref, rel))[2]:
                bad += 1
                print("  MISMATCH %s (%s)" % (rel, label))
        for rel in files(os.path.join(rec, "files")):
            if open(os.path.join(rec, "files", rel), "rb").read() != open(os.path.join(ref, rel), "rb").read():
                bad += 1
                print("  MISMATCH %s (%s)" % (rel, label))
        if bad:
            raise SystemExit("the recipe does not give the generators' output (%s)" % label)
        print("check: the recipe with the %s gives the generators' output exactly (%d images from the art, %d files as is)"
              % (label, len(images), len(files(os.path.join(rec, "files")))), flush=True)
    shutil.rmtree(work)


def fill(base, deps, bits):
    px = bytearray(base)
    for p, a, b, c1 in deps:
        if bits[a] or (b >= 0 and bits[b]):
            px[4 * p:4 * p + 4] = c1
    return bytes(px)


def write_bin(path, images, nbits):
    """art.bin, little-endian:
         'MDSK' u32 version=1, u32 nbits, u32 images
         per image: u16 len, path (utf-8, relative to the skins' root: main/, tapfx/), u8 mode (0 RGB, 1 RGBA), u32 w, u32 h,
                    the base: u32 runs, then runs of (u32 count, 4 bytes RGBA),
                    the art pixels: u32 count, then per pixel u32 index (y*w+x), u32 bit a, i32 bit b (-1: none), 4 bytes RGBA
       A pixel takes its base colour, or the art pixel's colour when the user's art has bit a or bit b set (a dial's ring and its dot
       light some pixels both)."""
    out = io.BytesIO()
    out.write(b"MDSK" + struct.pack("<III", 1, nbits, len(images)))
    for rel, mode, w, h, base, deps in images:
        p = rel.encode()
        out.write(struct.pack("<H", len(p)) + p + struct.pack("<BII", 1 if mode == "RGBA" else 0, w, h))
        runs = []
        i = 0
        while i < len(base):
            c = base[i:i + 4]
            j = i + 4
            while j < len(base) and base[j:j + 4] == c:
                j += 4
            runs.append(((j - i) // 4, c))
            i = j
        out.write(struct.pack("<I", len(runs)))
        for n, c in runs:
            out.write(struct.pack("<I", n) + c)
        out.write(struct.pack("<I", len(deps)))
        for p, a, b, c1 in deps:
            out.write(struct.pack("<IIi", p, a, b) + c1)
    open(path, "wb").write(out.getvalue())   # not compressed: the device reads it without zlib (the installer zip compresses it)


if __name__ == "__main__":
    if len(sys.argv) != 5 or sys.argv[1] != "make":
        print(__doc__)
        sys.exit(2)
    make(sys.argv[2], sys.argv[3], sys.argv[4])
