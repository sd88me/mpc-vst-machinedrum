# Handoff: v1.0.0 (adopting ModuleDrum's ideas, master effects, CTR) - state at 2026-10-30

Read `HANDOFF.md` first for the project. This file is the current state of the v1 workstream, rewritten as one document (earlier versions were an append-only log).

**Nothing is tagged, released, or opened as a pull request.** The user is testing the build on their Live 3 first; **wait for their report before building the public zips or releasing.**

## Where things are

| What | Where | State |
|---|---|---|
| MPC plugin work | branch **`v1`** (pushed), on top of `origin/main` (v0.5.1 + dsp-bump, `de3731e`) | `git log origin/main..v1`: engine, plugin, skin, installer fix, README, CTR plugin, CTR skin, handoffs |
| Desktop version (JUCE VST3/AU/Standalone) | branch **`desktop-improvements`** (pushed), base `desktop`; worktree `/home/sam/mpc-vst-machinedrum-desktop` | mixer solo and pan, mute groups, footer description, tests, README |
| Old base work | local branches `md-improvements`, `backup-md-improvements-wip` | superseded by `v1`, safe to delete after the merge |
| Live 3 (`192.168.2.217`, aarch64, MPC OS 3.x, busybox) | `/nvme/Synths` | **per-user v1.0.0 with CTR installed**: `Machinemodule` (`machinedrum_one.so` md5 `289af7e2...`) and `Machinemodule Tap` (the effect; `machinedrum_tapfx.so` md5 `a964611b...`). No B. |
| Force (`192.168.1.44` / `.187`) | - | unreachable; no Gen 1 (armhf) plugin built or tried |

The other worktrees (`/home/sam/mm-main`, `/home/sam/mm-pickers`) are the user's, not part of this work.

## What v1 adds (all on `v1`)

Ideas and code adapted from **ModuleDrum** (github.com/K-X420/ModuleDrum, AGPL-3.0, by KX; a fork of Shnolk's Monomodule). Credited in the README and the source files. Never run its scripts (the auto-mode classifier refuses); read its sources.

| Feature | Where | Verified |
|---|---|---|
| **Mute groups** (a trigger zeroes the target's mixer words until its own next trigger, MainOS `$20b3b0`) | `engine/HostModel.*`; kit parse in `vst/engine.cpp` (kit bytes at `$4a7`); state format **v2** (v1 projects still load) | `MD_MUTEGROUP=1 md-hash`; `md-hash` unchanged `04ef1db4fe767721` |
| **Master effects** (reverb, delay, EQ, dynamix), Gen 2 only, on by default | `engine/MasterFx.*` (DSP1 `$342-$9A8` on its own DSP, fed by the native mixer), `MachineRunner::runTo` (the OS's own knob conversion on the 68k), `EngineT::setMasterFx`; params `master_fx`, `rv1-8 dl1-8 eq1-8 dx1-8`; `kMasterFxSupported` false on 32-bit ARM | **`md-mastertest`: 0 mismatches vs the full mixer DSP, 400 random blocks**; bench on the Live 3: +45 us on a 725 us block (6%) |
| **Mixer page** (GLOBAL tab): per track PAN, LEV, meter, M, S | skin `mk_skin.py`; params `trackN_mute/solo/meter`; `meter[]` and `display_rev` in `vst/engine.cpp` (vst.json `HAS_DISPLAY_REV`) | user confirmed on the Live 3: buttons toggle once, PAN Q-Link page, meters. **Solo logic not yet confirmed.** |
| **FX tab** (after GLOBAL) and **MST FX** switch | skin | previews; skin check |
| **Machine logos** (the OS's own five family badges, read from the user's OS file) and **description lines** (ModuleDrum's wording) | `md_art.h`, `mdartdump`, `md_logos.py`, `md_machine_text.py` | previews; public zip has no Elektron content (see below) |
| **Q-Link pages**: mixer LEV, PAN, GLOBAL, FX; outlines through upstream's `qlink_boxes` (extended to take rectangles) | skin | user confirmed on the Live 3 |
| **CTR machines** (Gen 2): CTR-AL, CTR-RE/GB/EQ/DX, CTR-8P | `vst/engine.cpp`: `ctrSynMoved`, `ctrDefaults`, `ctrResync`, the `ovr` override table in the engine thread; skin: CTR column in the picker, `ctr8p_overlay` over the AMP and ROUTE grids | **`md-ctrtest` ALL OK** (CTR-AL +10/-10 and clamp, the four master machines, CTR-8P defaults, audio: a driven track is silent at 0 and has the same peak as the plain one at 100). **Not tried on the device.** |
| **Gen 1 skin** option: `mk_skin.py gen=1` (no FX tab, no MST FX, no CTR); `release/build_release.sh` passes it for `-a armhf` | skin, release script | both skins built and compared; **armhf plugin not built, Force not tested** |
| **Installer fix**: per-user zip lists `machinedrum/` as user data, so an upgrade keeps the user's flash image and kits | `release/build_release.sh` (`--user-data machinedrum`) | **confirmed on the Live 3**: the flash image survived an upgrade untouched. (Packaged OS file and factory kits are only put in place on a first install.) |
| **A/B build** support, kept for the future | `vst/vst_b.json`, `MD_VARIANT=b vst/build_so.sh`, `mk_skin.py name=Machinemodule_B` | worked; not installed now |

CTR rules (ported from the desktop version, which checked them against the OS): CTR-AL moves SYN k of every other sounding track by the delta of its own SYN k (clamped 0-127; CTR, MID, and RAM-record tracks left alone); CTR-RE/GB/EQ/DX are SYN1-8 of the delay/reverb/EQ/dynamix master knobs; CTR-8P writes P1-P8 (its SYN1-8) every block into the (track, parameter) targets its raw parameters 9-24 name (the target's own knob keeps its value), and a new CTR-8P points P1-P8 at SYN1 of tracks 1-8. Rules run on host parameter moves only 1.5 s after the instance is made and only when a value changed (a host that restores value by value must not run them); kit and state loads resync. Known limit: the CTR-8P target cells show the OS's raw 0-based values (TRK 0 = track 1, PRM 0 = SYN1).

**Desktop (`desktop-improvements`)**: mute groups (kit parse, `tNN_mutegroup`, `HostModel::setMuteGroup`), mixer view with PAN strip, LEV + meter, M and S (solo is `tNN_solo`, applied as a mute in `EngineFeeder`), machine description in the footer (`ui/MachineText.h`). Built in Docker (`md-desktop-builder` image: ubuntu 24.04 + build-essential cmake ninja-build git python3 xvfb ca-certificates curl + the JUCE dev packages libasound2-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxcomposite-dev libxext-dev libxrender-dev libfreetype-dev libfontconfig1-dev libgl1-mesa-dev; run with `--shm-size=4g`), `MmRender` = ALL OK with new tests for mute groups and solo; `MmSnapshot` under xvfb renders the editor. Its own master effects are more faithful than ours (it runs the OS's time-sliced conversion routines and the OS smoother in `HostModel`, so knob changes glide); `engine/MasterFx.*` there has a different API, so merging the branches conflicts.

## Pending, in order

1. **The user's device test (Live 3).** Things not yet seen: master effects by ear (the output is the hardware's own level, about 15 dB above the old dry mix: turn the track fader down first; clicks when turning master knobs, since our conversion does not apply the OS smoother), the CTR machines, solo, CPU with 16 tracks playing (`touch /tmp/md-stats-on`, stats file `/tmp/md-stats.<pid>`; `Engine::masterUs` is timed but not printed).
2. **After the test:** rebuild the public zips from `v1` (`release/build_release.sh <OS.syx> -P -v 1.0.0 -m ~/mpc-vst-plugins`, about 25 min; a pre-CTR public build passed catalog_check and was checked for Elektron content: about 1,600 samples of the OS file found nowhere, only the filename string in the `.so`, `art.bin` is the skin recipe), open pull requests (`v1` into `main`, `desktop-improvements` into `desktop`), tag `v1.0.0` (the catalog needs X.Y.Z), release (see `HANDOFF.md`, the "v0.5.1" section, for how), update `tested.json` with what was actually tested.
3. **Gen 1 (Force):** build armhf with `release/build_release.sh <OS.syx> <flash.bin> -a armhf` (recompiler steps, about 30 min; needs `libs/gearmulator-md-mm/build/.../mdProbe`), deploy to the Force, check the 10 Hz `display_rev` poll and meter cost there.
4. **Decisions deferred:** a master-effects trim parameter (the DSP saturates internally, so a trim after it only lowers the ceiling); whether the Tap should carry the reverb/delay sends when the master effects are on (today it still carries the dry sends); porting the desktop OS-smoother conversion to the MPC engine if clicks are audible; CTR-8P cell display (1-based would need dedicated dial strips).
5. **Not done from the original review:** compare ModuleDrum's idle-voice skip (`MdMixEngine.cpp`) with our silence release (`HostModel::setSilenceRelease`); INP/MID/RAM machines still have no logo or bar; the picker header logos are smaller than the "GND" text.
6. **Desktop:** only built and run on Linux (no macOS, Windows, or real DAW); footer version string still `V0.2.1`.
7. **Housekeeping:** `vst/params.lock` and `vst/tapfx/params.lock` are generated, untracked, and **a stale one breaks the build** (delete them only after checking `params.json` still extends the shipped order; it does today: 672 keys, upstream's 591 are an unchanged prefix); submodule pointers `libs/dsp56300` and `libs/gearmulator-md-mm` show as modified (local state, not committed); delete the superseded local branches; the device's `/tmp/v1`, `/tmp/v2` and old installers vanish on reboot; the session scratchpad holds a copy of the user's flash image and the pre-CTR public zips.

## Commands (x86; the OS file is `Elektron_SPS1-1UW_OS1.63.syx`, e.g. `/tmp/os.syx`)

```
cd build-vst-x86 && cmake . && ninja md-hash md-mastertest && cd ..
./build-vst-x86/md-hash /tmp/os.syx                        # hash 04ef1db4fe767721
MD_MUTEGROUP=1 ./build-vst-x86/md-hash /tmp/os.syx         # mute group
MD_MASTER=1 MD_TIME=1 ./build-vst-x86/md-hash /tmp/os.syx  # block timing against the 725 us budget
./build-vst-x86/md-mastertest /tmp/os.syx 400              # master effects vs the full DSP1: 0 mismatches
mkdir -p build-vst-x86-plugin && cd build-vst-x86-plugin && cmake -G Ninja -DMPC_VST_DIR=$HOME/mpc-vst-plugins -DCMAKE_BUILD_TYPE=Release .. && ninja machinedrum_one md-vst-smoke md-ctrtest
./md-ctrtest <dir holding the OS .syx>                     # the CTR machines: ALL OK
(cd vst && python3 -I gen_params.py)                       # regenerate params.json first (append only)
tools/mdskin/build_skin.sh /tmp/os.syx ~/mpc-vst-plugins   # the Gen 2 skin; add gen=1 as extra arguments for Gen 1
# the Gen 2 skin recipe check (after any skin change, about 10 min): see release/build_release.sh step 5, tools/mdskin/skin_recipe.py make
release/build_release.sh <OS.syx> <flash.bin> -a aarch64 -v 1.0.0 -m ~/mpc-vst-plugins    # per-user build (about 5 min)
release/build_release.sh <OS.syx> -P -v 1.0.0 -m ~/mpc-vst-plugins                        # the public download
```

Installing on the Live 3 (the default `/sdcard/Synths` is read-only there): copy the zips to `/tmp/<dir>` and unzip, then in one ssh: `systemctl stop acvs`; for each zip `sh install.sh -y -n -t /nvme/Synths`; `systemctl start acvs`. **Ask the user before stopping MPC** (it drops unsaved work; MPC does not reload a plugin `.so` it already has, and skins load only at startup). The installer's `uninstall.sh` takes the same flags.

## Gotchas that cost time

- Parameters are **append only** (`vst/params.json`, VST index = order; saved projects, automation and skins bind by index). Add after the last one.
- The skin generator: each pixel of the Gen 2 recipe may depend on at most one art bit (no layout that depends on the art, e.g. trimming a badge by its lit columns). Run the recipe check after skin changes.
- A busybox device: no `head -N`, no `unzip -l`.
- An upgrade from a package without `--user-data` deletes the data folder; the per-user build now has it.
- The auto-mode classifier refuses force-pushes, amending pushed commits, and running third-party scripts: make a new commit instead.
- Git: `v1` was made by replaying the old-base work (`git cherry-pick -n` of one WIP commit) onto `origin/main`, with conflicts resolved in favour of upstream's own Q-Link outlines, bank/kit lists, `rom=0` and VOICES 16, and my pages added to their mechanisms.
- A first note after a machine change is quieter than later ones (seen in `md-ctrtest`); it is not a CTR effect.
