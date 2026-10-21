# Machinemodule for Schwung (Ableton Move)

The Elektron Machinedrum SPS-1 UW sound engine as a [Schwung](https://github.com/charlesvestal/schwung) sound generator: the same
engine as the desktop plugin and the MPC builds (the voice DSP in the dsp56300 JIT, the OS's own control code, the track effects,
mixer and master effects), with the 16 tracks on Move's 16 drum pads and pages that follow the pad you hit, after
[DR32](https://github.com/legsmechanical/schwung-dr32). Test build: see [PROPOSAL.md](PROPOSAL.md) for the design, the measurements
and what is open.

Not affiliated with, endorsed by or sponsored by Elektron. No firmware is included: you upload your own.

## Install

1. `schwung/scripts/build.sh` (Docker), then Schwung Manager's **Install from File** with `schwung/dist/machinemodule-module.tar.gz`,
   or `schwung/scripts/install.sh ableton@move.local`.
2. On the module's page in Schwung Manager, upload:
   - **os**: the Machinedrum UW OS 1.63 `.syx` (required; a free download from Elektron).
   - **flash** (optional): the Machinedrum UW flash image `.bin`. The first start after the upload imports it in the emulated
     Machinedrum (about 6 minutes; the module is silent meanwhile): the 16 factory kits and the ROM machines' samples.
   - **kits** (optional): your own kit dumps (`.syx`), listed after the factory kits.
3. Restart Move, add **Machinemodule** to a track, play the pads.

## Playing it

Tracks 1-16 are notes 36-51, the drum pads. Hit a pad and every track page shows that track (while a pattern runs, only a pad you hit
moves the pages, not the sequence). Jog moves through the pages: **Kit**, **Track** (TRK, MACH, LEVEL, MUTE), **Synth** (the machine's
own SYN labels), **Amp/Filter**, **Mix**, **LFO**, then the kit-wide **Echo**, **Reverb**, **EQ**, **Dynamix** and **Master**.

The Master page: **FX** Off / Full / Rev+Echo (Rev+Echo skips the EQ and Dynamix: about a quarter less CPU, and quieter, since the
Dynamix is a compressor), **LTNCY** (blocks of 2.9 ms; raise it if it clicks), **VELMD** (MD: hits at or above ACTHR are accented, the
others ACAMT softer; VEL: velocity is the level), **VOICS** (how many tracks may sound at once), **LOAD** and **PEAK**.

Each track can go to its own Schwung bus (split voices): it leaves the main mix with its level and pan, and gets the bus's effects
instead of the MD's master section.

## Files

| | |
|---|---|
| `src/plugin/md_plugin.cpp` | `dsp.so`, inside Move: the kit table, the pages, MIDI, kits, state, buses; spawns and watches the engine |
| `src/child/md_engine.cpp` | `md-engine`, the engine process (cores 0-2, made real-time by the plugin) |
| `src/common/md_shm.h` | the shared memory between them |
| `module/` | `module.json`, `help.json` |
| `tests/md_hosttest.cpp` | loads `dsp.so` as Schwung does and plays it in real time, on a Move (run as root for Move's callback priority) |
| `tests/md_busybench.cpp` | a kit's CPU cost per block on a Move, for each engine configuration |
| `tools/move_bench.sh` | the first measurements (md-hash, md-cost on a Move) |

The kit table, the control machines' rules, the feed to the engine and the pre-warm are shared with the desktop plugin
(`desktop/src/core`).
