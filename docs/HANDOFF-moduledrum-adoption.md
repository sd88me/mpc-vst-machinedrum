# Handoff: adopting ModuleDrum's improvements (written 2026-10-30)

Read `HANDOFF.md` first for the project; this is the state of one workstream.

**Branches (2026-10-30):** `v1` = this work replayed onto `origin/main` (v0.5.1) and committed in four logical commits (engine, plugin, skin, docs); the work was first
done on `md-improvements` off an older main (kept locally as `backup-md-improvements-wip`, one WIP commit). Conflicts with upstream's own Q-Link outlines (`qlink_boxes`),
bank/kit lists, `rom=0` and VOICES 16 were resolved in favour of upstream, with my pages added to its mechanism. The desktop version is on branch `desktop-improvements`
(base: `desktop`). Planned release name: **v1.0.0** (catalog needs X.Y.Z); not tagged or released yet.

## What this is

[ModuleDrum](https://github.com/K-X420/ModuleDrum) (AGPL-3.0, by KX) is a desktop AU/VST3/standalone fork of Shnolk's Monomodule that emulates the
Machinedrum. We reviewed it against this repo (the MPC VST2 plugin, "Machinemodule") and adopted what fit. Differences in short: it runs the whole DSP1
in the emulator (79 M instr/s) and has its own sequencer and JUCE UI; we keep our bit-exact C++ track FX/mixer and added only DSP1's master section
(user chose this, "option A"). Credit ModuleDrum in the README and `THIRD_PARTY_NOTICES` when committing (the new skin files already credit it in comments).
The auto-mode classifier refused to run their `make_logos.py`: read their sources, never execute them (the logo bitmaps were decoded by a small script
from their generated `MdLogos.h`; in the end we use the OS's own badges instead).

## Done (all uncommitted)

| Item | Where | Verified |
|---|---|---|
| Mute groups (kit bytes at 0x4a7; a trigger zeroes the target's mixer words until its own next trigger) | `engine/HostModel.{h,cpp}` `setMuteGroup`, `vst/engine.cpp` (`Kit::muteGroup`, `Inst::muteGroup`, state **v2**; v1 still loads) | `MD_MUTEGROUP=1 build-vst-x86/md-hash OS.syx`; md-hash unchanged `04ef1db4fe767721` |
| Machine logos = the OS's own five family badges (E12 EFM P-I TRX UW), cropped 17x9 | `tools/mdskin/md_art.h` `familyBadges`, `mdartdump`, `md_logos.py`; `mk_skin.py` `machine_bar`, picker headers | previews; `skin_recipe.py make` check passes (Gen2 public zip stays art-free) |
| Machine/family description lines (ModuleDrum's wording) | `tools/mdskin/md_machine_text.py` | preview |
| Mixer page (16 strips: pan, LEV fader, meter, M, S); taller, nameplate moved into its title bar | `mk_skin.py` GLOBAL tab; params `trackN_mute/solo/meter` appended; engine slots `kSlotMute/kSlotSolo`, `meter[]`, `display_rev` (vst.json now `HAS_DISPLAY_REV` + `DISPLAY_REV_NO_UPDATE`) | previews; engine compiles; **not seen working on the device** |
| Q-Link column outlines (`qlinkBoundsData`, `hideQLinkBounds` false); GLOBAL tab has 3 Q-Link sub-pages: LEV, **PAN** (user request), GLOBAL | `mk_skin.py` `QL_*` | drawn over previews; skin_check clean on GLOBAL/FX |
| M/S double-press fix: `mdMixBtnTouch` = Mouse Down `Q-Link` + Enter Pressed `Toggle Switch` (as the ROM/randomise toggles; toggling on both fired twice) | `mk_skin.py` | **not yet on the device** (it was offline) |
| **Master FX** (reverb, delay, EQ, dynamix; ON by default, Gen2 only): DSP1 `$342-$9A8` on its own DSP + the OS's own knob conversion on the 68k | `engine/MasterFx.{h,cpp}`, `MachineRunner::runTo`, `EngineT::setMasterFx` (`engine/MdEngine.h`), `vst/engine.cpp` (`kSlotMasterOn`, `kSlotMasterFx`, `kMasterFxSupported`), params `master_fx`, `rv1-8 dl1-8 eq1-8 dx1-8` | **`build-vst-x86/md-mastertest OS.syx 400`: 0 mismatches** vs the full DSP1; ~10,300 DSP instr/block |
| FX tab (after GLOBAL; tracks moved to fnKeyIndex/Tab +1) and an MST FX toggle in GLOBAL's spare cell | `mk_skin.py` | previews; recipe check passes (321 images) |
| A/B build "Machinemodule B" (uid `MdOb`, `machinedrum_b.so`) | `vst/vst_b.json`, `MD_VARIANT=b` in `vst/build_so.sh`, `mk_skin.py name=Machinemodule_B` | installed on the Live 3 (below) |

Level note: with master FX on the output is the hardware's own main level, ~x5.76 (+15 dB) over the dry mix the plugin gave before (and still gives
with `master_fx` off). A hot kit clips at 16 bits. Not trimmed (the DSP saturates internally; a trim after it only lowers the ceiling). A trim/gain
param is an open option.

## Live 3 (192.168.2.217, aarch64, busybox, MPC OS 3.x)

- (History) `Machinemodule-B-0.4.4` installed with `sh install.sh -y -t /nvme/Synths` (default `/sdcard/Synths` is read-only there); installer restarted MPC. Settings
  backup `/data/Settings/MPC/MPC.settings.bak-machinedrum_b-20261029-122355`. Data folder is a **symlink** made by hand:
  `ln -sfn "../sd88me - VST - Machinemodule/machinedrum" "/nvme/Synths/sd88me - VST - Machinemodule B/machinedrum"`. `machinedrum_b.so` md5 `2777a0f6acebaabaf6d2218255811cad`.
- The user listened: "looks good so far". Then asked for pan on Q-Links and the M/S fix. Those are skin-only and are **built but not deployed**: the Live 3 was
  unreachable (no ping, no ssh). Deploy when it is back, then the user restarts MPC (skins load at startup; ask first, never restart MPC unasked):
  `cd "vst/build/skin-b/sd88me - VST - Machinemodule B" && tar -czf - "Plugin Skins" | ssh root@192.168.2.217 'cd "/nvme/Synths/sd88me - VST - Machinemodule B" && tar -xzf -'`
  (rebuild the skin first if `mk_skin.py` changed: command under "Commands").
- Remove: `sh /tmp/Machinemodule-B-0.4.4/uninstall.sh` on the device (the unzipped installer is still in /tmp until reboot).
- Taps attach only to the normal plugin (they `dlopen` `machinedrum_one.so`).

## Verified on the Live 3 (2026-10-30, user)

Skin redeployed and MPC restarted: M/S buttons toggle once per tap, the PAN Q-Link page works, meters move. (Items 1 below is therefore done except the solo logic.)

## Bench (2026-10-30)

`MD_MASTER=0|1 MD_TIME=1 md-hash OS.syx` (new) times each render() against the 725 us/32-frame budget. Live 3 (aarch64 JIT, single engine thread, 16-track fixed pattern, MPC idle; run with `env -i PATH=/usr/bin:/bin sh -c "ulimit -s 8192; ..."`): master off mean 126-129 us / p99 305; master on mean 173-174 / p99 364-394 (about +45 us, ~6%). Worst 15-16 ms in all runs with or without master (not caused by it; cause not found). x86 dev box: 274 -> 414 us. Still to measure: the plugin itself with 16 playing tracks (`/tmp/md-stats-on`).

## Open / to verify on the device

1. M/S buttons toggle once per tap; pan Q-Link page works (Q-Link button steps LEV -> PAN -> GLOBAL; outlines follow); meters move; solo logic (any solo mutes the rest).
2. Master FX: clicks on knob changes (the OS's slew is not applied, the converted words jump); CPU on the Live 3 with all 16 tracks (`touch /tmp/md-stats-on`; `Engine::masterUs` is timed but not yet printed in the stats line in `vst/engine.cpp`).
3. Taps with master on still carry the dry sends (they read the mixer's rev/del planes); decide whether that is right.
4. Compare ModuleDrum's idle-voice skip (`MdMixEngine.cpp`: idle strips left out, checked) with our silence release (`HostModel::setSilenceRelease`); not yet looked at beyond the review.
5. Before committing: your uncommitted **0.4.2 Gen2 work** (`mdextract`, `romsamples.h`, `build_mdextract.sh`, `release/build_release.sh`, `vst/build_so.sh` fix) is in the same tree; split the commits. Also README/THIRD_PARTY credit, bump version, rebuild the release zips (`release/build_release.sh <OS.syx> -P` for public; the `dist/*0.4.2*` zips are stale).
6. The Gen2 public (-P) zip: the recipe check passes, but run `release/build_release.sh -P` end to end once (it also needs the new `skin_recipe` outputs and the FX tab in `vst/skin_recipe.h`'s file list if it enumerates files).

## New requests from the user (not started)

- **"Make the improvements to the VST3 version."** Unclear which build this is: this repo has **no VST3 target** (`desktop/` holds only a build folder; the Monomodule family
  in the README ships AU/VST3/standalone, ModuleDrum is that). Ask the user whether they mean a desktop VST3 version of Machinemodule (to be made, ported from this MPC plugin or
  built on ModuleDrum), or something else. Candidates to carry over: mute groups, master FX (the desktop has the CPU for the full DSP1, as ModuleDrum does), mixer page with M/S/meters, logos/descriptions.
- **"Skin improvements to the Gen1 build, your own version."** Read as: give the Gen1 (armhf: Force, MPC Live II/X/One) build the skin improvements (mixer page, logos, descriptions,
  Q-Link outlines, M/S fix, PAN Q-Link page) in a version of its own, without the master FX (`kMasterFxSupported` is false on 32-bit ARM, so `master_fx` does nothing there; the sends leave through the taps).
  Suggested: a `gen=1` argument to `mk_skin.py` that leaves out the FX tab and the MST FX toggle (the params stay: `params.json` is append-only and shared), keep fnKeyIndex numbering consistent, and check
  the 10 Hz `display_rev` poll and the meter computation are cheap enough on the Force (measure with the stats line). Confirm "your own version" with the user (a separate skin/folder? a different name like the A/B?).

## Commands

```
# offline tests (x86; OS file at /tmp/os.syx, i.e. Elektron_SPS1-1UW_OS1.63.syx)
cd build-vst-x86 && cmake . && ninja md-hash md-mastertest && cd ..
./build-vst-x86/md-hash /tmp/os.syx                 # must print hash 04ef1db4fe767721
MD_MUTEGROUP=1 ./build-vst-x86/md-hash /tmp/os.syx  # mute group check
./build-vst-x86/md-mastertest /tmp/os.syx 400       # master FX vs the full DSP1: 0 mismatches
# plugin on x86 + smoke (MD_SMOKE_MASTER=0|1; data dir needs the OS .syx)
mkdir -p build-vst-x86-plugin && cd build-vst-x86-plugin && cmake -G Ninja -DMPC_VST_DIR=$HOME/mpc-vst-plugins -DCMAKE_BUILD_TYPE=Release .. && ninja machinedrum_one md-vst-smoke
# skins (also regenerates vst/params.json first: cd vst && python3 -I gen_params.py)
tools/mdskin/build_skin.sh /tmp/os.syx ~/mpc-vst-plugins
# the A/B variant: .so, skin, zip
MD_VARIANT=b vst/build_so.sh - ~/mpc-vst-plugins aarch64
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -e MPC_VST_TOOLS=/mv/tools -v "$PWD":/r -v ~/mpc-vst-plugins:/mv:ro -w /r python:3.11-slim sh -c "pip install -q --target /tmp/p pillow >/dev/null 2>&1; PYTHONPATH=/tmp/p python3 tools/mdskin/mk_skin.py vst/build/art.json vst/build/machines.txt vst/params.json vst/build/skin-b name=Machinemodule_B"
python3 ~/mpc-vst-plugins/tools/gen_vst.py vst/vst_b.json && python3 ~/mpc-vst-plugins/tools/release.py --so vst/build/aarch64/machinedrum_b.so --skin "vst/build/skin-b/sd88me - VST - Machinemodule B" --entry vst/build/pluginlist-entry.xml --version <x.y.z> --id machinemodule-b --repo sd88me/mpc-vst-machinedrum --license AGPL-3.0-only -o dist
# Gen2 recipe check (after any skin change): tools/mdskin/skin_recipe.py make <art.json> <machines.txt> <out>  (see release/build_release.sh step 5)
```

Rules that bit us: params are append-only (`vst/params.lock`); `gen_vst.py vst_b.json` overwrites `vst/build/params.h`/`pluginlist-entry.xml`, so regenerate before building the other plugin (`build_so.sh` does); the skin generator's pixels may depend on at most one art bit
each (no layout that depends on the art, e.g. trimming a badge by its lit columns); the skin preview draws toggles OFF always (MPC follows the param).


## Update 2026-10-30 (later): decisions and what was done

User decisions: **Gen1** = just update the existing plugin (no separate variant); **desktop** = our own desktop version, which already exists.

- **Gen1 skin:** `mk_skin.py gen=1` leaves out the FX tab and the MST FX switch (no master FX on 32-bit ARM); track tabs renumber (fnKeyIndex/Q-Link tab 1 less). `release/build_release.sh` passes `gen=1` when `-a armhf`.
  Verified by building both skins (gen=0: 37 tabs, gen=1: 35, previews). The Gen1 .so is the same engine.cpp (mute groups, mute/solo, meters work there; master FX is compiled out by `kMasterFxSupported`); **not built for armhf or tried on a Force yet** (needs the recompiled-DSP build, see build_release.sh steps 1-4).
- **Desktop version = the `desktop` branch** (JUCE VST3/AU/Standalone, `desktop/`, LCD editor after Monomodule; NOT in main/md-improvements). Worktree: `/home/sam/mpc-vst-machinedrum-desktop`, branch `desktop-improvements` (off `desktop`), uncommitted; submodules initialised from the local .git/modules.
  It already has: master effects (own, more faithful implementation than ours: runs the OS's time-sliced conversion routines + OS smoother in `HostModel`, `engine/MasterFx.*` with a different API, so the two `MasterFx` files conflict if branches are merged), mixer view with meters, per-track M (mute) and L (lock), the OS's family badges, CTR/INP machines, flash import.
  Missing there, and done so far: **mute groups** (HostModel `setMuteGroup`; `KitCodec.h` parses the groups at $4a7 [LFO block is 659 bytes]; `TrackValues::muteGroup` 0 = none, 1-16 = target; `KitValues::load`, `EngineFeeder::apply`, an APVTS int param `tNN_mutegroup` added last in each track group). md-hash on that engine unchanged (04ef1db4fe767721); the kit parser gives track 9 -> 10 (closed hat chokes open hat) in the factory kits. **Verified:** Docker image `md-desktop-builder` (ubuntu 24.04 + the JUCE dev packages; Dockerfile only in the session scratchpad: build-essential cmake ninja-build git python3 xvfb ca-certificates curl libasound2-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxcomposite-dev libxext-dev libxrender-dev libfreetype-dev libfontconfig1-dev libgl1-mesa-dev), `cmake -S desktop -B /b -G Ninja && ninja -C /b MmRender Machinemodule_VST3` builds with no errors (~15 min incl. JUCE download), run with `--shm-size=4g`. `MmRender OS.syx` = **ALL OK**, including a new test (h) in `desktop/tests/mm_render.cpp`: track 1's mute group silences track 2's bus after its trigger (0.000000 with the group, 0.033 without). Note: a wrong param key makes the test helper `setParam` segfault on a null parameter (SYN keys are `syn1`-`syn8`).
  Not done on desktop: solo and a PAN strip in the mixer view, machine/family description lines, idle-voice check. Their master FX has the OS smoother, so the knob-click question from the MPC build does not apply there; **worth porting that conversion path to the MPC engine** (engine/HostModel + MachineRunner::runRange) instead of our direct convertMasterFx, if clicks are audible.


## Update 2026-10-30 (night): Gen2 deploy, desktop done, rebase

- **Desktop** (`desktop-improvements`): mixer page now has PAN strip, LEV + meter, M and S keys per track (`MixerPage` in `desktop/src/ui/Widgets.*`); solo is an APVTS bool `tNN_solo`
  applied as an effective mute in `EngineFeeder` (not a kit value); machine/family description in the editor footer (`desktop/src/ui/MachineText.h`, centre of the footer when
  the status line is hidden). `MmRender` = ALL OK with new tests (h) mute groups and (i) solo; snapshots with `MmSnapshot` under xvfb (Docker image `md-desktop-builder`).
- **Gen2 on the Live 3:** `Machinemodule-1.0.0` (per-user build: OS file + flash image, factory kits and ROM samples) installed with one stop/start (`systemctl stop acvs`, `install.sh -y -n -t /nvme/Synths` for the
  three zips, `systemctl start acvs`). **That build came from the old base** (before the rebase onto v0.5.1); rebuild after the v1 commits for the real release. The upgrade
  removes the user's `elektron_sps1-1uw_os1.63.bin` from the data folder (the zip replaces the whole `machinedrum/` folder): copy it back (a copy is kept in the session scratchpad only).
  Machinemodule B is still installed beside it (its data is a symlink to the normal folder), remove it when done A/B-ing.
- **Not done / ideas:** idle-voice skip vs silence release; the OS-smoother master-FX conversion from the desktop engine for the MPC build (only if clicks are audible); Gen1 (armhf) build of the plugin and a Force test;
  `params.lock` files are untracked (generated by `gen_vst.py`; the order rule is by convention, params.json is append-only).

- **2026-10-30 (late): device state.** Live 3: **B = v1.0.0 from the committed `v1` branch** (`machinedrum_b.so` md5 bac69b07..., `Machinemodule-B-1.0.0` via stop acvs / `install.sh -y -n -t /nvme/Synths` / start acvs; the installer removes B's `machinedrum` symlink, recreate it: `ln -sfn "../sd88me - VST - Machinemodule/machinedrum" machinedrum`).
  **A = the early 1.0.0 test build from the OLD base** (md5 92d22801..., installed before the rebase), NOT the original 0.4.2/0.5.x: so A and B differ only in the rebase (upstream's lists, VOICES 16...). For a real A/B baseline, build upstream `origin/main` (v0.5.1) as A. The full v1.0.0 release zips (`dist/Machinemodule-1.0.0-mpc-aarch64.zip`, tapfx; plugin md5 d80cfe67...) are built from `v1` and not installed.

- **2026-10-30 (final): no A/B.** The Live 3 now has exactly two plugins, both from the committed `v1` branch: **Machinemodule 1.0.0** (`machinedrum_one.so` md5 d80cfe67...) and **Machinemodule Tap** (the effect, uid MdTf, `machinedrum_tapfx.so` md5 a964611b...). B, the old Tap instrument and the early Tap FX were uninstalled with their uninstall.sh; the flash image was put back in `machinedrum/`. One stop/start: `systemctl stop acvs`, the uninstalls and installs with `-y -n -t /nvme/Synths`, `systemctl start acvs`. (The A/B notes above are history.) `MD_VARIANT=b` and `vst_b.json` remain in the repo for future A/B builds.
