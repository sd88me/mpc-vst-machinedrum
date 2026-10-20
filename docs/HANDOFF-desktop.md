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
| 3 LCD editor | **version 2 done (2026-10-20), after shnolk's desktop Monomodule**: `desktop/src/ui/` (ported from `libs/monomodule/src/plugin/one/`, AGPL, attributed in each header): `RomArt` (fonts, dial ring and 128 pointer frames read from the user's OS file at the addresses `tools/mdskin/mdartdump.cpp` uses; stand-in face until then), `Lcd`, `Skin` (default = the MD's red LCD; also black/white, inverted, low contrast, custom), `Widgets` (KnobPage, MachineBar, LevelColumn, TrackPanel, MixerPage, KitStrip), `MachinePicker` (one column per group from the OS machine table), `MdEditor`. Layout = Monomodule One/Six: header (mark, machine block, kit strip, BPM, MIXER / MASTER / MENU), LEV column, pages SYN / AMP-EFX over ROUTE / LFO, 16-track panel in place of his third page column. MIXER shows the 16 LEV columns; MASTER swaps the four pages for the master effects. |
| 4 flash-image import (ROM samples, factory kits) | **done 2026-10-20 (Linux)**: MENU > "Import Flash Image..." runs `md-import` (`tools/mdimport/md_import.cpp`: runs `mdProbe` then `mdsamples`, writes `FACTORY.syx` + `ROM_SAMPLES.bin` to `<data dir>/factory`, byte-identical to `tools/mdkits/make_factory.py`) in the background and reloads (`MdProcessor::importFlashImage`; test: `MmSnapshot --import-test <OS.syx> <flash.bin>` with `MACHINEMODULE_DATA_DIR` and `MM_IMPORT_HELPER`). With the data: 16 factory kits, ROM machines sound, `md-hash` with the samples = `e2b514e70c173c33`. **Helpers ship (2026-10-20):** the desktop build copies `md-import` and `mdsamples` next to the plugin binary and standalone (and `mdProbe` when configured with `-DMM_MDPROBE=<path>`); `tools/mdimport/build_helpers.sh` installs all three in the data folder. Windows: CI builds `mdProbe.exe` (static runtime, depends only on KERNEL32) and puts all three in the VST3 bundle and next to `Machinemodule.exe`. **Open:** the import has not been run on Windows or macOS (macOS `mdProbe` is not built in CI yet). |
| 5 INP and CTR machines | **done 2026-10-20 (Linux x86, offline)**. INP: `VoiceEngine::setInput` (one 32-frame stereo block per render; the plugin's optional "Input" bus -> `MdProcessor::pushInput/engineInput`). The voice DSP reads the ADC from X:$100-$1ff (a 256-word ring filled by the emulated ESSI1 DMA, channel 1 - which runs on emulated time unrelated to the blocks), 96 words behind the block-aligned position it takes from X:$256 (the harness's replaced block-end code `$e2` used to store `DDR1 & ~$3f` there); `setInput` writes DCR1 = 0 and fills the ring itself and sets X:$256 (position steps 64 a block). A = left (even words), B = right. Test: `md-input` (sine left only -> INP-GA 441 Hz, INP-GB silent, and the reverse; no `setInput` call = silent and the hashes are unchanged: `e2b514e70c173c33`) and `mm-render` (g) through the processor at 44.1 and 48 kHz. CTR (the OS's own rules, decoded from the 68k code with a Musashi disassembly; the plugin does them on its parameters in `MdProcessor`): **CTR-AL** `$207e0e`: a SYN k change of delta (new minus the knob as stored; stored = new clamped to 1-126) adds delta to SYN k of every track that is not CTR/MID and not machine 160/161/165/166, clamped 0-127; **CTR-8P** `$2069bc`/tick `$20ae56`: SYN k (P k) is written into the (track, parameter) given by its parameters 8+2k, 9+2k (clamped 15 / 23) every tick, except into CTR tracks; defaults (OS `$246278`): P1-P8 -> SYN1 of tracks 1-8; **CTR-RE/GB/EQ/DX** (120-123, `$20ca14`, table `$2462e3` = 1,0,2,3): SYN k = master FX parameter `block*8+k` (block: echo 1, reverb 0, EQ 2, dynamix 3), starting at the current master values. CTR-8P's AMP/EFX and ROUTE pages become TARGETS 1-4 / 5-8 (`spec::kCtr8Pages`). Test: `mm-render` (f). **Not done:** MID (MIDI out) and RAM (RAM-R/RAM-P: the recording RAM, the DSP's `x:$600` ring) are still hidden; the CTR-8P target knob does not show the driven value; INP under the Gen1/MPC build is not exposed (no input on the Force plugin). |
| 6 Library, release CI | not started |

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
/tmp/b-desk/MmRender_artefacts/Release/MmRender OS.syx          # expect ALL OK (it uses an empty data folder: MACHINEMODULE_DATA_DIR)
# pluginval: https://github.com/Tracktion/pluginval/releases/latest/download/pluginval_Linux.zip
pluginval --strictness-level 5 --validate-in-process --skip-gui-tests --timeout-ms 120000 <...>/Machinemodule.vst3
```

mdProbe notes: time is in audio frames (the OS tick is ~every 352 frames; `adv:` does not run it, `wait:` does; allow >= 25000 frames
for the smoother to settle); `wav:PATH` records the main out; `trig:1:N` is an *accented* panel trigger; new actions this work
added: `poke8:ADDR:V[:V...]`, `dspmem:DSP:X|Y|P:FROM:TO`. MD_ROM=<ROM_SAMPLES.bin> makes `md-mfxref` load ROM samples.

## Container build (no sudo needed), added 2026-10-20

`docker build -t md-desktop-builder` from a plain `ubuntu:24.04` with build-essential cmake ninja git python3 xvfb and the JUCE dev packages
listed above. Mount the repo at /r and a build dir at /b. **Run anything that boots the engine with `--shm-size=4g`**: the JIT's memory
tables use shared memory and Docker's 64 MB /dev/shm makes the process die with SIGBUS in `MasterFx::MasterFx` (it is not a code bug).
Screenshots: `xvfb-run -a MmSnapshot <OS.syx> <outdir> [width]` (target `MmSnapshot`, `desktop/tests/ui_snapshot.cpp`: track, mixer, master and picker views).
The reference: shnolk's own desktop UI can be rendered with his tool (`cmake -S <mpc-vst-monomodule>/libs/monomodule -B /b -DMNM_BUILD_TESTS=OFF -DMNM_COPY_PLUGINS=OFF`, build `MnmUiSnapshot`, run `MnmUiSnapshot --one out.png <Monomachine OS.syx>` and `--one-picker`) - build it in a container with the source mounted read-only.

## Next steps (suggested order)
0. (done 2026-10-20: mute/lock keys on the track panel, pluginval 5 passes with the new editor, `.github/workflows/desktop.yml` builds Linux/Windows/macOS-arm64 - written but never run: push and see; the Windows MSVC and Apple Silicon JIT builds are the likely trouble spots.)
   Design note: the track outputs stay the MD's individual outputs (mono, no pan, `sample x VOL << 4`), so they are hotter than the track's share of Main and can clip at high VOL, as on the unit; stems that sum to Main (panned stereo, 34 engine channels) were not built.
   Phase 3 leftovers (master labels now come from the OS table at `$247cca`, 2026-10-20): mute/lock keys
   on the track panel; the "Import flash image..." menu item (done, Phase 4); a window scale option; try it in a real DAW.

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
CI status 2026-10-20: Linux x64, macOS arm64 and Windows x64 all build (run 39366601043). The Windows fixes were in the dsp56300 fork (`recomp-fixes`: recompiler helpers guarded with DSP56K_RECOMP; `writeMem/readMem<Inst, MMM, nullptr>` for MSVC) and `cmake/dsp56300.cmake` (vtuneSdk on Windows). The first-run screen was rebuilt after a Windows test showed overlapping text.
2026-10-20: `md-hash` and `md-multi` cross-build cleanly for aarch64 (JIT build, `g++-aarch64-linux-gnu` in an ubuntu:24.04 container, `-DCMAKE_TOOLCHAIN_FILE` with `CMAKE_SYSTEM_PROCESSOR aarch64`), but they were not run: this machine has no arm64 emulation and the only device reachable (192.168.1.187) is the 32-bit Force (armv7l, 2 cores), not a Gen 2. Run them on a Gen 2: expect hash `04ef1db4fe767721` (and `e2b514e70c173c33` with the ROM samples).
