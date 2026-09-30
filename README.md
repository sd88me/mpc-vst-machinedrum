# Machinedrum Module for MPC OS

The Elektron Machinedrum SPS-1 UW sound engine as a native VST2 instrument for Akai MPC OS standalone devices
(built and tested on the Force), with its own touchscreen skin and Q-Link support. All 16 Machinedrum tracks play
from one plugin instance, using the Machinedrum's own DSP code and its own machine, LFO and mixer maths.

<img width="640" height="400" alt="image" src="https://github.com/user-attachments/assets/30d1c99b-333c-4b98-b9e5-2c3339ef0c29" />

**v0.3.0.** It plays, saves and reloads with the project, and it is tested on a real Force (MPC OS 3.9.1). There is
no downloadable build: it needs your own Machinedrum firmware, so you build the installer yourself with one script (see
[Building](#building)). The Machinedrum's master effects are not built in: the reverb and delay sends come out through the new taps instead (see [Taps](#taps-each-track-or-send-on-its-own-mpc-track)).

**New in 0.3.0:** **Machinedrum Tap** and **Machinedrum Tap FX**: extra plugins that put any tracks, and the reverb and delay sends, on their own MPC tracks, submixes or return tracks, so MPC's mixer and effects can process them. Tracks and sends can be mixed freely on one tap. Timing against the main output was measured on a Force (sample-aligned). The Module itself is unchanged, and its output is bit-identical when no tap is in use.

**New in 0.2.1:** build fixes only. The build no longer needs Monomodule's art file (the LCD fonts come from your Machinedrum OS, and the
randomise toggle icon is now drawn by this project), `mdProbe` is built for you, and the README has plain steps including a macOS setup.
The skin's look is the same apart from two small digits and that icon. If you do have mpc-vst-monomodule's `vst/build/art.json` next to this repo (or `MNM_ART` pointing at it), the skin build uses it and gets the original toggle icon and digits.

**New in 0.2.0:** the voices now render on two threads, which is the main reason a busy kit holds up better; the VOICES
budget really cuts voices now (it did nothing before); TRX XT, CP, MA, CL and XC and EFM CY now play (they were silent);
ROM machines are off until you switch them on; new instances start at VOICES 4 with an 8.7 ms buffer. Every machine the plugin
offers is checked to make sound as part of the build.

Not affiliated with Elektron. Nothing of Elektron's is in this repository or distributed from it; the plugin
needs your own Machinedrum OS 1.63 file and a flash image (see [What you need](#what-you-need)).

## Features

- **16 tracks, one instance.** MIDI notes 36-51 play tracks 1-16 (the Machinedrum's own note-to-track map).
  Each track keeps the Machinedrum's own voice, effects and routing.
- **The machines:** GND, TRX (808-style), EFM, E12, P-I and the ROM sample machines. RAM, INP and MID/CTR machines
  are not offered (no sampling, no audio input and no MIDI output here); a kit that uses one shows a blank bar.
- **Every parameter page of the hardware**, per track:
  - SYN: the eight synth knobs, with the machine's own labels, which change live with the machine.
  - AMP/EFX: AMD, AMF, EQF, EQG, FLTF, FLTW, FLTQ, SRR.
  - ROUTE: DIST, VOL, PAN, DEL, REV and the LFO amounts.
  - LFO: the Machinedrum's per-track LFO, with its destination shown by name.
- **Kits.** The 16 factory kits (extracted from your own flash image at build time), plus any Machinedrum kit
  `.syx` you drop in (see [Kits](#kits)). A new instance starts on the first kit.
- **GLOBAL tab:** a 16-track level mixer, the voice budget, randomise (machines on all, tracks 1-8 or tracks 9-16,
  or a random kit) and the bank and kit selectors.
- **ROM on/off switch** (GLOBAL tab, off by default). Off, tracks on a ROM (sample) machine stay silent and the randomiser leaves ROM
  machines out; each track keeps its ROM setting for when you switch back. ROM machines are the most expensive on the
  Force's CPU, so this is the quickest way to make a busy kit safe.
- **Voice budget, default 4.** The VOICES knob on GLOBAL is a CPU budget, not a plain voice count. Most machines cost
  1 unit and the ROM (sample) machines cost 2, matching what they cost the Force's CPU. When a trigger would go past the
  budget, the oldest sounding track is cut, tail included. Heavy kits (TRX, EFM) play cleanly at 4 on a busy Force; sparse
  patterns or lighter machines can go higher, so raise it until you hear crackle and back off one.
- **Tempo follows MPC's**, so the LFOs stay in time with the project.
- **The skin** is drawn from the Machinedrum's own LCD (fonts, dials, page layout), generated at build time from
  your own firmware, inside a thin hardware-style bezel. Nothing captured from the firmware is stored in the repo.

### Taps: each track or send on its own MPC track

MPC gives a VST2 instrument only one stereo pair of outputs, so the Module cannot offer 16 outputs. Instead, two small extra
plugins read channels from the Module running in the same project:

- **Machinedrum Tap** (an instrument) and **Machinedrum Tap FX** (an effect, for a return or FX track). Each has an on/off
  cell for every track (mono, after the track's effects and VOL, before its pan), for the reverb send and for the delay send.
  Turn on as many as you like: the tap outputs their sum, so one MPC track or submix can carry any set of Machinedrum channels.
  Tap FX also has THRU, which adds whatever the track receives.
- A track that a tap reads leaves the Module's own dry main mix, so it is not heard twice. It still feeds the Machinedrum's
  reverb and delay sends (the track's REV and DEL knobs), which the send taps carry.
- Use them to give each drum its own MPC track, submix and insert effects, or to put the sends on a return track and run
  them through MPC's own reverb and delay. Taps are sample-aligned with the Module (measured on a Force) and cost almost no CPU.
- Needs one Machinedrum Module in the project; a tap is silent without it. Install `machinedrum_one.so`, `machinedrum_tap.so`
  and `machinedrum_tapfx.so` in the same folder.

### Not there yet (known limits)

- **Master effects:** this version bakes in none. The Machinedrum's own master section (rhythm echo, gate box/reverb, EQ,
  dynamix) is not emulated; the reverb and delay sends come out through the taps instead, so MPC's own effects do that job.
  A faithful version with the Machinedrum's master effects built in is planned for more powerful devices (Gen 2 and
  later): it costs about 13 M DSP instructions a second on top of the voices, too much for the current Force.
- **CPU.** The voices render on three threads (three cores) and the track effects run on the same threads. A voice that has
  died away (below -96 dBFS for 100 ms) stops costing anything until it is played again. About 6 voices can sound at once on a
  Force with MPC busy (fewer with ROM machines): a voice costs roughly 0.3-0.9 ms of a 2.9 ms audio block depending on the machine (ROM, P-I
  and EFM cost the most) and on how busy MPC is. Beyond that the plugin crackles, so the voice budget (default 4) is the guard:
  it cuts the oldest sounding track when a new one would go past it. The engine threads run below MPC's own
  audio threads, so overload drops the plugin's own blocks (crackle) rather than MPC's audio or its screen.
- **First load is slower** than later ones (the skin is large: MPC reads and decodes it from the card).
- **ROM machines** are silent unless the sample data was extracted at build time (it is, if you build with your
  flash image). ROM33-48 are empty on the factory image.
- **Latency:** the engine renders 3 blocks (8.7 ms) ahead, which rides out the stalls MPC's own audio threads cause on a busy kit; 2 (5.8 ms) is lower but crackled on the Force.
- Bank and kit are chosen with the arrows for now; a picker list like the machine one is planned for the next
  version.

## What you need

- An MPC OS standalone device (developed on a Force; other MPC OS devices use the same plugin host).
- **Your own Machinedrum OS 1.63 `.syx`** (`Elektron_SPS1-1UW_OS1.63.syx`). This is the sound engine.
- **Your own full flash image** of a Machinedrum UW (8 MB `.bin`), used once at build time for the factory kits and
  the ROM sample memory. Without it you still get every non-ROM machine and any kit `.syx` you add.
- A computer to build on (macOS or Linux; Windows through WSL) with **Docker** running, plus `git`, `cmake`, `ninja` and
  `python3`. That's all: the build fetches everything else itself (the shared VST wrapper, the emulator sources, the
  `mdProbe` tool) and reads the LCD fonts from your Machinedrum OS file, so no other Elektron file is needed.

### On a Mac (one-time setup)

1. Install Apple's command line tools (compiler, git, python3): open Terminal and run `xcode-select --install`.
2. Install [Homebrew](https://brew.sh), then run `brew install cmake ninja llvm`.
3. Install [Docker Desktop](https://www.docker.com/products/docker-desktop/) and **start it** (the whale icon in the menu bar
   must be running before you build). Apple Silicon and Intel Macs both work.
4. Put the two files in a folder whose path has no spaces, for example `~/md/`. In Terminal, drag a file into the window to
   type its path; **keep the quotes and the slashes** as Terminal writes them.
5. The build takes about 25 minutes and the first run also builds `mdProbe` (a few extra minutes). It needs about 10 GB of
   free disk space (Docker images). Keep the Mac awake.
6. macOS-specific notes: the script uses the tools Homebrew installed (`llvm` supplies the `nm` the recompiler step needs).
   If a step fails, copy the last 20 lines of Terminal output into an issue: that is enough to find the cause.
   The Mac path has had less testing than Linux.

## Building

The build reads your firmware and writes an installer zip containing the plugin, the skin, your extracted kits and ROM
samples, and your OS file (the plugin reads it at run time); the result contains firmware-derived code and data, so it is
for your own devices only.

**Where to run this: on your own computer, not on the Force.** The build runs inside Docker on your computer, and so does
the `git clone` below. The Force is only where the finished plugin is installed.

```bash
git clone --recursive https://github.com/sd88me/mpc-vst-machinedrum.git
cd mpc-vst-machinedrum
release/build_release.sh "/path/to/Elektron_SPS1-1UW_OS1.63.syx" "/path/to/flash image.bin"
```

That's the whole build. The result is `dist/Machinedrum-Module-<version>-mpc-armv7.zip`. Put the two file paths in quotes.
To install it on the Force from the same command, add `-d <device-ip>` (see below), or copy the zip over yourself,
unzip it on the device and run `install.sh` as root. Installing stops and restarts MPC, so save your project first and run it
with the device idle.

Options: `-v <version>` (default from `git describe`), `-d <device-ip>` (copy the zip over and run its installer),
`-m <mpc-vst-plugins checkout>` (default: fetched automatically, or `../mpc-vst` if it exists). `MDPROBE=<path>` uses a
ready-built `mdProbe`; otherwise `tools/mdtrace/build_mdprobe.sh` builds it the first time (you can also run that by hand).

The script builds, in order: the x86 helper tools, the factory kits and ROM samples (by booting the emulated MD from your flash
image), the recompiled voice DSP (traced from your OS file), a **bit-exactness gate** (the recompiled DSP must give the same
audio hash as the plain interpreter, ROM machines included, and every machine the plugin offers must make sound, or nothing is
built for the device), the skin, the ARM plugin, and the installer zip.

**If something goes wrong:** the script stops at the first error and says which step (`== 3/7 ...`). Run it again after fixing
what it names; finished work is reused. `HANDOFF.md` has every step's details.

## Kits

Put Machinedrum kit sysex files (`.syx`, the MD's own kit dump) in
`/sdcard/vst/machinedrum/kits/` or in `Force Documents/Machinedrum Kits/`. They are picked up within a few seconds.
Each file is a bank; a file with several kits shows them all. The factory kits are the bank called FACTORY.
Master-effect settings inside a kit are ignored for now.

## Plugin catalog

This is a **build-it-yourself** plugin: the installer zip contains firmware-derived code and data, so it is built per user and
must never be published as a release (a catalog entry for it links to this repo and its build instructions, not to a
download). The zip is still catalog-conformant in format: `mpc-plugin.json` (id `machinedrum-module`, license
`AGPL-3.0-only`, source repo) is generated, and the build runs mpc-vst-plugins' `catalog_check.py --catalog` as its last step.
The plugin locates its data next to the `.so` (`MODULE_SUBDIR`), not at a fixed path. Device testing is recorded in
`tested.json` (v0.3.0: Akai Force, MPC OS 3.9.1; the taps are not yet part of the installer zip: build them with `vst/build_so.sh`, which now builds all three plugins, and copy `machinedrum_tap.so`, `machinedrum_tapfx.so` and their skins by hand).

## How it works

The Machinedrum has a ColdFire processor for the sequencer and UI, and two DSP56303 chips: DSP2 renders the 16
voices and DSP1 runs the per-track effects, the mix and the master effects. We keep only the sound path:

- **Voices:** DSP2's program runs unmodified from your OS file in the [dsp56300](https://github.com/sd88me/dsp56300)
  emulator, with a small harness in place of the hardware's DMA and serial loop. All 16 voices render in one block.
- **The ColdFire side** is a C++ host model: the OS tick (smoothing, LFOs, triggers, mixer inputs) with the OS's own
  machine coefficient routines run in a 68k emulator, so the maths is the hardware's.
- **Per-track effects and the mix** (AMD, EQ, filters, SRR, distortion, pan, sends) are a native C++ translation of
  DSP1's code, checked sample by sample against the DSP running the original.
- **Speed on ARM:** the DSP program is statically recompiled ahead of time (from your OS file, so it is never
  shipped) into C++ that the ARM compiler can optimise.

## Development

The work behind this, in order (`HANDOFF.md` has the full log and `docs/` the protocol notes):

1. **Firmware decoded.** The OS `.syx` unpacks into five sections, two of them complete DSP programs.
2. **Protocol traced.** A patched full-system emulation logged every word between the ColdFire and the DSPs. That
   settled the roles (section 1 on DSP2, section 2 on DSP1), the 16 voice slots and the parameter structures.
3. **Voice engine, bit-exact** against the full-system emulation, then the host model (tick, LFOs, triggers).
4. **Mixer chain translated to native C++**, bit-exact against the DSP for 3.2 M samples of the per-track chain and
   384,000 words of the mix.
5. **ARM port of the DSP emulator.** Found and fixed a missing MERGE instruction, a static-initialisation-order bug in
   the opcode tables and a cache-invalidation gap in the boot loader, and proved the ARM output identical to x86 on the
   real Force.
6. **Static recompiler** for the voice DSP: 294% to 141% of real time for a 6-track demo on the Force.
7. **The plugin:** engine thread with real-time scheduling, 524 parameters, SYN labels that follow the machine,
   project save and restore.
8. **The skin, generated from the real LCD** driven in the emulator: font and dial atlases, page layouts, a machine
   picker, the LFO page, chassis and bezel.
9. **Kits and ROM machines:** kit sysex import (factory kits dumped from the emulated MD), and the sample memory the MD
   copies from its flash at boot.
10. **Performance work on the device (0.1):** skipping settled silent tracks, ring depth and priority tuning, trimming the skin
    and P memory (about 225 MB less RAM in MPC), the voice budget, skipping idle voices in the DSP loop and silent tracks in the mixer.
11. **0.2:** voices on two threads with the track effects on the same threads, cores chosen by how busy MPC keeps them, a voice
    budget that really cuts, cost-balanced voice groups, and fixes for machines that stayed silent (found by comparing every
    machine against the emulated Machinedrum).

## Credits

- **Elektron**, for the Machinedrum. All firmware and sound belong to them; this project ships none of it.
- **shnolk's [Monomodule](https://github.com/shnolk/monomodule)**: the approach this follows (the machine's own
  DSP in an emulator, a host model for the main processor, plugin and skin design), the OS-file decoder adapted in
  `tools/mdfw`, and the Elektron LCD fonts and dial art the skin borrows.
- **[gearmulator-md-mm](https://github.com/joelanders/gearmulator-md-mm)** by Joe Landers: a full-system Machinedrum
  emulation, used as the tracing and correctness reference, and the Musashi 68k core inside it, used by the machine
  routines. Not part of the plugin.
- **[dsp56300](https://github.com/dsp56300/dsp56300)**, the DSP56300 emulator this builds on (our fork,
  [sd88me/dsp56300](https://github.com/sd88me/dsp56300), adds the ARM interpreter and static recompiler work and the
  fixes above), and its bundled asmjit.
- **[mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins)**: the shared VST2 wrapper, skin toolchain and
  installer for MPC OS.
- Built with [Claude Code](https://claude.com/claude-code).

Licensed AGPL-3.0-only, like Monomodule (whose decoder is adapted here); dsp56300 is GPL-3.0.
