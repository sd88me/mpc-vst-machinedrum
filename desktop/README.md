# Machinemodule for the desktop (VST3, AU, Standalone)

The Elektron Machinedrum SPS-1 UW sound engine as a plugin for your DAW: the same engine as the MPC version (`../engine`), in a
JUCE plugin whose framework and editor follow shnolk's [Monomodule](https://github.com/shnolk/monomodule). Plan and status:
[../docs/PLAN-desktop-vst3.md](../docs/PLAN-desktop-vst3.md), [../docs/HANDOFF-desktop.md](../docs/HANDOFF-desktop.md). It plays all 16
tracks, has the Machinedrum's own master effects (rhythm echo, gate box/reverb, EQ, dynamix), and an editor drawn from the Machinedrum's
own LCD glyphs and fonts, which the plugin reads out of your OS file. Built and tested on Linux x86-64 so far.

Not affiliated with Elektron. The plugin contains nothing of Elektron's: it needs your own **Machinedrum OS 1.63** file
(`Elektron_SPS1-1UW_OS1.63.syx`, a free download from Elektron) and reads it when it starts.

## Using it

0. **Standalone:** the `Machinemodule` app (target `Machinemodule_Standalone`) is the same plugin in its own window with your audio and MIDI devices.
1. Put the plugin where your DAW looks (`~/.vst3/` on Linux, `C:\Program Files\Common Files\VST3\` on Windows,
   `/Library/Audio/Plug-Ins/VST3/` on macOS) and rescan.
2. Open it. The first time it asks for your OS file: choose the `.syx`. It loads in a second or two and remembers the file.
3. The editor: the machine block (click it for the machine picker), the **KIT** strip (`<` `>` step through the kits, click the name for
   the list), the **LEV** column and the four pages of the selected track (SYN, AMP/EFX, ROUTE, LFO). The **TRACKS** panel on the right
   selects a track; each cell has an **L** key (lock: the track keeps its sound when a kit loads) and an **M** key (mute). **MIXER** shows
   the 16 levels with meters; **MASTER** shows the master effects (reverb, echo, EQ, dynamix) with the **FX ON** switch. Drag a knob
   (shift = fine), click its number to type a value, double-click to reset. **MENU**: select the OS file, load a kit file, import a flash
   image, choose the skin (the Machinedrum's red LCD by default), engine status.
4. MIDI notes **36-51** play tracks 1-16 (notes 0-15 do too). Every knob of every track is a host parameter (raw 0-127, like a kit byte).
5. Outputs: **Main** is the dry main mix with the master effects. **Track 1-16** are extra outputs (enable them in your DAW's plugin
   routing): each carries one track after its effects and VOL, the same signal left and right, like the Machinedrum's own individual
   outputs (mono, no pan). A track whose output is enabled leaves the Main mix; a muted track is silent on both.

### Input and control machines

- **INP machines** (INP-GA/GB, FA/FB, EA/EB) process the Machinedrum's audio input. Enable the plugin's **Input** bus in your DAW (a sidechain
  or aux input; it is off by default): the left channel is input A (INP-GA, FA, EA), the right channel is input B (GB, FB, EB). They gate or
  filter-envelope the input when the track is triggered, like the hardware. (At 48 kHz and up the input is resampled with linear interpolation.)
- **CTR-AL** shifts SYN1-8 of every other sounding track by the amount its own knob moves (as the OS does: relative, clamped to 0-127; the
  knobs on the other tracks move). **CTR-8P** sends P1-P8 to eight (track, parameter) targets, set on its two TARGETS pages (TRK / PRM);
  they take its values continuously. **CTR-RE / GB / EQ / DX** are SYN1-8 of the echo / reverb / EQ / dynamix master effects.
- Still not offered: MID (MIDI output) and RAM (RAM recording and playback).

### ROM machines and factory kits

The OS file holds every machine except the ROM sample machines, and the 16 factory kits are not in it either: both come from the flash of a
Machinedrum UW. **MENU > Import Flash Image...** reads your own 8 MB flash image once (about a minute: it boots the emulated Machinedrum) and
writes the factory kits and the ROM samples to the data folder. It runs the `md-import` helper, which must sit in `<data folder>/bin`
(or next to the plugin binary) together with `mdProbe` and `mdsamples`. Configure the build with `-DMM_MDPROBE=<mdProbe>` and they are copied
next to the plugin and the standalone app automatically; otherwise run `tools/mdimport/build_helpers.sh` once (builds all three, installs them in
the data folder; no JUCE needed). Without the import everything else works and kit `.syx` files load.

Data folder (`~/.config/Machinemodule/`, `%APPDATA%\Machinemodule\`, `~/Library/Application Support/Machinemodule/`):
`os_path.txt`, `settings.xml` (skin), `factory/FACTORY.syx` and `factory/ROM_SAMPLES.bin` (from the import), `bin/` (the helpers).
`MACHINEMODULE_DATA_DIR` overrides the folder (tests); `MM_OS_SYX` names the OS file without the editor.

## Building

```bash
git submodule update --init --recursive
cmake -S desktop -B desktop/build -G Ninja -DCMAKE_BUILD_TYPE=Release     # downloads JUCE 8.0.9
cmake --build desktop/build --target Machinemodule_VST3
```

Linux needs the JUCE development packages (`libasound2-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev
libxcomposite-dev libxext-dev libxrender-dev libfreetype-dev libfontconfig1-dev libgl1-mesa-dev`). Without root, build in a container
(an `ubuntu:24.04` image with those packages, the repo mounted at `/r`, a build folder at `/b`); run anything that starts the engine
with `--shm-size=4g`, because the JIT's memory tables use shared memory (Docker's default 64 MB makes it die with SIGBUS).
CI (`.github/workflows/desktop.yml`) builds Linux, macOS (Apple Silicon) and Windows (Visual Studio 2022); the Windows build also builds `mdProbe` with the static runtime and ships the three import helpers inside the plugin bundle and next to the standalone app. Only Linux has been run; the macOS and Windows builds have not been tried by hand yet.
The helpers: `cmake --build <build> --target md-import mdsamples` in the repo root build, and `tools/mdtrace/build_mdprobe.sh` for `mdProbe`.

## Testing

- `MmRender <OS.syx> [out.wav]` plays notes through the processor and checks that (a) at 44.1 kHz the main output is sample-identical to
  the bare engine, (b) at 48 kHz the level matches, (c) a track on its own output leaves the main mix and appears on its output, (d) a muted
  track is silent. It uses an empty data folder, so installed factory data does not change the result.
- `MmSnapshot <OS.syx> <outdir>` renders the editor (track, mixer, master and picker views) to PNGs; `MmSnapshot --import-test <OS.syx>
  <flash.bin>` runs the flash import through the processor (set `MM_IMPORT_HELPER` and `MACHINEMODULE_DATA_DIR`).
- The VST3 passes [pluginval](https://github.com/Tracktion/pluginval) at strictness 5.

## Licence

AGPL-3.0-only, like the rest of this repository and Monomodule. `src/core/Resampler.*` is Monomodule's, unchanged; the files in `src/ui/`
are adapted from Monomodule's editor (`src/plugin/one/`) and say so in their headers.
