#!/usr/bin/env python3
"""Writes params.json for Machinedrum Module (VST index = order; append only, never reorder once shipped).
Keys match vst/engine.cpp. Per track (0-15): machine (a raw OS machine id, not a named option list - the
id table is decoded from the user's own firmware at runtime, see docs/FIRMWARE.md, so this repo can't
commit real names for it), vol, pan, the AMP/EFX page's 8 params (AMD/AMF/EQF/EQG/FLTF/FLTW/FLTQ/SRR -
HostModel raw params 8-15), and the ROUTE page's 6 not-otherwise-exposed params (DIST/DEL/REV/LFOS/
LFOD/LFOM - raw 16, 19-23). This 8+6 split (not one 9-param FX page) matches real hardware: the
`SynthesisEffectsRouting` panel button actually cycles THREE pages (SYN -> AMP/EFX "TFX" -> ROUTE
"ROUT" -> back to SYN), each an 8-dial grid; DIST is ROUTE's own first dial, not a 9th AMP/EFX param -
see HANDOFF.md, "found the third per-track page: ROUTE, and a real DIST placement bug". VOL/PAN are
also real ROUTE-page citizens on hardware, but keep their own existing keys/knobs rather than
duplicating them under ROUTE. FX_PARAMS' defaults give a clean, filter-open, EQ-flat starting point
(matching tools/mdrender/mdrender.cpp's demo kit), not the raw all-zero default, which would otherwise
leave every track's filter effectively closed; ROUTE_PARAMS default to 0 (matching real hardware's own
send-level/LFO-off defaults).

SYN1-8 (track%d_syn1..8, HostModel raw 0-7) come last (appended after the picker flags): their meaning
and good defaults are per-machine, so engine.cpp resets untouched ones to the machine's own defaults on
every machine change and serves each one's current label as "<key>_name" ("dynamic_name", read by the
wrapper's effGetParamName) - their declared default here is only a placeholder. Plus two globals, tempo and max_voices
(HostModel::setMaxActiveVoices, HANDOFF.md "adjustable voice cap")."""
import json

# key, name, default - HostModel raw params 8-15, in the real hardware's own AMP/EFX page order
FX_PARAMS = [
    ("amd", "AMD", 0), ("amf", "AMF", 0), ("eqf", "EQF", 64), ("eqg", "EQG", 64),
    ("fltf", "FLTF", 0), ("fltw", "FLTW", 127), ("fltq", "FLTQ", 0), ("srr", "SRR", 0),
]
# key, name, default - HostModel raw params 16, 19-23, in the real hardware's own ROUTE page order
ROUTE_PARAMS = [
    ("dist", "DIST", 0), ("del", "DEL", 0), ("rev", "REV", 0),
    ("lfos", "LFOS", 0), ("lfod", "LFOD", 0), ("lfom", "LFOM", 0),
]

params = []
sections = []
for t in range(16):
    keys = []
    params.append({"key": "track%d_machine" % t, "name": "T%d Machine" % (t + 1), "min": 0, "max": 191,
                    "default": 0, "display": "int"})
    keys.append("track%d_machine" % t)
    params.append({"key": "track%d_vol" % t, "name": "T%d Vol" % (t + 1), "min": 0, "max": 127,
                    "default": 100, "display": "int"})
    keys.append("track%d_vol" % t)
    params.append({"key": "track%d_pan" % t, "name": "T%d Pan" % (t + 1), "min": 0, "max": 127,
                    "default": 64, "display": "int"})
    keys.append("track%d_pan" % t)
    for key, label, default in FX_PARAMS:
        params.append({"key": "track%d_%s" % (t, key), "name": "T%d %s" % (t + 1, label), "min": 0, "max": 127,
                        "default": default, "display": "int"})
        keys.append("track%d_%s" % (t, key))
    for key, label, default in ROUTE_PARAMS:
        params.append({"key": "track%d_%s" % (t, key), "name": "T%d %s" % (t + 1, label), "min": 0, "max": 127,
                        "default": default, "display": "int"})
        keys.append("track%d_%s" % (t, key))
    sections.append(("TRACK %d" % (t + 1), keys))

params.append({"key": "tempo", "name": "Tempo", "min": 30, "max": 300, "default": 120, "display": "int"})
params.append({"key": "max_voices", "name": "Voice Budget", "min": 1, "max": 16, "default": 6, "display": "int"})
sections.append(("GLOBAL", ["tempo", "max_voices", "rom_enabled"]))

# Machine picker "open" flags (HANDOFF.md, "Machine picker: duplicating Monomodule's own design").
# Appended after every other param, per this file's own "append only" rule (keeps every existing
# param's index stable, so saved projects stay valid). "popup_of" makes the wrapper keep each of
# these entirely to itself - never sent to the engine, never in the chunk - exactly the mechanism
# shadow_skin.py's own `popup` widget uses (wrapper/popup.h's popup_is()/popup_set()/popup_picked()
# key off this field alone, generically, whether or not a real `popup` layout line exists - confirmed
# by reading wrapper/vst2_wrap.c directly: its setParameter() already calls popup_set() for every
# param before ever reaching this port's own set_param()). No engine.cpp changes needed for this.
for t in range(16):
    key = "track%d_machine" % t
    params.append({"key": "%s__open" % key, "name": "T%d Machine List" % (t + 1),
                    "min": 0, "max": 1, "default": 0, "display": "int", "popup_of": key})

# SYN1-8, appended last (append-only rule). engine.cpp owns their values: see the docstring.
for t in range(16):
    for p in range(1, 9):
        params.append({"key": "track%d_syn%d" % (t, p), "name": "T%d SYN%d" % (t + 1, p),
                        "min": 0, "max": 127, "default": 0, "display": "int", "dynamic_name": True})

# LEV (the kit's per-track level, HostModel::setLevel - separate from VOL), appended last. Default 100 is
# HostModel's own startup level.
for t in range(16):
    params.append({"key": "track%d_level" % t, "name": "T%d LEV" % (t + 1), "min": 0, "max": 127,
                    "default": 100, "display": "int"})

# The track's LFO page (engine.cpp kLfoKeys), appended last. lfo_param's shown text is the destination's own label
# ("dynamic_display": the wrapper asks the DSP for "<key>_display").
for t in range(16):
    params.append({"key": "track%d_lfo_track" % t, "name": "T%d LFO Track" % (t + 1), "min": 0, "max": 15, "default": t, "display": "int"})
    params.append({"key": "track%d_lfo_param" % t, "name": "T%d LFO Param" % (t + 1), "min": 0, "max": 23, "default": 0, "display": "int",
                   "dynamic_display": True})
    params.append({"key": "track%d_lfo_shp1" % t, "name": "T%d LFO Shape 1" % (t + 1), "min": 0, "max": 5, "default": 0, "display": "int"})
    params.append({"key": "track%d_lfo_shp2" % t, "name": "T%d LFO Shape 2" % (t + 1), "min": 0, "max": 5, "default": 0, "display": "int"})
    params.append({"key": "track%d_lfo_type" % t, "name": "T%d LFO Update" % (t + 1), "min": 0, "max": 2, "default": 0, "display": "int"})

# Kits (engine.cpp: the factory kits and any .syx kit packs): BANK and KIT rows as mpc-vst-monomodule's BANK/PRESET -
# prev/next are momentary, the names live text. Then the GLOBAL page's randomise toggles (momentary, held ON 1.5 s).
params.append({"key": "bank_prev", "name": "Bank Prev", "min": 0, "max": 1, "momentary": True})
params.append({"key": "bank_next", "name": "Bank Next", "min": 0, "max": 1, "momentary": True})
params.append({"key": "bank_name", "name": "Bank", "min": 0, "max": 0, "display": "string"})
params.append({"key": "kit_prev", "name": "Kit Prev", "min": 0, "max": 1, "momentary": True})
params.append({"key": "kit_next", "name": "Kit Next", "min": 0, "max": 1, "momentary": True})
params.append({"key": "kit_name", "name": "Kit", "min": 0, "max": 0, "display": "string"})
for key, name in (("randomize_all", "Randomise Machines"), ("randomize_1_8", "Randomise Machines 1-8"),
                  ("randomize_9_16", "Randomise Machines 9-16"), ("randomize_kit", "Randomise Kit")):
    params.append({"key": key, "name": name, "options": ["OFF", "ON"], "default": 0, "momentary": True, "hold_ms": 1500})

# ROM machines on/off (appended, like every later param, to keep the indices of saved projects). Off: tracks on a ROM
# machine stay silent (engine.cpp swaps in the empty machine; the track keeps its ROM setting for when it is back on).
params.append({"key": "rom_enabled", "name": "ROM Machines", "options": ["OFF", "ON"], "default": 1})

json.dump({"name": "Machinedrum Module", "params": params,
           "sections": [{"label": l, "keys": k} for l, k in sections]},
          open("params.json", "w"), indent=1)
