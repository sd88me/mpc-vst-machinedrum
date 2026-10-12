# Handoff: desktop Machinemodule (VST3/AU/Standalone) - resume here

Last updated 2026-10-12. Branch `ccr-17f199b1-c37d7j` (pushed; latest commit when this was written: see `git log`). The plan with
per-phase results is [PLAN-desktop-vst3.md](PLAN-desktop-vst3.md); this file is the short "where are we, how do I rebuild, what next".
The older [../HANDOFF.md](../HANDOFF.md) is the MPC Force history; this one covers the desktop work only.

## Decisions (from the user)

- Desktop version lives in `desktop/` of this repo (not a fork of Monomodule); Monomodule is the framework model only (its
  `Resampler.*` is copied unchanged, with attribution).
- **Native 64-bit desktop build** (x86-64 now; arm64/macOS/Windows to verify). Not the 32-bit ARM Force build. Same engine source.
- Outputs: Main + 16 track outputs (Track n = that track after effects and VOL; a track whose bus the host enables leaves Main).
- Name stays "Machinemodule". The MPC build is left as it is (no master effects there). The desktop build has the Machinedrum's
  original master effects built in.
- Gen 2 MPC port is a later, separate effort (see "Gen 2" below).

## State: Phases 0, 1, 2 done on Linux x86-64

| Phase | Status |
|---|---|
| 0 engine on desktop under the dsp56300 JIT | done. Fix: `cfg.interruptRegionIsCode = true` (VoiceEngine, JIT builds only). JIT = interpreter hash `04ef1db4fe767721` (no ROM samples) and `e2b514e70c173c33` (with ROM samples = the Force build's hash). `md-multi`: 4 engines on 4 threads identical to solo. |
| 1 plugin that plays | done: `desktop/` JUCE project, 16 tracks, 496 params, Main + Track 1-16 outputs, kit loading, resampler, simple editor. pluginval strictness 5 passes. `mm-render` (processor == bare engine, sample-identical). |
| 2 master effects | done: `engine/MasterFx.*`, 32 master params + on/off in the plugin. Verified vs the full-system emulation (see below). **Not done: pan on the track outputs.** |
| 3 LCD editor | **next** (below) |
| 4 flash-image import (ROM samples, factory kits) | engine side works (`tools/mdkits/make_factory.py` ran fine); no in-plugin import button yet |
| 5/6 INP/RAM/MID machines, Library, release CI | not started |

Not yet tried: macOS, Windows, Apple Silicon (arm64 JIT), a real DAW, the ARM (Force) build with the new engine files (only x86 built;
`md-hash` for the MPC path is unchanged, but nobody has cross-built `MasterFx.cpp`/`MachineRunner::runRange` for armhf).

## How master FX work (what a new session must know)

- `MasterFx` = DSP1 (section 2 of the OS file) booted like `tools/mdmix/MixerRef`, then per 32-sample block: poke X:$180 (dry main
  pairs), X:$1c0 (reverb send), X:$600 (delay send), the 6-channel frames at X:$400, X:$640=$400, write the parameter words, run
  `P:$342 -> $9a8`. **The main output is X:$180 after the run** (the DSP's output routine at $270 sends it to the codec).
- Parameter words (Y:$150-$158, Y:$170-$18c) come from the OS's own tick code: four time-sliced routines `$20b4a2-$20ba4c`
  chosen by `$265404 & 3`, replayed in the 68k emulator once per tick by `HostModel` (`MachineRunner::runRange`). Inputs: the kit's 32
  master bytes (kit sysex offset `$487`: reverb 8, echo 8, EQ 8, dynamix 8) as targets of the OS smoother
  (`$1000f6c`, slewed into `$1000d9c`). Shadow -> Y mapping is in `HostModel.cpp` (`kMasterShadow*`).
- `Engine::enableMasterFx(true)` after the warm-up; first parameter set uses `setMasterFx(bytes, snap=true)` (`snapMasterFx` pre-rolls
  the OS smoother 100x so values sit where a long-running unit has them); later changes glide (faithful).
- Findings: (1) accented trigger = velocity byte 128+2*amount read sign-extended (`extb`) by the OS -> negative gain -> inverted
  polarity; `HostModel` now matches (MIDI velocities 1-127 unaffected). (2) the OS EQ code branches on `<= $1fff` where the neutral
  value sits, so EQ coefficient sign depends on whether the smoothed value last approached from above or below. (3) starting the
  section with zero parameters leaves its dynamics state wrong (~0.19x gain): never run it before it has its parameters.
- Verification recipe (all in the repo): `mdProbe` records the real emulated unit; `md-mfxref` renders the same kit/trigger through
  the engine; `tools/mdmix/mfx_compare.py ours.raw ref.raw` aligns and reports correlation/gain/residual. Result: parameter words
  identical (38/38), correlation 1.000000, gain 1.00003, residual -71 dB for the master settings of five factory kits. To compare a
  kit switch, use the same history on both sides (`--master K --switch` vs poking the target bytes into the unit's `$1000f6c`).

## Rebuild the environment (the cloud container is ephemeral; /tmp is gone)

Firmware is the user's own and must never be committed (`.gitignore` blocks `*.syx *.bin *.inl fw/`). Ask for / re-attach:
`Elektron_SPS1-1UW_OS1.63.syx` and the 8 MB flash image `.bin`. Copy them to a scratch dir outside the repo.

```bash
git submodule update --init --recursive --depth 1
# engine tools (JIT build; add -DCMAKE_CXX_FLAGS=-DDSP56K_NO_JIT_RUNTIME for the interpreter build to compare)
cmake -S . -B /tmp/b-jit -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C /tmp/b-jit md-hash md-multi md-mfxref mdsamples
/tmp/b-jit/md-hash OS.syx [ROM_SAMPLES.bin]        # expect 04ef1db4fe767721 / e2b514e70c173c33
# mdProbe (full-system reference emulator; patches the submodule working tree, never commit it)
tools/mdtrace/build_mdprobe.sh                      # ~10 min; binary: libs/gearmulator-md-mm/build/source/elektron/md/mdLibTest/mdProbe
git config submodule.libs/gearmulator-md-mm.ignore dirty   # keeps `git status` clean after the patches
# factory kits + ROM samples from the flash image (about 2 min)
python3 tools/mdkits/make_factory.py <mdProbe> /tmp/b-jit/mdsamples flash.bin OS.syx /tmp/factory
# desktop plugin (Linux needs: libasound2-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxcomposite-dev
# libxext-dev libxrender-dev libfreetype-dev libfontconfig1-dev libgl1-mesa-dev; run apt-get update first)
cmake -S desktop -B /tmp/b-desk -G Ninja && ninja -C /tmp/b-desk MmRender Machinemodule_VST3
/tmp/b-desk/MmRender_artefacts/Release/MmRender OS.syx          # expect ALL OK
# pluginval: https://github.com/Tracktion/pluginval/releases/latest/download/pluginval_Linux.zip
pluginval --strictness-level 5 --validate-in-process --skip-gui-tests --timeout-ms 120000 <...>/Machinemodule.vst3
```

mdProbe notes: time is in audio frames (the OS tick is ~every 352 frames; `adv:` does not run it, `wait:` does; allow >= 25000 frames
for the smoother to settle); `wav:PATH` records the main out; `trig:1:N` is an *accented* panel trigger; new actions this work
added: `poke8:ADDR:V[:V...]`, `dspmem:DSP:X|Y|P:FROM:TO`. MD_ROM=<ROM_SAMPLES.bin> makes `md-mfxref` load ROM samples.

## Next steps (suggested order)

1. **Phase 3: Machinedrum LCD editor.** Use the OS file's own fonts/dials (`tools/mdskin/mdartdump.cpp`, `tools/mdtrace/ui_spec.py`
   page layouts) drawn like Monomodule's `Lcd`/`RomArt` code (`ext` clone of shnolk/monomodule; `src/plugin/one/`). Pages: SYN (labels
   follow the machine: `MachineRunner::machines()`), AMP/EFX, ROUTE, LFO, master FX (real labels from the OS strings), kit browser, 16
   level bars, machine picker. Rename the generic `REV/ECHO/EQ/DYN n` master parameters with the hardware labels.
2. **Phase 4 button:** "Import flash image..." in the editor -> run the `make_factory` steps (boots gearmulator-md-mm in a background
   job) -> writes `factory/FACTORY.syx` and `factory/ROM_SAMPLES.bin` to the data folder (`~/.config/Machinemodule/` etc.; the plugin
   already loads them if present).
3. Pan on the track outputs (pan law is in `Mixer`); confirm the track-output level vs hardware (about 6 dB hotter than the track's
   share of Main; clips on very loud machines at high VOL).
4. CI/builds for macOS (universal), Windows, Linux; check arm64 JIT (the `interruptRegionIsCode` fix was only seen on x86-64).
5. Known small gaps: note timing is to the 32-frame engine block; residual -71 dB vs the unit is a tiny attack-time/LFO-phase
   difference, not understood further; TRX CB / TRX S2 noise differs between JIT and interpreter (timing-dependent seed).

## Gen 2 MPC (later)

Engine, MasterFx and the verification tools carry over unchanged; what is Force-specific is the shell (VST2 wrapper from
`mpc-vst-plugins`, `vst/engine.cpp` threading, skin, installer, taps, armhf Docker toolchain). Plan: probe the device (CPU/arch,
MPC OS, plugin host, can a process map executable memory?), build `md-hash`/`md-multi` for aarch64 and run over SSH, try the dsp56300
aarch64 JIT (fallback: static recompiler built for 64-bit, then interpreter), measure the 16-track load before deciding threads, then
reuse the wrapper with the Gen 1 limits off (master FX on, no voice budget, no silence release, 16 taps) and the Phase 3 UI source.
Needs from the user: which Gen 2 devices, and SSH access to one.
