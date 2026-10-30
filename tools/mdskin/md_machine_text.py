"""A line about each machine and its family, shown next to the machine block. The words are ModuleDrum's
(https://github.com/K-X420/ModuleDrum, src/plugin/md/MdMachineText.h, AGPL-3.0; by KX), which follow the Machinedrum manual's
machine reference. Caps only: the LCD fonts have no lowercase.
"""

FAMILY = {
    "GND": "BASIC TONES AND NOISE",
    "TRX": "ANALOG STYLE DRUMS",
    "EFM": "ENHANCED FEEDBACK FM",
    "E12": "12-BIT SAMPLED DRUMS",
    "P-I": "PHYSICAL MODELS OF DRUMS",
    "ROM": "YOUR OWN SAMPLES",
}

MACHINE = {
    0: "EMPTY TRACK", 1: "SINE WAVE", 2: "WHITE NOISE", 3: "IMPULSE",
    16: "ANALOG BASS DRUM", 17: "ANALOG SNARE", 18: "ANALOG TOM", 19: "ANALOG CLAP", 20: "ANALOG RIMSHOT",
    21: "ANALOG COWBELL", 22: "CLOSED HI-HAT", 23: "OPEN HI-HAT", 24: "ANALOG CYMBAL", 25: "MARACAS",
    26: "CLAVES", 27: "CONGAS", 28: "SECOND BASS DRUM", 29: "SECOND SNARE",
    32: "FM BASS DRUM", 33: "FM SNARE", 34: "FM TOM", 35: "FM CLAP", 36: "FM RIMSHOT", 37: "FM COWBELL",
    38: "FM HI-HAT", 39: "FM CYMBAL",
    48: "SAMPLED BASS DRUM", 49: "SAMPLED SNARE", 50: "SAMPLED HIGH TOM", 51: "SAMPLED LOW TOM",
    52: "SAMPLED CLAP", 53: "SAMPLED RIMSHOT", 54: "SAMPLED COWBELL", 55: "SAMPLED CLOSED HAT",
    56: "SAMPLED OPEN HAT", 57: "SAMPLED RIDE", 58: "SAMPLED CRASH", 59: "SAMPLED BRUSHED SNARE",
    60: "SAMPLED TAMBOURINE", 61: "SAMPLED TRIANGLE", 62: "SAMPLED SHAKER", 63: "SAMPLED BONGO / CONGA",
    64: "MODELLED BASS DRUM", 65: "MODELLED SNARE", 66: "MODELLED TOM", 67: "MODELLED METALLIC STRIKE",
    68: "MODELLED MARACAS", 69: "MODELLED RIMSHOT", 70: "MODELLED RIDE", 71: "MODELLED CRASH",
    72: "MODELLED HI-HAT",
}


def lines(m):
    """(machine line, family line) for a machine from mk_skin.parse_machines."""
    text = MACHINE.get(m["id"])
    if text is None and m["group"] == "ROM":
        text = "SAMPLE SLOT %d" % int(m["short"])
    return text or "", FAMILY.get(m["group"], "")
