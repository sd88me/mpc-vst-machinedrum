# Machinemodule for the desktop (VST3, AU, Standalone)

The Elektron Machinedrum SPS-1 UW sound engine as a plugin for your DAW: the same engine as the MPC version (`../engine`), in a
JUCE plugin whose framework follows shnolk's [Monomodule](https://github.com/shnolk/monomodule). Plan and status:
[../docs/PLAN-desktop-vst3.md](../docs/PLAN-desktop-vst3.md). **Phase 1**: it plays, with a basic editor. Not yet: the Machinedrum's
master effects, the LCD editor, ROM samples/factory kits import (Phases 2-4).

Not affiliated with Elektron. The plugin contains nothing of Elektron's: it needs your own **Machinedrum OS 1.63** file
(`Elektron_SPS1-1UW_OS1.63.syx`, a free download from Elektron) and reads it when it starts.

## Using it

1. Put the plugin where your DAW looks (`~/.vst3/` on Linux, `C:\Program Files\Common Files\VST3\` on Windows,
   `/Library/Audio/Plug-Ins/VST3/` on macOS) and rescan.
2. Open it, click **Select OS file...** and choose the `.syx`. It loads in a second or two and remembers the file.
3. MIDI notes **36-51** play tracks 1-16 (notes 0-15 do too). **Load kit file...** loads a Machinedrum kit `.syx`
   (`<` `>` step through the kits in the file). Every knob of every track is a host parameter (raw 0-127, like a kit byte).
4. Outputs: **Main** is the dry main mix. **Track 1-16** are extra stereo outputs (enable them in your DAW's plugin routing):
   each carries one track after its effects and VOL (no pan, same signal left and right). A track whose output is enabled
   leaves the Main mix.

Data folder (`~/.config/Machinemodule/`, `%APPDATA%\Machinemodule\`, `~/Library/Application Support/Machinemodule/`):
`os_path.txt`, and later `factory/FACTORY.syx` (kits) and `factory/ROM_SAMPLES.bin` (ROM machines), both picked up if present.
For testing without the editor, the environment variable `MM_OS_SYX` names the OS file.

## Building

```bash
git submodule update --init --recursive
cmake -S desktop -B desktop/build -G Ninja -DCMAKE_BUILD_TYPE=Release     # downloads JUCE 8.0.9
cmake --build desktop/build --target Machinemodule_VST3
```

Linux needs the JUCE development packages (`libasound2-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev
libxcomposite-dev libxext-dev libxrender-dev libfreetype-dev libfontconfig1-dev libgl1-mesa-dev`). macOS and Windows build
the same way (Visual Studio 2022 on Windows) but have not been tried yet.

## Testing

`cmake --build desktop/build --target MmRender` builds `mm-render <OS.syx> [out.wav]`: plays notes through the processor and
checks that (a) at 44.1 kHz the main output is sample-identical to the bare engine, (b) at 48 kHz the level matches, (c) a track
on its own output leaves the main mix and appears on its output. The VST3 passes
[pluginval](https://github.com/Tracktion/pluginval) at strictness 5 (`pluginval --strictness-level 5 --skip-gui-tests`).

## Licence

AGPL-3.0-only, like the rest of this repository and Monomodule. `src/core/Resampler.*` is Monomodule's, unchanged.
