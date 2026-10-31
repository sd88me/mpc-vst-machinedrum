"""The machine block's family logos: the Machinedrum's own family badges, read from the user's OS file (mdartdump: bitmaps
"familyBadge", in the order E12 EFM P-I TRX UW, found as ModuleDrum does - https://github.com/K-X420/ModuleDrum). ROM and RAM show
UW. GND has none: its name is printed (and INP / MID / CTR / RAM have no machine bar of their own, see mk_skin.drawn()).
"""

BADGE_INDEX = {"E12": 0, "EFM": 1, "P-I": 2, "TRX": 3, "ROM": 4, "RAM": 4}


def logo(group, bitmaps):
    """The group's logo as rows of '#'/'.', or None (print the name). bitmaps: art.json's "bitmaps"."""
    i = BADGE_INDEX.get(group)
    badges = bitmaps.get("familyBadge")
    if i is None or not badges:
        return None
    b = badges[i]
    # not trimmed: the layout must not depend on the art (skin_recipe.py checks every pixel follows at most one art bit)
    return ["".join("#" if (int(r, 16) >> (63 - x)) & 1 else "." for x in range(b["w"])) for r in b["rows"][:b["h"]]]
