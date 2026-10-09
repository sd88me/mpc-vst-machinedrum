# Plan: Machinemodule for the desktop (VST3/AU), built on Monomodule

Goal: a desktop version of Machinemodule (VST3 first, AU and Standalone because JUCE builds them for free) that stays
**as close to the Machinedrum as possible**. It reuses this repo's engine and takes shnolk's
[Monomodule](https://github.com/shnolk/monomodule) (v1.0.1, `eb5cfee`) as the plugin framework. The desktop has none of the
Gen 1 Force's limits, so most of the MPC-specific work is dropped and the parked "faithful build" from the roadmap becomes
the default.

## 1. What carries over, what is dropped, what comes back

### Kept from this repo (the sound engine, unchanged in behaviour)

| Part | Files | Notes |
|---|---|---|
| OS file decoder | `tools/mdfw/Firmware.*` | Already adapted from Monomodule's decoder. |
| ColdFire side (OS tick, smoothing, LFOs, machine coefficient functions in Musashi) | `engine/HostModel.*`, `engine/MachineRunner.*` | Portable apart from one `memmem` call (not on MSVC). |
| Voice DSP (DSP2 program in dsp56300) | `engine/VoiceEngine.*`, `engine/Dsp56.h` | Moves from the ARM static recompiler to the **dsp56300 JIT** (x86-64 and arm64). |
| Per-track effects and mix (bit-exact C++ of DSP1) | `engine/TrackFx.*`, `engine/Mixer.*` | Kept as they are: verified, and cheaper than emulating them. |
| Engine wrapper | `engine/MdEngine.h` | `EngineT<VoiceEngine>` (single thread) is enough on a desktop; `ParallelVoiceEngine` stays as an option. |
| Kit sysex import, state chunk | parts of `vst/engine.cpp` | Lifted into a portable `KitCodec` (no MPC paths, no POSIX directory code). |
| Reference and test tools | `tools/mdhash`, `tools/mdmix` (MixerRef), `tools/mdrender` | Become ctest tests (section 4). |

### Taken from Monomodule (the framework)

| Part | Monomodule files | What changes for the MD |
|---|---|---|
| CMake: JUCE 8.0.9 via FetchContent, pinned dsp56300 + patch, plugin targets, hardened-runtime JIT entitlements on macOS | `CMakeLists.txt`, `cmake/dsp56300.cmake`, `cmake/apply_patch.cmake`, `src/plugin/CMakeLists.txt` | A second patch for the MD fixes (Phase 0). |
| Processor skeleton: APVTS, MIDI handling, state, latency reporting, bypass | `src/plugin/one/OneProcessor.*`, `OneParams.h` | 16 tracks in one instance (Monomodule Six is the model), MD note map. |
| 44.1 kHz <-> host-rate converter | `src/core/dsp/Resampler.*` | As is. The MD also runs only at 44.1 kHz. |
| OS-file selection, shared settings, status text | `SharedSettings.h`, `setFirmwarePath` etc. | Settings folder `.../Machinemodule/`. |
| LCD editor: art read from the OS file at run time, fallback face without one | `one/Lcd.*`, `one/RomArt.*`, `one/OneEditor.*`, `one/MachinePicker.*` | RomArt addresses for MD OS 1.63 (we already have them: `tools/mdskin/mdartdump.cpp`). MD page layouts from `tools/mdtrace/ui_spec.py`. |
| Library app and catalogue (sysex import, versions, previews) | `src/core/library`, `src/library`, `src/app` | Later phase: needs an MD kit codec in place of the Monomachine one. |
| Capture harness and UI snapshot tool | `src/plugin/Capture.cpp`, `UiSnapshot.cpp` | For regression renders and screenshot review. |

### Dropped (MPC/Force only)

- The static recompiler, the build-time trace step and the bit-exactness gate in `release/build_release.sh`
  (replaced by the JIT plus a ctest that compares JIT and interpreter; see Phase 0).
- Docker, the armhf toolchain, `vst2_wrap.c`, the MPC skin generator (`tools/mdskin/mk_skin.py`, the Python atlases), the
  installer zips, `mpc-plugin.json`, the MPC OS drum-pad patch.
- The engine thread with its 3-block ring and 8.7 ms of added latency, core picking from `/proc/stat`, FIFO priorities.
  Render inside `processBlock` like Monomodule. Latency is then only the resampler's (0 at 44.1 kHz).
- The voice budget (VOICES), silence release and the ROM on/off switch. All 16 tracks always run, as on the hardware.
  Silence release can come back later as an opt-in "eco" switch if CPU use is a problem, off by default.
- Taps / Tap FX: replaced by real output buses.
- Randomise: not a Machinedrum feature. Leave it out of v1 and add it back later if people miss it.

### Brought back (now that CPU and the host allow it)

- **Master effects**: rhythm echo, gate box/reverb, EQ, dynamix. Emulate DSP1's master section (`$342-$971`) in
  dsp56300, MixerRef-style, as the 2026-09-30 HANDOFF entry already planned (about 13.4 M DSP instructions a second: trivial
  for a desktop JIT). REV and DEL then do what they do on the hardware.
- **Full 16-track output** (decided): Main stereo (the MD's main out, master effects included) plus 16 per-track outputs,
  one per track, each after the track's effects and VOL. This replaces the taps. See Phase 2 for the details.
- **INP machines** (INP-GA, GB, FA, FB): the plugin's side-chain input as the MD's audio inputs A/B, the same way
  Monomodule feeds its FX machines.
- **RAM machines** (RAM-R/P): record from the side-chain input or the main mix into the voice DSP's RAM. Later phase:
  needs the record/play protocol traced first.
- **MID machines**: MIDI out from the plugin. Later phase, low priority.
- **Pre-built releases**: everything firmware-derived is read at run time (OS file, LCD art, ROM samples, factory kits).
  Nothing of Elektron's is in the binary, so unlike the MPC build this one can be shipped as a download, as Monomodule is.

## 2. Repository layout

Decided: a **`desktop/` folder in this repo** for now, with its own CMake project, so the MPC build (`CMakeLists.txt`,
`release/`, `vst/`) is untouched. The desktop build uses `engine/` and `tools/mdfw/` from the repo root as they are, so
there is one copy of the engine. Monomodule's framework files are copied in (with their AGPL headers and a note of the
upstream commit, `eb5cfee`), not forked; upstream fixes are carried over by hand when needed.

```
desktop/
  CMakeLists.txt              Monomodule's top level, cut down: JUCE 8.0.9 via FetchContent, MD targets
  cmake/dsp56300.cmake        Monomodule's pinned commit + 0001-dsp56300-mnm.patch + 0002-dsp56300-md.patch
  ext/patches/                the two dsp56300 patches
  src/core/                   MdVoice (engine + resampler glue), MasterFx, KitCodec, RomData, Resampler (from Monomodule)
  src/plugin/                 MdProcessor, MdEditor, MdParams, RomArt (MD OS 1.63), Lcd, MachinePicker
  src/cli/                    md-render (WAV render), md-import (flash image -> ROM samples + factory kits)
  tests/                      JIT vs interpreter hash, MixerRef vs TrackFx, master FX vs emulated DSP1, golden renders
  README.md                   desktop build and install
```

`desktop/CMakeLists.txt` adds `../engine` and `../tools/mdfw` as the `mdcore` library (the same source list as the root
`CMakeLists.txt`) and Musashi from `../libs/gearmulator-md-mm`. The root `.gitignore` already keeps firmware and derived
files out; add `desktop/build*/`.

The master effects (`MasterFx`) live in `desktop/src/core/`: the MPC build stays as it is, without them, by decision.
If the MPC build ever gets them, the class moves to `engine/`.

Licences fit together: both projects are AGPL-3.0-only, dsp56300 and gearmulator-md-mm are GPL-3.0, JUCE 8 is AGPL.

## 3. Phases

Each phase ends in something that can be run and checked.

### Phase 0: engine on desktop, bit-exact under the JIT (the main technical risk)

1. Build `mdcore` on x86-64 Linux, macOS (arm64 and x86-64) and Windows (MSVC). Replace `memmem` with `std::search`.
   Confirm Musashi's CPU state is per instance (`getCpuState()`), because a DAW runs several plugin instances in one process.
2. Settle the dsp56300 version. Our fork (`libs/dsp56300`, branch arm32) carries the MERGE instruction, the boot-loader
   cache-invalidation fix and the opcode-table static-init (SIOF) fix. Monomodule pins `c051afad` with SR.SM saturation,
   MPYRI and PFLUSH. HANDOFF records that the JIT "can crash while VoiceEngine initialises" on x86-64/arm64 hosts.
   - Recommended: start from Monomodule's pinned commit and patch, and add our fixes as a second patch
     (`0002-dsp56300-md.patch`). The JIT is the same one Monomodule ships on macOS, Windows and Linux.
   - Fallback: fix the JIT crash in our own fork.
3. Gate: `md-hash` (interpreter) = `md-hash` (JIT) on the same OS file and ROM samples, including the per-machine sweep. This
   is the existing release gate, run as a ctest.
4. Measure: real-time factor for 16 busy tracks on a modest laptop. Expected: well under one core, since the Force does about
   6 voices on a slow ARM core with the recompiler.

**Done when:** the JIT hash equals the interpreter hash on all three OSes, with no crash on init.

### Phase 1: a minimal VST3 that plays

1. Set up `desktop/` with the Monomodule files listed in section 1. Add the `MdVoice` glue: `EngineT` -> 32-frame blocks -> FIFO -> Monomodule's Resampler -> host
   buffer (the same pattern as Monomodule's `MonoVoice::process`).
2. `MdProcessor`: OS file selected at run time (Monomodule's flow and settings file); MIDI notes 36-51 play tracks 1-16
   (the MD's map), velocity to trigger velocity; host tempo to `HostModel::setTempo`.
3. Parameters: 16 tracks x (machine, SYN1-8, AMD AMF EQF EQG FLTF FLTW FLTQ SRR, DIST VOL PAN DEL REV, LFO page, LEV), raw
   0-127 like a kit byte (Monomodule's convention). Stable parameter IDs from day one, for automation and saved projects.
4. State: APVTS plus the kit bytes. Load a kit `.syx` from a file chooser.
5. Generic JUCE editor for now.

**Done when:** it loads in Reaper, Bitwig and Ableton (VST3) and Logic (AU); a factory-style kit plays, saves and reloads;
the output at 44.1 kHz is sample-identical to `md-render` for the same note list.

### Phase 2: 16 track outputs and the master effects

1. Buses: Main stereo + Track 1-16. Each track bus carries the track after its effects and VOL (`Mixer::solo`, which
   the taps use today). Each track bus is stereo, so PAN can apply there, with a per-track "PAN on track out" switch
   (default on); a mono bus is offered too where the host supports it. When a track's bus is enabled in the host, the
   track leaves the dry main mix (the `dryMute` mask the taps already use) but still feeds the reverb and delay sends,
   as on the hardware's individual outputs. Disabled buses cost nothing, and with none enabled the main output is
   identical to the single-output build.
   The MD's own ROUTE setting (main / A-D) stays a kit parameter so kits load and save unchanged, but the plugin routes by
   track bus instead.
2. `MasterFx`: DSP1 in dsp56300 running only the master section, fed with the dry mix and the REV/DEL sends; parameters
   `Y:$150-$18c` from the kit's 32 master-FX bytes. First decode how the OS tick fills them (`$1000d7c+16`).
3. Master FX pages as parameters: rhythm echo, gate box, EQ, dynamix. Kits now load their master settings (today ignored).
4. Check against gearmulator-md-mm's final audio for the same kit and triggers (the full-system reference).

**Done when:** a kit with reverb and delay matches gearmulator-md-mm's main output.

### Phase 3: the Machinedrum LCD editor

1. `RomArt` for MD OS 1.63: the font and dial addresses `tools/mdskin/mdartdump.cpp` already reads, drawn by Monomodule's
   `Lcd` code. Fallback face when no OS file is selected.
2. Pages from `tools/mdtrace/ui_spec.py`: SYN (labels follow the machine, from `MachineRunner::machines()`), AMP/EFX,
   ROUTE, LFO (destination by name), master FX, and a kit/mixer page with 16 level bars.
3. Machine picker (Monomodule's `MachinePicker`), track select 1-16, kit browser.
4. Hardware-style bezel, scalable, from Monomodule's `SkinDialog` and look-and-feel.

**Done when:** every hardware page can be edited from the plugin, and `mnm-uisnapshot`-style screenshots look like the
MD's LCD.

### Phase 4: ROM machines and factory kits from the user's flash image

Today these come from booting the emulated MD at build time (`mdProbe` + `tools/mdkits`).

1. `md-import` (CLI) and an "Import flash image..." button: boot gearmulator-md-mm once in a background thread, dump DSP2's
   sample memory and the 64 kits, and write `ROM_SAMPLES.bin` and `FACTORY.syx` to the user data folder. Runs once per
   user; the plugin never holds the flash image.
2. Without an import: every machine except ROM plays and there are no factory kits (the same as today's flash-less build).
3. Later, optional: read the sample flash and kit area of the `.bin` directly, without booting. Only worth it if the boot
   is slow or fragile on Windows/macOS.
4. Close the open check from the README: ROM machine output bit-exact against the emulated MD.

**Done when:** a fresh install plus OS file plus flash image gives the 16 factory kits and sounding ROM machines.

### Phase 5: closer to the hardware

- INP machines from the side-chain input.
- Sequencer-side behaviour the host model lacks: parameter locks (DAW automation covers most of this already), trigger
  groups, accent. Mute and solo per track.
- Kit/global MIDI behaviour: base channel, CC map as in the MD manual, so the hardware's own controller templates work.
- RAM machines, then MID machines.
- Resolve the TRX SD difference between our dsp56300 fork and gearmulator-md-mm's (the README's open check) by comparing
  against a hardware recording.

### Phase 6: library and release

- Port Monomodule's Library (catalogue, versions, previews) to MD kits; patterns and songs only once there is a
  sequencer to preview them with.
- Release CI: macOS universal (signed, notarised, hardened runtime with the JIT entitlements Monomodule already declares),
  Windows x64 installer, Linux x64 zip. As with Monomodule, the binaries contain no Elektron data.
- README: getting the OS file, importing the flash image, what is and is not modelled, "not affiliated with Elektron".

## 4. Verification (carried over from the MPC work)

- **JIT = interpreter**: `md-hash` hash and machine sweep, every CI run (needs the OS file: skip without it, as Monomodule's
  tests do with `MNM_OS`).
- **TrackFx/Mixer = DSP1**: `tools/mdmix` MixerRef comparison as a test.
- **MasterFx = DSP1**: the master section in the emulator against the full DSP1 program on the same inputs.
- **Plugin = engine**: Monomodule's Capture harness renders fixed scenarios through `MdProcessor` and compares them with
  `md-render`; at 48/96 kHz, check the resampler's level and latency with the existing `--check-latency`-style test.
- **Plugin = hardware**: gearmulator-md-mm full-system renders as golden files (kept outside the repo, like the firmware).

## 5. Risks and open questions

| Risk | Mitigation |
|---|---|
| The dsp56300 JIT crashes on the MD voice program (seen once on x86-64/arm64). | Phase 0 settles this first; the interpreter fallback is always available (slower but well within desktop budgets). |
| Two dsp56300 lineages (ours and Monomodule's). | One pinned commit plus two small patches, both tested by the JIT=interpreter gate. |
| Several plugin instances in one process. | Check for global state in Musashi and in the dsp56300 JIT; Monomodule already runs Six with six DSPs in one process. |
| Booting gearmulator-md-mm inside the plugin for the flash import. | Keep it in a background job and a separate CLI; it runs once and its output is cached. |
| Master-FX parameter mapping not decoded yet. | Phase 2 starts with tracing the tick on the full-system emulator, as was done for DSP1's per-track words. |
| macOS JIT needs the hardened-runtime entitlements and notarisation. | Monomodule's CMake already sets them; copy as is. |

Decisions (2026-10-09):

1. A `desktop/` folder in this repo, for now.
2. Full 16-track output: Main + one bus per track.
3. The name stays **Machinemodule**.
4. The MPC build keeps its current effects (no master FX there for now).
5. The desktop version has the Machinedrum's original master effects (rhythm echo, gate box/reverb, EQ, dynamix), built in.
