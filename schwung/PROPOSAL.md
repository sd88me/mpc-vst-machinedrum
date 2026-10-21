# Machinemodule for Schwung: proposal

Status: exploration (2026-10-21). Nothing here is built yet, except the measurement tool `tools/move_bench.sh`. The folder
lives in this repo until the module is mature, then moves to its own repo (`schwung-machinemodule`).

The goal is the Machinedrum SPS-1 UW engine as a Schwung **sound generator** on Ableton Move:

- the 16 MD tracks on Move's 16 drum pads;
- an editor that follows the last pad you hit;
- DR32's interface philosophy: Schwung's own knob grid, as few menus as possible, one set of pages that always shows
  the focused pad;
- a `dr32_engine.so`, so that MD machines can also be played on DR32 pads.

## 1. Can the Move run it? Measured on the user's Move

The device: a CM4, with 4 Cortex-A72 cores at 1.5 GHz, 1.85 GB RAM, glibc 2.35 and Schwung 1.7.3. The engine is the
desktop branch's `mdcore`, cross-built for aarch64 with `-mcpu=cortex-a72` and linked statically. The test ran on core 2
while Move itself was running, using `tools/move_bench.sh`.

| test | result |
|---|---|
| `md-hash`, dsp56300 **JIT** | hash `e2b514e70c173c33`: **bit-exact** with desktop/Force |
| `md-hash`, static recompiler + interpreter (the Gen2 MPC configuration) | bit-exact, but **2.4x slower** than the JIT, so we use the JIT on Move |
| 12 s of the dense 16-track pattern, JIT, one core, no silence release | 12.6 s wall time, including boot (about 1.3 s) |
| the same with silence release (what the module will run) | **7.8 s for 12 s of audio = about 55% of one core**; mean 6.4 and at most 10 voices rendering |
| the same split over 2 / 3 voice threads | 8.7 s / 10.7 s: **slower**, because the per-32-frame sync costs more than it saves. One engine thread is enough |
| `md-cost`: idle engine | **229 us per 128-frame block** (8% of the 2902 us budget): OS tick 128, voice DSP 59, track FX 11, mixer 5 |
| `md-cost`: one more playing voice | synth machines on average **+100 us** (TRX-BD class); ROM machines +284 us |
| peak RSS of one engine process | 630 MB (mostly the JIT's mapped tables; see risk R2) |

How this compares with the Force (armhf, recompiled), where a voice cost 600-900 us and 4-6 voices was the limit: on the
Move the JIT makes a voice **6-9x cheaper**. A whole kit with 16 voices sounding at once is about 229 + 16 x 100 = 1.8 ms
per block, or 63% of one core, on a core that Move's audio callback does not use. A typical drum pattern, where most voices
are released after their tail, sits around 30-55% of one core.

The bench boosts the core's clock, so it is optimistic. Monomodule's port measured real in-Move costs 1.5-2x higher than its
bench, because of the `ondemand` governor and work arriving in bursts. So the design must leave headroom: run on cores 0-2,
render a block or two ahead, release silent voices, and keep a voice budget as a safety net. All of these exist already from
the Force work.

**Verdict: feasible, with the engine in a helper process, not in the audio callback.**

## 2. Architecture

The precedent is legsmechanical's Schwung port of Monomodule (`schwung-monomodule`, installed on this Move as
`monomodule-one`). It hosts the same kind of DSP56300 + host-CPU emulation the same way, and its notes (docs/PLAN.md,
docs/PERF.md there) cover most of the traps.

```
 MoveOriginal (SPI callback, core 3, FIFO 70)                md-engine (spawned helper, cores 0-2, SCHED_OTHER / FIFO 10)
 ┌──────────────────────────────────────────┐   memfd shm   ┌────────────────────────────────────────────────┐
 │ dsp.so  (plugin_api_v2)                  │ ───────────▶  │ command FIFO → HostModel (params, triggers,    │
 │  on_midi: note 36..51 → trigger cmd      │  commands     │   machines, kit, CTR rules, tempo)              │
 │  set/get_param: authoritative param table│               │ Engine::render (JIT DSP + 68k + track FX +      │
 │  render_block: copy block k−D from ring  │ ◀───────────  │   mixer [+ MasterFx]) 128 frames per wake       │
 │  render_split: per-track buffers         │  audio ring   │ ring slots: main L/R, 16 track monos, sends     │
 │  watchdog: respawn + replay param table  │  (D+1 slots)  │ stats: load, peak, voices, underruns            │
 └──────────────────────────────────────────┘               └────────────────────────────────────────────────┘
```

- **`dsp.so` does no emulation.** Each block it writes commands, reads the output slot from D blocks ago, bumps the block
  counter and wakes the helper with a futex. D is the latency in blocks: 1 = 2.9 ms by default, raised with LTNCY
  (as Monomodule does) if a kit needs more headroom. If the helper is late, the block is silent and the underrun is counted. The audio
  thread never waits.
- **`md-engine` is a separate executable** started with `posix_spawn`, not `fork()`, so MoveOriginal's page tables are not
  copied. It demotes itself before anything else (the Schwung rule: threads inherit FIFO 70), pins to cores 0-2, and
  declares `capabilities.forks_processes`. Running out of process also keeps a JIT crash out of Move, and keeps the JIT's
  mmap traffic off MoveOriginal's `mmap_lock`.
- **The engine is unchanged.** It is `engine/` as it is on the desktop branch, so the INP, CTR, master FX and track FX work
  carries over. The desktop plugin's non-JUCE logic (the parameter model, CTR rules, kit parsing, machine defaults) should
  move from `desktop/src/plugin/MdProcessor.cpp` into a shared `core/` library that the JUCE plugin and the helper both
  link. That is milestone M1, and the desktop tests (`MmRender`, `md-hash`) must stay identical.
- **The Force work is reused as is:** silence release (`setSilenceRelease`), the voice budget, the duty cap, the
  `ParallelVoiceEngine` (kept at 1 group; the measurement above says more groups do not pay on the A72), and the tap
  design, which becomes Schwung's per-voice buses (below).
- **Per-track buses.** Answer `get_param("split_voices")` with the 16 tracks, named after their machines ("T01 TRX BD"),
  and export `move_plugin_render_split`. Schwung can then put any group of MD tracks on its own bus with its own insert FX,
  as DR32 does. This replaces the MPC "Tap" plugins. The helper already produces each track's mono signal (`Mixer::solo`).
- **Master FX.** The MD's own rhythm echo, gate reverb, EQ and dynamix (`MasterFx`, about 13 M DSP instructions per
  second) go on their own kit-wide pages. Turning them off saves their cost; Schwung's FX slots behind the module still
  work either way.
- **Tempo.** Move's tempo comes from `move_info.h`, and the helper's `HostModel` is set from it, for LFO sync and the echo's
  time.
- **Audio input (INP machines).** These are left out of v1. A sound generator's chain slot has no audio input, which is why
  Schwung has a separate "Line In" generator. To revisit later.

### Firmware

This follows the desktop rules. Nothing derived from firmware ships in the module.

- The module.json `assets` block asks for **the MD OS 1.63 `.syx`**, which is a free download from Elektron. The same is
  done by `monomodule-one` for the Monomachine OS.
- **Factory kits and ROM samples:** the user either uploads the `FACTORY.syx` and `ROM_SAMPLES.bin` that the desktop
  plugin's "Import flash image..." already makes, or uploads the flash `.bin` and the module runs the same `md-import`
  (built for aarch64) once, on a worker. The first option needs no new code. The second avoids needing a computer. Both
  can be done.

## 3. The UI: DR32's way

DR32's rules, and what follows from them here:

1. **Use Schwung's own knob grid and no custom interface.** Every page is the host's 8-knob grid. Values, faders, enum
   pickers, Copy/Delete and the header all draw the way they do in every other module.
2. **The editor always follows the pad you hit.** There is no "follow" toggle and no track menu. The pages are one
   template child level with 16 instances. `child_index_param: "ui_track"` owns the focus. `child_press_param:
   "live_press"` tells a finger from the sequencer, so a running pattern never drags the editor around. The focus answer
   carries a change token (`"17:5"`), so hitting the pad you are already on brings you back to it from a kit-wide page.
3. **Use as few pages as possible, and the same pages for every track.** Jog moves through them in the MD's own order:
   Track, Synth, Amp/Filter, Mix, LFO. After those come the kit-wide pages: Echo, Reverb, EQ, Dynamix, Master.
4. **The TRK knob sits at k1 on the Track page.** That suppresses Schwung's "Selected Pad" picker page, as DR32's PAD
   knob does. It shows the track number in large type, and turning it is the second way to pick a track.

Performance surface: `pad_layout: "drums"`, `child_note_base: 36` (tracks 1-16 = notes 36-51 = Move's 16 drum pads,
bottom left first). `child_names` is served live as "BD", "SD"... from each track's machine, so a sequencer such as movy
labels its pads the way the MD does.

### The pages

Each row is one page of 8 knobs. The keys are the desktop plugin's `tNN_*` keys: the template is `t{index}_{key}`, with
`child_index_base: 1` and 2 digits.

| page | k1 | k2 | k3 | k4 | k5 | k6 | k7 | k8 |
|---|---|---|---|---|---|---|---|---|
| **Kit** (items) | Init · Factory · User → kit list; scrolling auditions, as DR32's kit browser does | | | | | | | |
| **Track** | TRK | MACH | LEVEL | MUTE | CHOKE* | | | |
| **Synth** | SYN1 | SYN2 | SYN3 | SYN4 | SYN5 | SYN6 | SYN7 | SYN8 |
| **Amp/Filter** | AMD | AMF | EQF | EQG | FLTF | FLTW | FLTQ | SRR |
| **Mix** | DIST | VOL | PAN | DEL | REV | LFOS | LFOD | LFOM |
| **LFO** | TRACK | PARAM | SHP1 | SHP2 | TYPE | | | |
| **Echo** | the 8 echo parameters (the MD's own names) | | | | | | | |
| **Reverb** | the 8 gate reverb parameters | | | | | | | |
| **EQ** | the 8 EQ parameters | | | | | | | |
| **Dynamix** | the 8 dynamix parameters | | | | | | | |
| **Master** | VOL | FX ON | LTNCY | VEL MODE | ACC THR | ACC AMT | LOAD | PEAK |

\* Not an MD parameter: an optional choke group (open/closed hat), something a Move pad rack adds. VEL MODE / ACC THR /
ACC AMT are described in section 7.

- **The SYN knobs carry the machine's own labels.** For example, TRX-BD shows PTCH DEC RAMP RDEC STRT NOIS HARM CLIP, and
  EFM-SD shows PTCH DEC NOIS NDEC MOD MFRQ MDEC HPF. This works the way DR32 handles engine pages. There is one Synth
  level per machine, and each declares the same `syn1..syn8` keys with page-level `short_name` overrides, gated on
  `visible_if ui_machine == <id>` (`ui_machine` is the focused track's machine: read-only, and on no level). As with DR32
  (whose module.json limit is 64 KB), the DSP serves only the levels for machines that the loaded kit uses (at most 16),
  and the host re-reads them on the `is_loading` edge after a machine or kit change. The labels come from the OS's own
  machine table (`tools/mdmachine`), read from the user's OS at boot, so no firmware text is shipped.
- **The machine picker (MACH)** works like DR32's ENGN picker. It is the host's list, with one section per MD group:
  GND, TRX, EFM, E12, P-I, CTR and ROM (ROM only when samples were imported). The rows are machine names. Scrolling puts
  each machine on the track as you land on it, so you hear it on the next hit, and clicking closes the picker. MID, RAM and
  INP are left out, as explained above.
- **CTR machines.** CTR-AL and CTR-8P behave as they do on the desktop. CTR-8P's target pages (TRK1..PRM8) appear in place
  of Synth when that machine is on the focused track. The CTR-RE/GB/EQ/DX machines move the master FX pages.
- **Copy, Delete and Undo of a track** use the host's gesture (`claims_edit_ccs`, `child_copy_keys` = machine first, then
  every `tNN_*` key). Hold Copy, hit the kick, hit another pad, and the second pad is now the kick. There is no menu for it.
- **Kits.** The kit browser lists Factory (from the user's `FACTORY.syx`), User (kits saved in the set, plus `.syx` kit
  dumps the user uploads) and Init. The module's `state` (what a Schwung set saves) is the full param table, so a set
  remembers the kit and every change.

What the Move screen shows (128x64, the host's grid; the header is the host's, with the focused track's name):

```
┌────────────────────────────────┐   ┌────────────────────────────────┐
│ T01 TRX BD           Synth  2/5│   │ T03 EFM SD      Amp/Filter 3/5 │
│ PTCH DEC  RAMP RDEC            │   │ AMD  AMF  EQF  EQG             │
│  64   48   0    32             │   │  0    0    64   64             │
│ STRT NOIS HARM CLIP            │   │ FLTF FLTW FLTQ SRR             │
│  0    0    12   0              │   │  0   127   0    0              │
└────────────────────────────────┘   └────────────────────────────────┘
  hit pad 3 → the same page, now T03; hit pad 1 again → back to T01
```

## 4. DR32 compatibility (`dr32_engine.so`)

DR32's contract (`dsp/dr32_engine_api.h`, MIT) says that one engine instance is **one mono drum voice per pad**, and that
`render` runs inside DR32's own render, on the audio callback. A whole MD per pad cannot fit that: each would be a 229 us
engine plus 630 MB, times 32 pads. So the plugin uses a **shared engine**:

- `dr32_engine.so` sits in the Machinemodule folder. DR32 finds it by name in sibling module folders and needs no
  registration. It talks to **its own** `md-engine` helper, started on first `create`, through the same shared-memory
  protocol as the module. It does not share the module's helper, so DR32 works whether or not a Machinemodule instance
  is loaded.
- **Each pad instance leases a track (1 of 16)** of that engine when it is created, and returns it on `destroy`. A 17th MD
  pad starts a second engine. `note_on` queues a trigger with the pad's velocity and `tune_st`, which is mapped onto the
  machine's pitch parameter. `render` copies that track's mono output (`Mixer::solo`, before VOL/PAN/sends, because DR32
  does those) from the ring, at the pad's own read position.
- **What DR32 does around it:** Volume, Pan, sends, choke fades, buses, Stereo and state. The engine's per-track VOL, PAN,
  DEL and REV are pinned. Master FX are off in this engine.
- **Engines and models:** the limits are 64 engines and 256 models **across every installed plugin**, 8 knobs per page and
  no conditional knobs. One engine per MD machine would take most of that and still not fit, since 80+ machines are on
  offer. One engine per family with generic SYN1-8 labels loses the MD's labels. The proposal is a **curated set of about
  24 engines**, one per useful drum machine: TRX BD/SD/XT/CP/RS/CB/CH/OH/CY/MA/CL, EFM BD/SD/XT/CP/RS/CB/HH/CY, E12 BD/SD/HH,
  P-I BD/SD. Each has 3 pages: **Synth** (its own 8 labels), **Shape** (AMD AMF EQF EQG FLTF FLTW FLTQ SRR) and **Drive**
  (DIST, LFO). The models are each machine's default sound, plus the factory-kit sounds read from the user's `FACTORY.syx`
  in the entry point, which may read files, named "<kit> <track>". That stays under about 120 models.
- **What has to be accepted, or asked of DR32:**
  1. **Latency:** MD pads sound D blocks (2.9-5.8 ms) after DR32's own pads. DR32 has no delay compensation.
  2. **Resample:** DR32 renders a pad offline, on a worker, faster than real time. A real-time shared engine cannot follow
     that. In v1 the instance detects it (its `create` comes from a non-audio thread while a live instance holds the same
     slot model) and renders the pad on a private engine, which costs a 1.3 s boot. Otherwise it is documented as
     unsupported. A small request to DR32's author (legsmechanical) would make this clean: an `offline` flag in
     `dr32x_host` or in `create`.
  3. **Rules:** the contract asks for "no globals that a voice writes to". The shared engine is exactly that, kept behind
     a lock-free SPSC command queue per pad. It needs to be said openly in the plugin's notes, and preferably agreed with
     DR32's author first.
- `tools/plugin_check.c` from DR32 runs over the plugin on the desktop. It will report the latency as a slow attack, so
  that warning is expected.

## 5. Risks

| | risk | what we do |
|---|---|---|
| R1 | Real in-Move cost is 1.5-2x the bench (governor, bursts) | LOAD/PEAK readouts on the Master page, as Monomodule does; D = 1 by default, LTNCY to raise it; voice budget; silence release |
| R2 | Memory: one engine peaks at 630 MB RSS. Monomodule hit a commit charge of about 850 MB per engine (no swap on the Move, so `bad_alloc`) and fixed it in dsp56300 (PROT_NONE + MAP_NORESERVE reservations, small ESSI rings) | measure PSS / Committed_AS in M2, and port their patch 0002 to our fork (`sd88me/dsp56300`). A DR32 second engine doubles this, so its use must be measured too |
| R3 | JIT first-compile spikes on a machine switch (Monomodule: 2-30 ms on a CM5) | pre-warm every offered machine at helper boot, as Monomodule's `prewarm()` does, then reset the DSP while keeping its code |
| R4 | With silence release, 1 / 2 / 3 voice groups give different hashes (they are equal without release) | check before M2; it may be the order of release between groups |
| R5 | glibc 2.35 on Move; our aarch64 image (bookworm) is 2.36 | the helper is linked statically, or built on ubuntu:22.04; `dsp.so` is plain C++ with `-static-libstdc++` |

## 6. Milestones

| | what | gate |
|---|---|---|
| M0 | **done:** engine bench on the Move | bit-exact JIT, cost table above |
| M1 | `core/` shared library: the parameter model, CTR rules and kits taken out of `MdProcessor` | desktop `MmRender`, `md-hash`, pluginval unchanged |
| M2 | `md-engine` helper + `dsp.so` + a bare page set | 10 min with a busy kit and 0 underruns at D = 1 (or the D it needs, recorded); helper killed → audio back with state; PSS measured |
| M3 | the full DR32-style page set: Kit browser, per-machine Synth labels, MACH picker, master FX pages, Copy/Delete, state | the user plays it; `pages_check` / `validate.mjs` clean |
| M4 | `split_voices`: per-track buses | a track on its own bus with an insert FX |
| ~~M5~~ | parked: `dr32_engine.so` (curated engines) | `dr32-plugin-check` clean; listed in DR32's `plugins.log`; MD pads in a DR32 kit |
| M6 | packaging (`release.json`, tarball, catalog entry), then its own repo | installed from the Schwung Manager |

## 7. Decisions (the user, 2026-10-21)

1. **Latency: D = 1 (2.9 ms) by default.** LTNCY on the Master page (1-4 blocks) raises it if a busy kit underruns.
2. **Velocity: a global (kit-wide) VEL MODE on the Master page.**
   - **MD** (the hardware's way): a hit at or above **ACC THR** (default 100) is an accented trigger
     (`HostModel::trigger(track, 127, true)`): measured in the full-system emulation (mdProbe), the front panel's accented trigger
     plays at the level of MIDI velocity 127/128 (0.220 vs 0.221), with the OS's inverted polarity. A softer hit is a plain trigger
     at velocity 127 - **ACC AMT** (default 32 -> 95), so ACC AMT is how far an unaccented hit sits below an accented one. (The
     MD sequencer's own unaccented-step velocity is still to be measured; ACC AMT can then default to it.)
   - **VEL** (like any Move instrument): the hit's velocity is the trigger velocity 1-127, which scales VOL through the OS's
     own gain law (`LEV² x VEL x VOL²`); no accent. Measured on the Move: velocity 64 = 0.50 of 127.
3. **Master FX kept**, on the kit-wide Echo / Reverb / EQ / Dynamix pages, with FX ON on the Master page.
4. **Factory kits and ROM samples are imported on the Move** from the user's flash `.bin`: the module's `assets` block
   takes the OS `.syx` and the flash image; on first boot (or when the image changes) a worker runs `md-import` (mdProbe +
   mdsamples, built for aarch64) into the module's data folder, showing "IMPORTING..." until it is done. Expect it to take
   a while on the CM4 (it boots the whole emulated MD); measure in M2.
5. **DR32 integration parked.** Section 4 stays as the record of what was found; M5 is off the plan for now.

## 8. M2 status (2026-10-21): running on the Move, as a test module

Built and installed as `sound_generators/machinemodule` (version 0.1.0-test.1): `schwung/scripts/build.sh` (Docker, ubuntu 22.04's
aarch64 toolchain: glibc 2.35 as on the Move; bookworm's 2.36 pulled in `arc4random`) and `schwung/scripts/install.sh`.

| file | what |
|---|---|
| `src/plugin/md_plugin.cpp` | `dsp.so`: the kit table (desktop `core/KitModel`), the pages (served `ui_hierarchy`, one Synth page per machine in use with the OS's labels, gated on `ui_machine`), focus follow (any note while stopped; a vouched press while playing), MIDI -> triggers (MD/VEL modes), kits, state, the supervisor (spawn, watch, respawn + replay) |
| `src/child/md_engine.cpp` | `md-engine`: the engine, fed by `EngineFeeder` from its copy of the table; pre-warms every machine at boot; renders the master effects on a second thread (`master_worker.h`) |
| `src/common/md_shm.h` | the shared segment: clock, command ring, the engine's answers (machine defaults, the OS's machine table), audio |
| `tests/md_hosttest.cpp` | loads `dsp.so` as Schwung does and drives it in real time, on the Move |
| `tests/md_busybench.cpp` | a kit's cost per Move block, offline, for each engine configuration (with an audio hash) |

`md-hosttest` on the Move: boot 2.0 s (1.4 s of it the pre-warm), pages served (12 KB), a machine pick returns its own values, the
editor follows the pad, MD accents and VEL velocity behave, kits load, the state round-trips, a killed engine is back in 2 s with the
table replayed, destroy ends the engine. The one failure is the deliberately extreme case below.

What the Move measured, per 128-frame block against the 2902 us budget (`md-busybench`, one voice thread):

| | master FX off | master FX on |
|---|---|---|
| a typical groove (kick, snare, hats, clap, tom fill) | 586 us (20%), p99 1.7 ms | 1.2-1.4 ms (41-48%), p99 3.0-3.8 ms |
| every track busy (default sounds) | 1.6 ms (56%) | 2.4 ms (82%) with the master thread, p99 4.6 ms |
| every track busy with factory kit 1's long decays and sends | 3.3 ms of voice DSP: does not fit one core | |

Found and fixed on the way (both bit-exact: the same audio hash before and after):

- **The master effects recompiled themselves every block.** `MasterFx::process` patched a jump into P memory and restored it each
  32 frames, so the JIT recompiled that code twice per block: 2.4 ms per Move block of master effects. Patched once at construction:
  1.4 ms; linked JIT blocks: 1.3 ms. The section is genuine work (~9,700 DSP instructions per 32 frames, one loop per sample, no
  padding), still about as much as a busy kit's voices.
- **The master effects now run beside the voices** (`Engine::renderDry` + `renderMaster`, one 32-frame block behind, on a second
  thread; same audio as `render`): a busy block went from 3.1 to 2.4 ms. The threads spin up to 0.5 ms before sleeping, because a
  futex wake-up costs tens of microseconds on the A72, four times a block.
- **First hits compiled code mid-play** (15-18 ms blocks, a dropout at any latency): every offered machine is played once at boot.

Open, in order:

1. **Listen on the Move** with real kits at LTNCY 1, master FX on; raise LTNCY where it crackles. A typical kit's p99 (3.0-3.8 ms) is
   above one block with the master effects on, so LTNCY 2 may have to be the default when they are on.
2. **Heavy kits:** split the voices over two threads (`ParallelVoiceEngine`, with the same spin-then-sleep hand-off as the master
   thread; its condition-variable wake-up is what made it lose offline), and pin the three threads to separate cores.
3. **The flash import on the Move** (decision 4): mdProbe + mdsamples for aarch64, run once by the supervisor.
4. Machine picker as DR32's ENGN browser (sections per group) instead of an enum; `split_voices`; Copy/Delete checked on the device.

### M2, second round (2026-10-21)

- **Master FX: Off / Full / Rev+Echo** (the FX knob on the Master page). Rev+Echo jumps over the section's EQ and dynamix (P:$67c ->
  $96f: they filter the main mix in place after the returns are added, and read only their own parameters Y:$170-$184 and state
  Y:$18d-$19f): about 25% less CPU (a groove: 1.5 -> 1.1 ms per block with the section run in line). Not the MD's sound: the dynamix
  at the factory settings adds about +5.6 dB, so Rev+Echo is quieter. The section also runs on its own thread (`MasterWorker`).
- **VOICES** (Master page, 4-16, 16 = no limit): the engine's voice budget (`HostModel::setMaxActiveVoices`; ROM machines count 2).
  A heavy voice costs ~230 us per block on the Move, so ~6-8 heavy voices with the master FX is the ceiling of one core.
- **Real-time threads:** a spawned engine cannot raise its own priority (MoveOriginal has CAP_SYS_NICE; a child does not inherit
  it), so the engine publishes its thread ids and the supervisor, inside MoveOriginal, sets them to SCHED_FIFO 10 (below Move's own
  audio, 35/70). Measured: a typical beat on factory kit 1 went from ~500 gaps in 20 s to none.
- **No locked memory:** the emulator mlocks its maps (~5 GB of address space: a few pages mapped many times). Under MoveOriginal's
  unlimited RLIMIT_MEMLOCK an engine took >4 s to exit (respawn, unload). The engine now lowers the limit and drops inherited
  capabilities first.
- **Voice threads:** `ParallelVoiceEngine::setSpinUs` (default 0: the MPC and desktop builds are unchanged). On the Move 2 voice
  threads help only a kit too heavy for one core (2.9 -> 2.2 ms of voices) and cost more on a normal one, so the module stays at one.
- `md-hosttest` now runs its callback as Move does (SCHED_FIFO 70, core 3, when run as root) and has a groove gate: factory kit 1,
  a typical beat, the defaults (LTNCY 1, FX Full): **no gaps** (twice in a row). The 16-busy-tracks case is reported, not gated:
  with VOICES 6 + LTNCY 2 it plays with 0-5 gaps.
- R4 explained: with silence release, voice groups change the noise machines' output only (each DSP instance has its own noise
  sequence; a released voice starts afresh), as the MPC notes found for release alone.

