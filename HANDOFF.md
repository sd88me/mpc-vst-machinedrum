# Handoff

Read this first in any new session. Keep it current at every checkpoint.

## The plan

Follow Monomodule's design (see `mpc-vst-monomodule`): run only the MD's DSP code in dsp56300, with a harness
stub in place of the hardware DMA/ESSI main loop, and a C++ host model in place of the ColdFire. Split into two
plugins: the voice machines (One) and the master effects (FX).

The difference from Monomodule: we don't have to reverse-engineer the host protocol from disassembly alone.
gearmulator-md-mm already boots the whole MD, so we can log the real ColdFire-to-DSP traffic.

Steps:

1. **Trace (x86).** Build gearmulator-md-mm (console or test target, no plugin needed) with a full flash
   image. Log which OS program is booted into which DSP, then every HI08 word and host command to each DSP
   while triggering one track with known machines and parameters. Output: the MD's parameter protocol (its
   equivalent of Monomodule's 52-word block) and the voice-DSP/FX-DSP roles. **This step decides feasibility.**
   **Done** (see Status and `docs/PROTOCOL.md`).
   That next pass should also settle the per-voice dispatch pattern and whether per-voice audio is
   separable before DSP2's internal mix — see "Design goal: all voices in one plugin instance" in
   `docs/PROTOCOL.md`. Goal: **Machinedrum One plays all voices at once from one instance** (a MIDI
   note-number drum map), the way Monomodule's Six plays all 6 Monomachine tracks — not one voice per
   plugin instance. Real hardware already renders every voice inside one audio block on the single
   voice-producer DSP, so this should fall out of the per-voice dispatch pattern rather than need
   redesigning later.
2. **Engine.** `md::DspEngine`: load the voice program from the `.syx` (`tools/mdfw`), stub the main loop, and
   drive all voices per audio block (not just one — see step 1's goal). Check sample-for-sample against
   gearmulator-md-mm.
3. **Measure.** Instruction rate for all voices together, on x86 and on the Force. The MD may need
   voice subsets or the static recompiler (`libs/dsp56300`, `arm32` branch) to fit a core.
4. **FX engine** from the mixer program, the same way.
5. **Port**: VST wrapper, skin and `vst.json`, following `mpc-vst-monomodule` and `mpc-vst-plugins`.

## Status

- **2026-09-27: OS file decoded.** `tools/mdfw` unpacks the MD OS 1.63 `.syx` into five sections, two of them
  complete DSP programs. Details and open questions in `docs/FIRMWARE.md`.

- **2026-09-27: step 1 (trace) done for the DSP-role question; runtime protocol still open.** Built
  gearmulator-md-mm's mdLib/test targets on x86 (plugin build off, `BUILD_TESTING=ON`; needs the
  `dsp56300`, `mc68k`, `cpp-terminal`, `asmjit`, `freetype`, `RmlUi` submodules initialized — see
  `tools/mdtrace/README.md`). It boots and passes `mdAudioFirmwareTest` against the user's full MD
  1.63 flash image.

  Added `tools/mdtrace`: a local patch to `mddsp.cpp` (not committed to the submodule, not
  upstreamed) that logs every UC↔DSP HI08 word, plus a driver tool (`mdTraceTool`). Capturing 5
  emulated seconds of boot and comparing the real transfer to `tools/mdfw`'s decoded sections
  **confirmed which OS-file section runs on which DSP**: section 1 (large) → DSP2 (voice producer);
  section 2 (small) → DSP1 (mixer/codec/master FX). This also confirms `tools/mdfw` decodes
  byte-exactly, and settles the split: **Machinedrum One** (voices) comes from section 1 on DSP2,
  **Machinedrum FX** from section 2 on DSP1, exactly the two-plugin split the user wants (master FX
  dropped from One, shipped as its own plugin). Also found: the boot has two stages, a tiny
  first-stage loader over the standard `dsp56300` boot protocol, then the real program streamed as
  ordinary runtime host-port words read by DSP-side loader code. Full details in
  `docs/PROTOCOL.md`.

  **Not yet done: the runtime (post-boot) parameter protocol** — the MD equivalent of Monomodule's
  52-word block. Both DSPs reach real runtime traffic within the 5-second capture (350K+ runtime
  words, 36K host-command IRQs on top of the program transfer), but what any of it means is
  undecoded. That's the next tracing session, and it's the hard part: correlate known
  machine/parameter changes (driven via sysex or the front panel, see md-mm's `mdautomation.cpp`/
  `mdsysexautomation.cpp` for how to script that) against the word stream, the way Monomodule's
  `HostModel.cpp` was worked out for the Monomachine.

- **2026-09-27: runtime protocol decoded (step 1 complete).** Details in `docs/PROTOCOL.md`
  "Runtime protocol". In short:
  - The ColdFire drives both DSPs by **writing fixed-layout structures in their Y memory** (host
    command `P:$12` = DMA block write, `P:$10` = peek), on a 96-sample control tick.
  - **DSP2 = 16 voice slots** at `Y:$800+$40·k`: word 0 = trigger/machine code on the trigger tick,
    words 1-12 = machine coefficients the ColdFire computes from SYN1-8 (+LFO).
  - **DSP2 sends DSP1 16 separate dry mono voice streams** (32-sample blocks per voice). So all
    voices from one instance and per-voice outs are both possible.
  - **DSP1 runs each track's effects page** (AMD, EQ, filter, SRR, distortion; raw params at
    `Y:$200+$40·k`), vol/pan/sends (`Y:$100+5·k`), the mix and the master FX. So Machinedrum One
    needs DSP1's per-track section, not just DSP2.
  - **Load:** DSP2 ~35-60 M instr/s with voices playing (inactive voices ~free); DSP1 ~79 M instr/s
    always. That's roughly 5-7× Monomodule's one track: ~3-4 Force cores if both are emulated.

  **Decisions needed from the user before step 2** (see `docs/PROTOCOL.md` "Design consequences"):
  (1) how to generate the 12 per-machine coefficient words (tables swept from the user's ROM at first
  run, running the ColdFire's own routine in Musashi, or per-machine reverse engineering), and
  (2) the CPU strategy for the Force (native C++ reimplementation of DSP1's per-track chain/mixer vs.
  emulating both DSPs, and whether to target desktop first).

  Tools: `tools/mdtrace` (patches + `mdProbe` scripted driver + analysis scripts; see its README) and
  `tools/mddis` (disassembler for the `.syx` DSP programs).

- **2026-09-27: decisions made, and the coefficient routines found.** The user chose:
  (1) **the MD's own routines** for the voice coefficients, and (2) **a bit-exact native C++
  translation** of DSP1's per-track effect chain and mixer (fixed-point arithmetic matching the
  DSP56300, verified sample by sample against the emulated DSP1), rather than emulating DSP1.

  (1) turned out to be cheap and clean: the OS has a 135-entry machine descriptor table whose
  coefficient functions are pure `fn(out, params)` C functions (see `docs/PROTOCOL.md`, "Host
  model: the ColdFire side"). Plan: load section 0 of the user's `.syx` into a Musashi instance and
  call the function for the voice's machine. Remaining for (1): translate how the OS builds the
  per-track 24-value parameter array (`a6`: kit value scaling, LFO, smoothing, pitch) and the tick
  routine's DSP1 values.

  **Next steps:**
  1. Find where `a6` is built (watch writes to the per-track parameter arrays; start from the tick
     routine around `$20b1a0` and its callers) and translate the LFO/smoothing code.
  2. Prototype `md::VoiceEngine`: DSP2 alone in dsp56300 (program from section 1), a harness that
     writes the 16 voice slots directly into Y memory and reads the ESSI0 link (16 × 32-sample
     blocks), Musashi calling the machine functions. Check it sample-for-sample against md-mm.
  3. Disassemble DSP1's per-track chain (hot loops at `$16e-$196`, `$95c-$9a0`) and start the
     bit-exact C++ translation, with a DSP1-in-emulator reference test.
  4. Measure DSP2 alone on the Force (static recompiler).

- **2026-09-27: step 1 of the host model done: the per-track parameter pipeline is decoded.**
  Smoothing (same slew as the Monomachine), per-track LFO (tempo-relative oscillator, 8 shape
  functions, mix and depth) and the LFO apply that builds each voice's 24-value parameter array
  are all self-contained routines in the OS; they run per sequencer tick (64 per beat at 120 BPM).
  Measured cost of running them plus the machine functions in a 68k emulator: ~1 M 68k
  instructions/s in total. Plan (see `docs/PROTOCOL.md`, "Host model plan: hybrid"): our C++ owns the
  tick schedule and inputs; the MD's own routines do the maths in Musashi. Next: translate the tick
  routine's per-voice orchestration (triggers, accent/velocity, DSP1 per-track values), then
  prototype `md::VoiceEngine` (step 2 below).

- **2026-09-27: voice engine prototype works, bit-exact.** `engine/VoiceEngine` (DSP2 alone from the
  `.syx`, Monomodule-style harness, 16 voice outputs per 32-sample block) matches md-mm's output
  sample for sample; `engine/MachineRunner` (the OS's machine functions in Musashi) reproduces the
  slot words exactly. Build with `tools/build_proto.sh` (x86, against md-mm's built libraries);
  tools `mdvoice` and `mdmachine`. Details in `docs/PROTOCOL.md`, "Voice engine prototype".

  **Next:**
  1. Host model (`engine/HostModel`): tick schedule (64 per beat), smoothing + LFO via the OS's own
     routines in `MachineRunner`'s CPU (load the internal-SRAM routine copy from OS `$2622f4` to
     `$1000000`), trigger codes, then per voice `compute()` → `VoiceEngine::setSlot()`. Translate the
     tick routine's trigger/accent/velocity handling and DSP1 per-track values (`$20af52-$20b44c`).
  2. End-to-end test: kit + triggers through HostModel + VoiceEngine vs. md-mm (mdProbe ESSI dump).
  3. Measure every machine family's DSP2 cost (EFM, E12, P-I, ROM); ROM/RAM machines also need the
     user's sample data (flash, not in the `.syx`).
  4. DSP1 per-track chain: disassemble and start the bit-exact C++ translation.
  5. Port VoiceEngine to `libs/dsp56300` (arm32 branch): needs md-mm's DSP fixes (MERGE, DMA) checked.

- **2026-09-27: host model, first version, bit-exact end to end.** `engine/HostModel` (tick schedule,
  machine assignment applied at trigger, trigger codes, per-voice machine functions, smoothing and
  LFO via the OS's own routines) + `engine/VoiceEngine`: TRX-B2 and TRX-SD each 6,400/6,400 samples
  identical to md-mm. Tick scheduling decoded: DSP2-driven interrupts, CPU-bound ~120 Hz, not tempo
  synced (see `docs/PROTOCOL.md`, "Host model: tick scheduling"). Tool: `mdhost`.

  **Next:**
  1. ~~LFO configuration and trigger restart~~ done 2026-09-27: `HostModel::setLfo()`, trigger
     flag + the tick's trigger path; tick-by-tick identical to md-mm with an LFO on PTCH.
  2. ~~Measure DSP2 cost per machine family~~ done: +1.5-3.6 M instr/s per playing voice over a 6.2 M/s
     baseline (docs/PROTOCOL.md "DSP2 cost per machine"). Next for cost: skip silent voices in the
     harness.
  3. Mixer DSP: translate the `Y:$100+5·k` computation (volume/velocity/accent, pan, sends); then
     the bit-exact C++ translation of DSP1's per-track chain.
  4. ROM/RAM machines: sample data from the user's flash (not in the `.syx`).
  5. Port to `libs/dsp56300` (arm32) and measure on the Force.

- **2026-09-27: silent voices skipped in the harness.** `VoiceEngine::installHarness` now redirects
  the per-voice render call to a fast clear when the voice's persisted machine code is 0 (never
  triggered) or 1 (the empty machine GND--, which all tracks default to at boot) — both cases
  already output 32 zeros, so this only removes the cost of getting there. Confirmed
  byte-identical output to the pre-patch engine for 200 blocks, both idle and with a playing voice.
  Baseline (16 silent voices) 6.2 → 2.7 M instr/s; one playing voice + 15 idle 8.0 → 4.8 M instr/s.
  Details and a gotcha (this assembler's `beq`/`bra` take a raw relative displacement, not an
  address) in `docs/PROTOCOL.md`.

  **Next:**
  1. ~~Mixer DSP: translate the `Y:$100+5·k` computation~~ done; ~~bit-exact C++ translation of DSP1's
     per-track chain~~ done (see the next entry).
  2. ROM/RAM machines: sample data from the user's flash (not in the `.syx`).
  3. Port `VoiceEngine` to `libs/dsp56300` (arm32) and measure on the Force.

- **2026-09-27: mixer DSP per-track chain translated, bit-exact; host model sends the mixer words.**
  - `HostModel` computes each track's DSP1 inputs as the OS tick does: the 9 effect words and the 5
    mix words (route; VOL gain from level, velocity/accent and VOL; PAN; REV; DEL), with the OS's own
    level slew (`$100029e`) and new `trigger(track, velocity, accent)`, `setLevel`, `setMute`,
    `setRouting`. Identical to md-mm's DSP1 writes.
  - `engine/TrackFx` (+ `engine/Dsp56.h`, DSP56300 fixed-point helpers): AMD, EQ, both filter
    sections, SRR, distortion, translated from DSP1's per-track function. **3.2 M samples and all
    state identical** to the DSP's own code in the emulator (`tools/mdmix`: `MixerRef` reference,
    `mdfxtest`), fixed and moving parameters. 16 tracks ≈ 3% of one x86 core.
  - Details in `docs/PROTOCOL.md`, "Mixer DSP inputs" and "Mixer DSP per-track chain".

  **Next:**
  1. ~~The mix~~ done; ~~wire it together~~ done (next entry).
  2. End-to-end comparison with md-mm's audio output (needs the master FX, below).
  3. ROM/RAM machines: sample data from the user's flash (not in the `.syx`).
  4. Port `VoiceEngine` to `libs/dsp56300` (arm32) and measure on the Force.

- **2026-09-27: the mix translated, bit-exact; the engine renders audio.**
  - `engine/Mixer`: pan law, VOL gain, reverb/delay sends, individual-output routing, from DSP1's
    mix code (`$294-$341`, `$9de` and the code it generates). 384,000 words identical to the DSP's
    own code (`mdmixtest`).
  - `engine/Engine`: HostModel → VoiceEngine → 16 × TrackFx → Mixer; outputs dry main L/R, the two
    sends, the individual outputs and each track's post-effects signal. `tools/mdrender` renders a
    demo pattern to a WAV: 8 s in ~1.3 s on x86, nearly all of it the voice DSP emulation.

  **Next:**
  1. Machinedrum FX: translate the master section (`P:$344-$970`) the same way; then One + FX can be
     compared end to end with md-mm's audio.
  2. ~~Port `VoiceEngine` to `libs/dsp56300` (arm32)~~ done (next entry); still needed: measure on the
     Force (no physical device in this session).
  3. ROM/RAM machines: sample data from the user's flash (not in the `.syx`).
  4. The plugin itself (wrapper, skin, `vst.json`) following `mpc-vst-monomodule`.

- **2026-09-27: engine cross-compiles and runs correctly for 32-bit ARM (the Force).** No physical
  device in this session (sandboxed container) — verified with cross-compilation + `qemu-arm`, not
  on-device timing. `docs/PROTOCOL.md`, "Force port" has the details; summary:
  - One real fix needed: on a target with no JIT, this fork's interpreter opcode cache must be
    turned on explicitly (`setInterpreterEnabled(true)`) or `exec()` calls through a null instruction
    pointer on the first instruction. Done in `VoiceEngine`'s constructor (guarded by
    `!dsp56k::g_useJIT`, so x86 is unaffected).
  - **Portability proved**: this fork's interpreter, forced on x86 too
    (`-DDSP56K_NO_JIT_RUNTIME`), matches the armhf cross-build byte-for-byte on the full engine
    (8 s demo render). The port itself is sound.
  - **Found and fixed a real gap**: `op_Merge` was an unimplemented stub in `libs/dsp56300`; ported
    the real implementation from gearmulator-md-mm's separate fork, adapted to this fork's
    accumulator representation. Pushed to `sd88me/dsp56300` branch `armhf-interp-merge-fix`
    (this repo's `libs/dsp56300` submodule now points there). Not exercised by the current demo kit.
  - **Open, not blocking**: bisecting the demo kit found that TRX-SD (only, of 6 machines checked)
    produces different DSP2 audio between this fork and gearmulator-md-mm's fork, diverging after
    2 blocks. Not yet root-caused (Tcc and LRA inspected and ruled out). Since this session never
    checked TRX-SD's *audio* against real hardware (only its coefficient words, elsewhere), it's
    not yet known which fork is right. Needs either a real hardware capture of TRX-SD or an
    instruction-level trace diff between the two forks to resolve.
  - **Still needed**: real Force timing (this session has no device access) — re-run the static
    recompiler's discovery step (`libs/dsp56300`'s `tools/arm32jit_prototype/recomp/`, ~3.8-3.9x
    over the interpreter for Monomachine machines) against DSP2's program, once on-device.

- **2026-09-27: real Force timing, first number, and cross-arch bit-exactness confirmed on
  hardware.** `mdrender_arm` copied to the Force and run against the user's OS `.syx`: **8.0 s of the
  6-track demo kit rendered in 23.6 s (294% of real time)**, interpreter only, no static recompiler —
  and it produced the exact same WAV as an x86 build of the same commit forced onto the plain
  interpreter (`-DDSP56K_NO_JIT_RUNTIME`, same trick as the earlier qemu check): both `md5
  bcaf9ded0bc6ea4659a6eacb939f0cf1`. So the ARM port's determinism claim (previously only checked
  under qemu) now holds on the real device too.

  294% is ~3x too slow for 6 of 16 tracks, but expected at the interpreter-only stage: HANDOFF's own
  plan was always interpreter-for-correctness-first, static-recompiler-for-speed-second. If DSP2/DSP1
  get a similar speedup to the ~3.8-3.9x the recompiler measured for Monomachine machines, that's
  ~77% of real time — inside budget. Confirming this is now the load-bearing next step, not a
  nice-to-have.

  One build wrinkle worth keeping: `tools/build_proto.sh`'s x86 reference build links against
  md-mm's *own* dsp56300 fork, which predates `setInterpreterEnabled`/the ARM port's other API
  additions — building the current `engine/` sources against it fails to compile. The x86 side of
  this check instead built `libs/dsp56300` (our arm32 fork, the same one `mdrender_arm` used) natively
  for x86, with the JIT forced off. `build_proto.sh` itself still targets md-mm's fork and hasn't been
  updated; do that (or note the split) before relying on it again for anything touching `VoiceEngine`.

  **Next:** static recompiler discovery pass (`libs/dsp56300/tools/arm32jit_prototype/recomp/`)
  against DSP2's program from the user's `.syx`, then measure the recompiled version on the Force the
  same way.

- **2026-09-27: recompiler discovery pipeline adapted to Machinedrum; generated program crashes at
  DSP init, not yet root-caused.**
  - New `tools/mdrecomp/mdrecomp_discover.cpp`: a discovery tracer for DSP2 (`VoiceEngine` alone,
    same generic `DSP::s_recompTraceHook`/`getRecompInfo` infrastructure `arm32jit_prototype/recomp`
    already provides — it isn't Monomodule-specific). Drives every machine in the OS's descriptor
    table (skipping id 0/1, never rendered) through 6 coefficient sweeps each, with a mid-decay
    retrigger, to exercise parameter-dependent branches. `recomp_gen2.py` on the trace: **1527
    blocks, 98.4% instruction coverage, 264 whole-loop blocks**, from a 6.7 s x86 run.
  - Built a gated x86 gate build (`-DDSP56K_RECOMP -I<dir with dsp56k_recomp.inl>
    -DDSP56K_NO_JIT_RUNTIME`, our own `libs/dsp56300` fork — see the build-split note two entries
    up). **It segfaults before rendering anything**, inside `VoiceEngine`'s constructor/`reset()`
    (program init, not even the demo pattern): `op_ResolveCache` dereferences a null
    `OpcodeInfo*` for a bogus opcode word (`0x000800`) at PC `$65`. The generated `.inl` has no
    `recompBlock<$65>` (the block before it, at `$64`, is one word and should fall through to `$65`
    normally) — so `$65` must be running the plain interpreter, reading a P-memory word that isn't
    what's really there at that point in execution. Not yet resolved; candidates not yet checked:
    whether the opcode-cache entry struct's layout changes size under `DSP56K_RECOMP` in a way one
    translation unit doesn't agree with (ODR/ABI mismatch), or whether `VoiceEngine::installHarness`'s
    P-memory patching interacts with the recompiled-block dispatch's word-verification differently
    than plain interpretation.
  - **Not a dead end**: the discovery pipeline itself (tracer, generator, coverage) worked correctly
    and is reusable; the bug is in what runs after, specific to enabling `DSP56K_RECOMP` for this
    program. Needs isolating (bisect which of the 1527 blocks is actually active near PC $64-$70;
    or try recomp with a trimmed `.inl` containing only later, more-exercised blocks, to check
    whether the whole mechanism or just this early one is broken) before it's safe to cross-compile
    and try on the Force.
  - Housekeeping: this session downloaded `gdb` + its runtime deps as loose `.deb`s (via
    `apt-get download` + `dpkg-deb -x`, no root) into `/tmp/gdbroot`, since apt/dpkg needs root and
    wasn't available interactively. Not installed system-wide, nothing added to the repo.

  **Next:** root-cause the segfault (see candidates above) before re-measuring on the Force.

- **2026-09-27: recompiler segfault root-caused and fixed (two real bugs, both in shared
  `libs/dsp56300`, not Machinedrum-specific) — recompiled DSP2 verified bit-exact and measured on
  the Force: 294% → 141% of real time.**
  - **Bug 1**: `Opcodes::getFieldInfo()`'s backing table (`g_runtimeFieldInfos`, `opcodes.cpp`) was a
    namespace-scope global. Any translation unit that also constructs its own `Opcodes` object at
    global/static-init scope (our discovery tracer does, mirroring `mnm_recomp_discover.cpp`) races
    it: C++ doesn't order dynamic initialization between TUs, so which one runs first is an
    accident of link order. When ours ran first, every opcode field lookup silently failed and the
    tracer misclassified real instructions as invalid. Root-caused by writing a ~10-line minimal
    repro (`Opcodes g_ops;` at namespace scope, one lookup) that reproduced with the library alone,
    no engine code involved — confirming it wasn't Machinedrum-specific. **Fixed**: made
    `g_runtimeFieldInfos` a function-local static in `getFieldInfo()` (construct-on-first-use,
    immune to cross-TU ordering).
  - **Bug 2, the actual crash**: the DSP56300 boot-protocol program loader (`dspBootCode.cpp`, used
    to stream DSP2's program in over HI08 — see "Runtime protocol" above) writes P memory directly
    and invalidates only the JIT's block-chain cache (`Jit::notifyProgramMemWrite`), never the
    static recompiler's per-block verification cache (`DSP::notifyProgramMemWrite` →
    `recompInvalidate`) — because it bypasses `memWriteP`, the one path that calls both. A block
    that verified true against not-yet-fully-streamed P words then stayed cached as "verified"
    forever, since nothing ever invalidated it once the rest of the boot transfer overwrote those
    same words with the real program — so the recompiled dispatch ran stale, wrong code, corrupting
    execution until it landed on a bogus opcode and crashed. **Fixed**: the boot loader's per-word
    write now also calls `DSP::notifyProgramMemWrite` (made public for this; was private, only
    reached internally via `memWriteP`). Likely latent in `libs/dsp56300` generally, not just for
    Machinedrum — anything whose program load goes through this exact boot-protocol path and then
    gets recompiled could hit it; Monomodule's tooling apparently loads differently (or never
    revisited the affected addresses before boot fully finished) and never tripped it.
  - Both fixes are in `libs/dsp56300`, pushed to `sd88me/dsp56300` branch `recomp-fixes` (built on
    top of the earlier MERGE-op fix commit, same as that branch).
  - **Verified**: rebuilt discovery (`tools/mdrecomp/mdrecomp_discover.cpp`, still 8228 distinct
    instructions, now with correct lengths — 840 blocks instead of 1527, 97.3% coverage, since
    correct lengths merge more instructions per block), rebuilt `mdrender` x86 with `-DDSP56K_RECOMP`:
    **byte-identical WAV** (md5 `bcaf9ded...`) to the plain-interpreter x86 build. Cross-compiled for
    armhf and ran on the real Force: same md5 there too, and **8.0 s rendered in 11.3 s — 141% of
    real time**, down from the interpreter-only 294% measured two entries up. Roughly a 2.1x
    speedup on-device (less than the ~3.8-3.9x Monomachine measured; DSP2's program/instruction mix
    differs, and only 97.3% of instructions got recompiled here).
  - Still 41% over budget for this 6-of-16-track demo kit; a full 16-track pattern needs more. Not
    yet tried: whether coverage or block quality improves with a broader discovery sweep (more
    machines' edge cases, or ROM/RAM machines once flash sample data is available), or whether the
    remaining 2.7% uncovered instructions are concentrated in something hot.

  **Next:**
  1. ~~Push both `libs/dsp56300` fixes to a branch~~ done (`recomp-fixes`). Consider upstreaming bug 2
     (boot-protocol invalidation gap) since it's a real, generally-applicable correctness bug, not
     Machinedrum-specific.
  2. Try to close the gap to real-time: wider discovery coverage, and/or measure where the
     remaining time actually goes (per-machine cost, same as the interpreter-only breakdown earlier
     in this doc) now that the recompiled build is trustworthy to profile.
  3. DSP1 (mixer/FX) is already native C++, not part of this — this was all DSP2 (voice engine).
  4. ROM/RAM machines: still need the user's flash sample data.
  5. The plugin itself (wrapper, skin, `vst.json`).

- **2026-09-28: full 16-track kit measured on the Force — 231% of real time, the gap is bigger than
  the 6-track number suggested.** Built an ad-hoc benchmark (not committed — a throwaway variant of
  `tools/mdrender/mdrender.cpp` with all 16 tracks assigned instead of 6: a mix of TRX, EFM and E12
  machines, several with dense 16th-note patterns, no silent tracks). Recompiled build, same
  `.inl` as the 141%-measurement above: **29% of real time on x86, 231% on the real Force** for 8 s
  of this pattern. So a genuinely busy full kit is over 2x too slow even with the static
  recompiler — the earlier 141% (6 of 16 tracks, some idle) undersold the gap for a real
  performance target.
  - Not yet done: a per-machine-family cost breakdown on the recompiled build (the interpreter-only
    breakdown in "DSP2 cost per machine" above is stale now that the recompiler changes the constant
    factor). That's the next useful measurement before deciding where to spend further optimization
    effort — e.g. whether cost concentrates in a few expensive machine families (letting some voices
    stay interpreted while cheap ones get the recompiler's full benefit is not how it works today:
    recompilation is program-wide, not per-machine) or is roughly uniform.
  - Given the scope of closing this gap further (profiling, and likely real work on interpreter
    per-instruction overhead or the discovery sweep's coverage), this is a substantial next
    investigation in its own right, not a quick follow-up — left for the next session rather than
    rushed here.

- **2026-09-28: per-machine cost profiled on the recompiled build (x86) — no single hot machine;
  costs are fairly uniform, so the fix isn't "avoid machine X."** Single voice active, others
  silent (already near-free per the harness), 2000 blocks/machine, several coefficient sweeps:
  **baseline (all 16 silent) 24.3 µs/block; cheapest active machine ~24.6-24.7 µs (+0.3, matches
  "inactive/settled voices are nearly free"); most expensive (P-IRC, P-IMT, P-IHH, P-ISD, EFMHH,
  P-ICC, TRXMA — all ~40-42 µs) only ~1.7x the baseline.** A block's real-time budget is 32/44100 s
  = 725.6 µs; even 16 simultaneous worst-case machines wouldn't come close to that on this x86 box
  (~307 µs), yet the real 16-track pattern measured 231% (~1676 µs/block-equivalent) **on the
  Force** — consistent with the Force's interpreter being roughly 6.5-8x slower than this x86 dev
  machine for the same workload (matches the ratio between every x86/Force pair measured so far:
  45%→294% and 19%→141% for the 6-track kit, 29%→231% for the 16-track kit). That's an ARM-vs-x86
  raw-speed gap, not a code hotspot — no amount of "which machines are cheaper" analysis closes it
  by itself.
  - **Concrete next lever, not yet attempted**: split the 16 voice slots across the Force's multiple
    CPU cores. DSP2 is one emulated chip processing all 16 slots per call, but nothing prevents
    running *two or more `VoiceEngine` instances* in parallel, each fed only a subset of the 16
    slots (the rest left silent, which the existing silent-voice harness optimization already makes
    nearly free in that instance) — each thread's cost ≈ baseline + its own subset's active-voice
    cost, on separate cores. This was flagged as a possibility from the very start of this project
    (see "The plan", step 3: "The MD may need voice subsets... to fit a core") and the profiling
    here confirms per-voice costs are additive and roughly independent, which is exactly what a
    split like this needs to be worth doing. Splitting 16 into 2-4 groups could plausibly close most
    or all of the 231%→100% gap. Not implemented: this is a real architecture change (multiple
    engine instances, per-block thread sync, merging each instance's 8 real + 8 silent-slot outputs
    into one), worth its own planning session rather than rushing unsupervised.

- **2026-09-28: multi-core voice split prototyped (`engine/ParallelVoiceEngine`); real hardware
  test possibly caused the Force to reboot — treat this as unconfirmed but concerning, not as a
  working result.**
  - Implemented `ParallelVoiceEngine` (splits the 16 voice slots round-robin across N
    `VoiceEngine` instances, one `std::thread` per group per block, merged by voice index — silent
    voices in a group are free, same as the existing single-engine harness optimization) and
    templated `HostModel`/`Engine` on the voice-engine type (`HostModel<TVoices>`, `EngineT<TVoices>`,
    with `Engine = EngineT<VoiceEngine>` and `ParallelEngine = EngineT<ParallelVoiceEngine>` aliases)
    so both the existing single-engine path and this one share all the code above VoiceEngine.
    Verified on x86: output is bit-identical regardless of group count (1/2/4), confirming the
    split/merge logic is correct; not a speed win on x86 (that dev box is fast enough that
    per-block thread spawn overhead exceeds the compute saved).
  - **The Force has 2 cores, not the 3-4 this project's early docs guessed** (`nproc` = 2) — so at
    most a 2-way split is possible here, not 4-way.
  - Cross-compiled and copied to the device to measure `groups=1` (238% — consistent with the
    231% measured earlier, same workload) and `groups=2`. **The device rebooted during or right
    after the `groups=2` run** (`uptime` showed ~1 minute afterward; no persistent journal survived
    the reboot to show a cause). MPC came back up and was running normally afterward, and this
    session's files were cleaned off `/tmp`, but **the cause is not confirmed** — could be this
    test (spawning extra threads once per 32-sample/725µs block, ~11,000 times over the 8 s test,
    on a `PREEMPT_RT` kernel that may be running the audio path at real-time priority — thread
    creation at that rate on that kernel is a real suspect) or an unrelated device event. **Do not
    re-run `groups=2` (or any multi-threaded variant) against the physical Force without the user
    present and aware of this**, and don't treat the x86 groups=1/2/4 numbers above as informative
    for the real device — they were never meaningfully compared on-device before the reboot.
  - Before trying this on hardware again: per-block `std::thread` creation is the wrong design
    regardless of the reboot question — at 725µs/block, spawn+join overhead alone is likely a
    meaningful fraction of budget on this hardware. A real implementation needs a persistent
    thread pool (threads created once, woken per block via a condition variable or similar), not
    threads spawned fresh every block. `ParallelVoiceEngine::renderBlock` as written now is a
    correctness prototype only, not close to production-ready.

- **2026-09-28: adjustable voice cap added (`HostModel::setMaxActiveVoices`), as a lower-risk
  alternative to the multi-core split above — real 16-simultaneous-voice patterns are unlikely in
  practice, so bounding worst-case load this way avoids the threading/reboot question entirely.**
  - `HostModel<TVoices>::setMaxActiveVoices(int)` (1-`kTracks`; default `kTracks` = disabled,
    identical to current behavior — verified bit-exact, same md5 as every prior render). Tracks the
    N most-recently-triggered tracks; a new distinct trigger past the cap steals the
    least-recently-triggered one by force-triggering it to the OS's own empty machine (id 0,
    `GND--`, no params) — using the normal `MachineRunner::compute()` + `VoiceEngine::setSlot()`
    path, not a raw memory hack, so it stays correct by construction. The victim's assigned machine
    (`m_machine`/`m_pendingMachine`) is untouched, so its next real trigger plays normally; this is
    a hard cut (no fade), which is the standard tradeoff for simple voice-stealing, not something
    fixed here.
  - **Measured and it's more nuanced than hoped**: on the stress-test 16-track pattern (all 16
    tracks retriggering almost every 16th note — much busier than realistic use), lowering the cap
    from 16 down to 2 plateaus around 19-20% of real time on x86 (was 29% uncapped) and doesn't go
    lower, because capping only skips DSP2's *render* cost for stolen voices — it doesn't skip
    `MachineRunner::compute()` (the 68k-emulated machine coefficient function), which still runs on
    every trigger regardless of whether that voice ends up immediately silenced. For this
    artificially dense pattern, that computation cost turns out to be comparable to DSP2's own
    render cost.
  - **Reframing the actual target**: this stress pattern (every track retriggering continuously) is
    not how a real Machinedrum kit gets programmed. The original sparse 6-track demo pattern -
    much more representative - is already comfortably inside budget on the recompiled build (45%→19%
    on x86; by the established ~6.5-8x x86/Force ratio, likely 50-60% on the Force, i.e. real-time
    with headroom, not yet directly re-measured on-device since the last hardware session ended out
    of caution after the reboot). So for realistic use, the voice cap is insurance against pathological
    edge cases, not a required fix.
  - **Next, if pushing the stress-test number further matters**: profile `MachineRunner::compute()`'s
    cost per call (already known aggregate, ~1 M 68k instructions/s total, from "Host model: first
    version" above, but not broken out per-trigger under heavy simultaneous-retrigger load) and
    consider skipping it for voices already marked for stealing before computing, not just after.

- **2026-09-28: step 5 (the plugin) started — Machinedrum One's `mpc_engine()`, offline-verified.**
  Following `mpc-vst-monomodule`'s architecture: `vst/engine.cpp` (all 16 voices from one instance,
  MIDI note → track, a persistent render thread — not per-block spawn, unlike yesterday's
  `ParallelVoiceEngine` prototype — feeding a ring buffer the host's audio callback drains). V1
  params are deliberately minimal (per track: machine id, level, pan; global: tempo, max_voices);
  per-track FX/LFOs aren't exposed yet. Top-level `CMakeLists.txt` + `cmake/dsp56300.cmake` mirror
  `mpc-vst-monomodule`'s structure (an `MPC_VST_DIR`-gated plugin target, plus `vst/smoke.cpp` for
  an offline end-to-end test — see `mpc-vst-plugin` skill's pipeline, step 4).
  - **First end-to-end test caught two real bugs** neither of which any earlier tool (mdrender,
    mdrendercap, mdrenderpar) had exercised, since those always set every per-track param
    explicitly: "level" was wired to `HostModel::setLevel` (a separate kit LEV knob), not param 17
    VOL, which is what the mixer's own gain formula actually reads — silent regardless of
    everything else. And FLTW (filter width) defaults to 0 (closed) unless set, same as EQF/EQG —
    now defaulted per track in `engine.cpp` (matching `mdrender.cpp`'s working demo kit), since
    they aren't yet exposed as VST params. Fixed; verified real audio (peak 5710/32767), zero
    underruns, ~12% CPU for 8 s of real time on x86.
  - **Deliberately stopped here for this session**: no `.so` build yet (needs `MPC_VST_DIR` +
    the armhf Docker toolchain from `mpc-vst-plugins`), no skin/`TUI.json`, nothing touching the
    physical Force. Given yesterday's reboot incident, any step from here that reaches the device
    (bench, deploy, register/restart) should happen with the user present, per the `mpc-vst-plugin`
    skill's own "ask before restarting MPC" rule.
  - **Next:** build the actual `.so` (offline, no device) via `mpc-vst-plugins`' `tools/build_port.sh`
    or this repo's own CMake + `MPC_VST_DIR`; run the skill's `tools/test_port.sh` (ASan/UBSan host
    test) and `tools/bench.sh` before any device step; expose per-track FX/LFO params; then a skin.

- **2026-09-28: `machinedrum_one.so` built for the Force, verified offline under QEMU.**
  `vst/build_so.sh` (+ `tools/Dockerfile.armhf-builder`, `tools/armhf.cmake`) mirrors
  `mpc-vst-monomodule`'s own armhf cross-build: `debian:bookworm`'s `crossbuild-essential-armhf`
  (glibc ~2.36, staying below the Force's 2.39) rather than this session's own local
  `arm-linux-gnueabihf` toolchain, which is too new for a *shared* library that must dynamically
  link against the device's already-loaded glibc (fine for the static standalone test binaries
  built with it earlier, wrong for this).
  - Found and fixed a real bug this surfaced: `libs/gearmulator-md-mm/source/mc68k` was a PUBLIC
    include dir on `mdcore`, leaking into `vst2_wrap.c`'s plain-C compile and shadowing the
    sysroot's real `<endian.h>` with mc68k's own (C++-only) one. Now PRIVATE.
  - Verified: one exported symbol (`VSTPluginMain`), highest `GLIBC_2.36` (device has 2.39),
    libstdc++ statically linked (matches the skill's `-fvisibility=hidden`/`-Bsymbolic`/
    `--exclude-libs` setup for multiple plugin instances sharing MPC's process). Ran the `.so` +
    `md-vst-smoke` under Docker's QEMU-backed `--platform linux/arm/v7` against the user's own
    `.syx`: real audio (peak 14578/32767), no crash — a functional check only, not a timing
    measurement (QEMU emulation speed says nothing about the real device).
  - **Still nothing touching the physical Force**: no deploy, no `MPC.settings` edit, no restart.
    Per the skill's own rules and the reboot incident two entries up, that step needs the user
    present.
  - **Next:** the skill's `tools/test_port.sh` (ASan/UBSan) if it can be adapted to this repo's
    CMake-based build rather than its own generic `sources` compile step; per-track FX/LFO params;
    a skin; then, with the user present, `tools/bench.sh` and the actual device deploy/register.

- **2026-09-28: the skin's foundation — bit-exact real LCD capture, confirmed working, following the
  same idea as Monomodule's skin but a more direct route for this project.** Monomodule's
  `mnm_artdump.cpp` gets pixel-exact LCD art by calling upstream Monomodule's own reverse-engineered
  font/icon decoders (`RomArt.cpp`/`SpecData.cpp`) on the user's OS file — years of prior community
  work this project doesn't have an equivalent of for the Machinedrum. But `gearmulator-md-mm`
  *full-system emulates the real LCD controller*, so the real ColdFire OS's own UI code, running in
  Musashi exactly as on hardware, produces real pixels we can just read
  (`md::FrontPanel::getLcdPixel(x,y)`, 128x64, 1-bit) — no font reverse-engineering needed at all.
  - Confirmed end to end: built `mdProbe` (`tools/mdtrace`, needs only `mdLib` — no RmlUi/freetype/
    cpp-terminal despite the full project needing those submodules present to configure) against
    the user's full flash image, and added a new `lcdpng:PATH.ppm` action (alongside the existing
    ASCII `lcd` action) that writes the real framebuffer as a binary PGM. Converted to PNG (a few
    lines of `zlib`, no ImageMagick/PIL needed) and viewed: **a pixel-perfect capture of the real
    Machinedrum's home/track screen** — BPM readout, LEV/PTCH/DEC/RAMP/HOLD column labels, kit
    number, track name, pattern name, exactly as real hardware renders it.
  - **Never commit captured LCD art** (this session's test screen was viewed then deleted, never
    added to the repo) — same policy as the ROM/flash image and the recompiler's `.inl`: it's
    Elektron's own content, per-user build data only. A real skin-generation script (this project's
    analog of Monomodule's `mk_skin.py`) would call `mdProbe` with `panel:`/`sysex:` actions to
    reach each screen it needs (track select, parameter pages, kit browser, ...) and `lcdpng:` to
    capture each one, at build time, from the user's own firmware — never bundling the images
    themselves.
  - **Not yet done** (the actual `mk_skin.py`-equivalent is real, scoped work, not attempted this
    session): enumerating which MD screens the plugin's `TUI.json` needs, the `panel:`/`sysex:`
    sequences to reach each one, and the Python generator that composites captured LCD crops (plus
    the skin's own knob/button chrome, likely via the `mpc-vst-plugin` skill's
    `layout.conf`/`shadow_skin.py` path) into `TUI.json` + PNGs.
  - Housekeeping: this needed the user's full 8 MB flash `.bin` (not just the `.syx`), already on
    disk from the earlier tracing session (`/home/sam/roms/machinedrum/`), and a flash-cache file
    (`mdProbe`'s second argument) that `mdProbe` generates on first run and reuses after — also
    derived-from-firmware, also never committed.

- **2026-09-28: real screen navigation confirmed; a reusable capture tool committed.**
  `mdProbe`'s `panel:` action only recognized 11 hand-picked button names; rebuilt its map from the
  full `PanelControl` enum via `panelControlName()`, so every real MD button (`Track1`-`6`,
  `BankGroup`/`A`-`D`, `Tempo`, `SynthesisEffectsRouting`, `DataPageForward`/`Backward`, `Scale`,
  `PatternSong`, `TrigSelect`, `SongEnable`, `ClassicExtended`, plus the ones already there) is
  reachable by name. Verified `panel:SynthesisEffectsRouting` cycles the real parameter page and
  captured it: **AMD EQF EQG FLTF FLTW FLTQ** — exactly `HostModel`'s own per-track FX param order
  (params 8-14). The default (SYN) page's real layout is confirmed too: LEV + the current machine's
  8 SYN param names (e.g. TRXB2: PTCH DEC RAMP HOLD TICK NOIS DIRT DIST), matching
  `MachineRunner`'s per-machine descriptor table exactly — real, independent confirmation that the
  host-model translation lines up with what the actual hardware shows the user.
  - `tools/mdtrace/capture_screens.py`: drives `mdProbe` through a named list of screens (each a
    list of `panel:` actions replayed from a fresh boot, so screens don't depend on each other or
    capture order) and saves each as a PNG (a small pure-Python PGM→PNG conversion, no PIL/
    ImageMagick dependency, since neither is guaranteed available). Verified end to end: 3 screens
    (home, syn_page, amp_fx_page) captured and visually confirmed correct. Never commits its own
    output, same policy as everything else derived from the user's firmware.
  - **This is capture infrastructure, not the skin itself.** The actual generator (this project's
    `mk_skin.py` equivalent — deciding which screens the plugin needs, where on each real capture
    the touch/Q-Link regions go, and writing `TUI.json`) is real, separately-scoped work, not
    started. Also not yet done: exposing the AMP/EFX page's 9 params (only VOL/PAN are in
    `vst/engine.cpp`'s V1 param set today; AMD/AMF/EQF/EQG/FLTF/FLTW/FLTQ/SRR/DIST are wired in
    `HostModel` already but not surfaced as VST params yet) and the SYN1-8 params (trickier: their
    real names change per machine, unlike everything else).
  - **Next:** decide the skin's actual screen set and layout (a design step, best done with the
    user looking at real captures rather than guessed at), then build the compositor; separately,
    extend `vst/gen_params.py`/`engine.cpp` with the AMP/EFX and SYN1-8 params now that their real
    layout is confirmed.

- **2026-09-28: first real on-device test — deployed, played, fixed a real bug, confirmed working.**
  Deployed a quick checkpoint build (auto-generated `gen_vst.py` skin, no custom layout — dropped
  `custom_skin` from `vst.json` for this) to the Force: `.so` + the user's `.syx` (to
  `MODULE_DIR`) + skin, registered in `MPC.settings`, MPC restarted (device itself stayed up the
  whole time — `systemctl restart acvs` is not a reboot; `force_shadow.so` confirmed still loaded
  both times). The user played it and reported audible sound but choppy on some material.
  - **Root cause and fix**: the render thread was a plain `SCHED_OTHER` `std::thread` — exactly the
    kind of thing that can get starved under system load. `mpc-vst-monomodule`'s own DSP thread
    elevates to `SCHED_FIFO` priority 30 (above MPC's `AudioWorkers` at `SCHED_RR` 20) once booted,
    plus picks the least-busy non-UI core via `/proc/stat` sampling — ported both over verbatim
    (`MD_FIFO`/`MD_CPU` env overrides, matching `MNM_FIFO`/`MNM_CPU`). Also exposed a `core`
    get_param for future diagnostics (no remote way yet to query a live instance's params — worth
    a debug-log-file mechanism if a future issue needs it).
  - Rebuilt, redeployed (md5-verified), MPC restarted again. **User confirmed: sounds better.**
    Real-time scheduling was the actual missing piece for this architecture on real hardware, not
    a DSP or engine correctness issue.
  - Still on the auto-generated skin, not the bit-exact LCD one — that's the next real design/build
    step, per the entry above.

- **2026-09-28: pushed both open threads — AMP/EFX params exposed, and a real bit-exact LCD skin
  deployed and confirmed stable.**
  - **Params**: exposed AMD/AMF/EQF/EQG/FLTF/FLTW/FLTQ/SRR/DIST per track (`HostModel` raw params
    8-16, the real hardware's own AMP/EFX page order, confirmed against a captured real screen) —
    12 params/track × 16 + 2 globals = 194 total, up from 3/track. Renamed `level`→`vol` to match
    what it actually reads (param 17 VOL, not the separate kit LEV knob). Removed the hardcoded
    per-track FLTW/EQ startup defaults now that `gen_params.py`'s own declared defaults cover it.
    **Deliberately still not exposed**: SYN1-8 (their good defaults are per-machine, set by
    `HostModel::setMachine()` itself — a flat VST default would stomp them the moment a machine is
    assigned; needs "has the user touched this knob" tracking, not implemented) and
    DEL/REV/LFOS/LFOD/LFOM. Verified bit-exact: x86 smoke test peak unchanged at 5710/32767.
  - **Skin**: `vst/gen_layout.py` writes a 16-tab (+ GLOBAL) `layout.conf` — each tab shows the
    real Machinedrum's own AMP/EFX page (captured via `tools/mdtrace/capture_screens.py`, never
    committed itself) as a decorative header behind knobs for that track's 12 params, roughly
    column-aligned with the real screen's own AMD/AMF/EQF/EQG/FLTF/FLTW/FLTQ/SRR layout. Built via
    the browser art renderer (`"art": "html"` in `vst.json`, `mpc-vst-html-art` image). Previewed
    (`tools/studio.py preview`): genuinely reads as the real hardware's LCD content integrated with
    working MPC controls — not yet pixel-exact per-control alignment against the real column
    positions, but a real, working version of "the same philosophy as Monomodule."
  - **Deployed and confirmed stable on the Force**: rebuilt the `.so` (properties unchanged: one
    exported symbol, `GLIBC_2.36`), redeployed both `.so` and skin (md5-verified), MPC restarted
    (device uptime unaffected, `force_shadow.so` confirmed still loaded). Not yet re-tested by the
    user for sound/feel with the new knob layout and params — that's the natural next check.
  - **Next**: get the user's read on the new skin/params on-device; then either refine alignment
    (line knobs up exactly against the real screen's own column positions, per-tab instead of
    reusing one static capture) or move to the SYN1-8 "touched" tracking, whichever the user
    prioritizes.

- **2026-09-28: the static-whole-screen-capture skin approach doesn't scale — the user called it
  out (a single captured screen's labels are only accurate for the machine that happened to be
  showing when it was captured; the SYN page's labels are per-machine, so one static image can't
  serve all 16 tracks correctly). Agreed direction: rebuild "the Monomodule way" — reusable font/
  icon assets composited freely per screen/state, not baked screenshots. Plan below, not yet
  started; this is the resume point after compaction.**

  ## Skin plan: the Monomodule way (not started)

  The difference from Monomodule: upstream Monomodule already reverse-engineered the Monomachine's
  font/icon storage as data (`RomArt.cpp`/`SpecData.cpp`), so `mnm_artdump.cpp` just calls those
  decoders and `mk_skin.py` composites the result. We have no equivalent decoder for the
  Machinedrum's ROM. But we have something Monomodule's approach doesn't need: a full-system
  emulator (`gearmulator-md-mm`) that renders real pixels for anything we tell it to display, plus
  the bit-exact capture tooling already built and proven this session (`lcdpng`, `capture_screens.py`,
  full `panel:` navigation). The plan replaces "decode the font table" with "capture known text and
  slice it" — same end result (a reusable glyph/icon atlas), different, more tractable method for
  this project.

  **Phase 1 — Font extraction (the key unlock, do this first).**
  - Machinedrum kit/pattern/track names are user-editable ASCII strings (sysex, or
    `gearmulator-md-mm`'s automation tooling — `mdautomation.cpp`/`mdsysexautomation.cpp`, already
    referenced in "The plan" at the top of this doc for scripting parameter changes; check there
    for the exact rename mechanism first, don't assume).
  - Set kit/pattern/track names to strings covering every character the skin will ever need: A-Z,
    0-9, and whatever symbols appear in real captures (`:` `.` `-` `>` seen already in "KIT:01",
    "125.0", "TRX►B2", "PATTERN A01" — check for others once more screens are captured).
  - Capture the screen(s) showing those strings with `lcdpng` (already built and proven).
  - The font is fixed-pitch (visible in every capture so far — compare letter spacing in "TRX UW"
    vs "KIT:01" to get the exact cell pitch in pixels before writing the slicer). Slice the known
    grid into per-glyph 1-bit bitmaps at that pitch. This is a small new script (not started),
    output as a JSON atlas — same shape as Monomodule's own font dump (glyph index → bitmap rows),
    so `mk_skin.py`-equivalent code can reuse rendering logic in the same spirit.
  - Verify by re-rendering a captured string from the extracted glyphs and diffing pixel-for-pixel
    against the original capture — the gate before trusting the atlas for anything else.

  **Phase 2 — Icon assets. Done for the AMD dial (2026-09-28); same recipe covers the rest.**
  - Wrote `tools/mdtrace/build_dial_atlas.py` (committed). It reaches the AMP/EFX page
    (`panel:SynthesisEffectsRouting`) and sweeps a knob's full 0-127 range one step at a time via its
    rotary `encoder:` action (confirmed `PanelEncoder::DataEntryA` = the AMD knob - turning it also
    shows a temporary numeric readout under the dial, useful as an independent sanity check while
    developing this, though the script itself doesn't depend on reading that overlay), capturing the
    LCD after every step and deduping consecutive identical frames within a fixed crop region (found
    the same way as the font glyph band: full-frame pixel variance across the sweep, restricted to
    the dial's own area to exclude the numeric overlay and the LEV meter/neighboring dials, which vary
    too but aren't this icon). Result: **36 distinct rotation states across the AMD dial's 0-127
    range**, verified visually at 8x scale - a clean, correctly-ordered sweep of the pointer dot
    rotating clockwise around the dial. Output `dial_atlas.json` (`states`: list of
    `{value_first, value_last, rows}`) + `dial_atlas_grid.png`, both build output (gitignored).
  - **All 8 AMP/EFX dials swept (2026-09-28).** Confirmed the 8 dials sit in a uniform 4-col x 2-row
    grid by variance-scanning three of them (DataEntryA/B/E): same 14x12 icon box, offset by a
    constant pitch (`x0 = 52 + col*21`, `y0 = 14 + row*31`). Generalized `build_dial_atlas.py` to
    compute the crop region from the encoder's grid position (`dial_crop_for()`) instead of one
    hardcoded box, then ran all 8 (`DataEntryA`..`H` = AMD AMF EQF EQG / FLTF FLTW FLTQ SRR). Distinct
    icon-state counts varied a lot - `36 36 31 13 28 9 12 33` - and that's real hardware behavior, not
    a bug: spot-checking `DataEntryF` (FLTW)'s grid image showed the pointer rotating through only a
    small arc for values 0-26 and then staying pinned in the same position for the rest of 27-127 -
    i.e. this parameter's dial has a genuinely limited visual arc, confirmed by looking, not assumed.
  - **LEV bar-meter investigated, not yet solved (2026-09-28) - ruled out several easy guesses.**
    Tried: `encoder:Level:20` (no visible change to the bar at all - so `PanelEncoder::Level` is not
    what drives it), `encoder:SoundSelection:20` (this one *did* do something real - it changed the
    breadcrumb's machine name, i.e. it's the machine/sound-select knob - but still no LEV bar change),
    a trigger-and-decay sweep (`trig:1:256` then 20x `wait:1024` + `lcdpng`, variance-scanned the whole
    top-left quadrant - zero pixel variance across all 21 frames, meaning either the LEV bar genuinely
    didn't move or trigger 1 isn't the same track this AMP/EFX page is showing - the breadcrumb read
    "TRX►B2►TFX", i.e. bank B track 2, not trigger 1's track), and `panel:DataPageForward`/`Backward`
    (no visible change at all on this screen - may need a different starting screen or a hold/repeat
    semantics not yet tried). **Next things to try, not yet done**: align the triggered track with the
    displayed track (select bank/track A1 first, then trigger pad 1, before capturing the decay sweep)
    since a live VU-style meter is the most likely remaining explanation; if that still shows no
    movement, LEV may require actual DAC/output-stage audio routing this full-system emulation doesn't
    drive by default, or may be a static per-track "level" *setting* controlled by some other physical
    input not yet tried (e.g. one of the `Track1-6` panel buttons, or a dedicated hardware level pot
    modeled as its own `PanelControl`/`PanelEncoder` not yet probed).
  - Tried the "align track" idea immediately: `panel:BankA panel:Track1` before the AMP/EFX page.
    Result: `BankA` opens a "BANK A" **popup dialog** (confirmed by capture - a modal overlay with 4
    selectable squares), not a direct track-select; `Track1` after it didn't close the popup or
    change the active track (breadcrumb stayed on "TRX►B2►TFX"), and triggering while the popup is
    still open silenced playback entirely (peak dropped to 0, vs. ~0.17-0.22 without the popup open).
    So this specific combo doesn't reach bank/track selection - needs the popup's own confirm/dismiss
    sequence worked out first (probably `panel:Enter` after `Track1`, not tried yet) before this
    approach can be retried.
  - Not yet done: generalizing/verifying this same grid-formula approach on a different screen (SYN
    page knobs, which Phase 3 will also need labels for).

  **Phase 3 — UI structure (hand-authored, not extracted — the one part with no ROM-derived
  shortcut). Started 2026-09-28.**
  - Wrote `tools/mdtrace/ui_spec.py` (committed): the SYN page and the AMP/EFX page turned out to be
    **the same physical 4x2 dial-grid widget** - confirmed by diffing the two captures, dial positions
    are pixel-identical, only the label text and bound parameter differ. `DIAL_GRID` captures the one
    shared geometry (dial crop per cell, label text band at y=3-7 above each dial); `SCREENS["amp_fx"]`
    and `SCREENS["syn"]` each just reference it with their own label source.
  - **Independently verified the "SYN1-8 labels are free from MachineRunner" claim against real
    hardware ground truth** (not just trusted from reading the header comment): built a standalone
    `mdmachine` tool (compiled `tools/mdmachine/mdmachine.cpp` directly against the existing
    `build-vst-x86/libmdcore.a` + `mc68k`/`dsp56kEmu` static libs with a plain `g++` invocation -
    faster than a full CMake reconfigure for a one-off tool check) and ran it against the user's OS
    `.syx`. It lists every machine's name and 8 SYN param names, decoded from the OS's own machine
    descriptor table. Machine 28, `TRXB2`, reports `PTCH DEC RAMP HOLD TICK NOIS DIRT DIST` - an
    **exact match** to the SYN page capture from earlier this session (same 8 labels, same order) -
    and `TRXB2` is exactly what the real LCD's breadcrumb showed split across two segments
    (`TRX►B2►SYNT`). So `MachineInfo::params` is confirmed correct and sufficient for Phase 3/4's
    per-machine SYN labels - no manual transcription needed, and now proven, not just assumed.
  - Not yet done: pixel-verifying the label text band's column boundaries per-column (currently an
    approximate `label_col_pitch=20`, not confirmed against a third page); confirming whether any
    screens exist beyond SYN/AMP-EFX (LFO page? routing page? - `panel:DataPageForward/Backward` had
    no visible effect when tried from the AMP/EFX page, logged under the parked LEV investigation).

  **Found the third per-track page: ROUTE (2026-09-28), answering "what other Machinedrum pages are
  there?"** The `SynthesisEffectsRouting` panel button isn't a single-destination button - its own
  enum name was the clue, in hindsight. Pressing it a SECOND time (same button, not a different one)
  cycles past AMP/EFX to a page whose breadcrumb reads "ROUT": `DIST VOL PAN DEL / REV LFOS LFOD
  LFOM`, the same 4x2 `DIAL_GRID` geometry as SYN and AMP/EFX. A third press returns to a screen
  pixel-identical to the true SYN home page - confirmed by direct capture comparison, not assumed.
  Verified `DataEntryA` still maps to that page's first dial (DIST) the same way it does on the other
  two pages (turned it and read the live numeric overlay "20"). So there are exactly **three** real
  per-track pages - SYN, AMP/EFX ("TFX"), ROUTE ("ROUT") - not two, and the cycle wraps after three
  presses (no further pages reachable this way).
  - **This corrects a real placement mistake already shipped in `vst/gen_params.py`/`gen_layout.py`**:
    `DIST` was modeled as an awkward 9th param wrapping onto AMP/EFX's grid (that file's own old
    comment: "9th (dist) wraps to a 3rd row"), but real hardware's AMP/EFX page has only 8 dials
    (`AMD AMF EQF EQG FLTF FLTW FLTQ SRR`, no DIST at all) - DIST is actually ROUTE's own first dial.
    `SYN(8) + AMP/EFX(8) + ROUTE(8) = 24`, exactly matching `HostModel::kParams` - confirms the real
    split is clean 3x8, not the 2-page-with-an-awkward-9th-slot shape currently coded. **Not fixed
    yet** - `gen_params.py`'s `FX_PARAMS` list and `gen_layout.py`'s knob layout both need DIST moved
    off the AMP/EFX tab; `vol`/`pan` are also real ROUTE-page citizens on hardware (shown alongside
    DIST/DEL/REV/LFOS/LFOD/LFOM) rather than a separate top-of-tab area, though keeping them separate
    in our own skin is a legitimate design choice, not a correctness bug the way DIST's placement is.
  - `ui_spec.py` updated with a `SCREENS["route"]` entry (same `DIAL_GRID`, `reach:
    ["SynthesisEffectsRouting", "SynthesisEffectsRouting"]`, fixed labels `DIST VOL PAN DEL REV LFOS
    LFOD LFOM`). This is genuinely good news for the user's 2x2 tab-per-track design: **machine
    picker, SYN, AMP/EFX, ROUTE is a clean, real 4-quadrant mapping** - three confirmed real hardware
    pages plus the machine picker, no invented/placeholder quadrant needed.
  - Not yet done: capturing ROUTE's own dial-pointer icon atlas (Phase 2's `build_dial_atlas.py` was
    only ever run against AMP/EFX's 8 dials - ROUTE's dials likely need their own sweep per parameter,
    since dial appearance is param-specific, not page-specific; VOL/PAN/DIST/DEL/REV/LFOS/LFOD/LFOM
    each need their own `dial_atlas.json` the same way AMD/AMF/etc. did) and extracting ROUTE's label
    text into the small label font - checked, and no new capture pass is needed: every letter in
    `DIST VOL PAN DEL REV LFOS LFOD LFOM` (D I S T V O L P A N E R F M) is already in the current
    24-letter set.

  **Phase 4 — Compositor. First working version 2026-09-28.**
  - Wrote `tools/mdtrace/compose_skin.py` (committed): given the font atlas, one dial-pointer atlas
    per knob, and `ui_spec.py`'s `DIAL_GRID`, it draws a screen's 8 dial icons + labels for a chosen
    set of live values into a PNG. Ran it for the AMP/EFX screen (all values 0): **the 8 dial icons
    composited correctly** - right shape, right position, aligned with the grid's divider ticks, a
    real structural proof that the font/icon atlases and the hand-authored grid geometry all agree
    with each other.
  - **Found a real gap while looking at the result, not by inspection alone: the header label text
    overlaps between columns** (e.g. "AMD"/"AMF" run together as "AMDAMF" with no gap). Measured why:
    the label text band is `y=3-7` (5px tall), but `build_font_atlas.py`'s font (extracted from the
    kit-rename "Enter name:" screen) is `13px` tall - **these are two different fonts at two different
    sizes**, not one font reused. The name-entry font's glyphs are simply too wide for the ~20px label
    columns here. Reusing the name-entry atlas for header labels was an unverified assumption in the
    original Phase 1/3 plan text (which only ever explicitly needed the name-entry font for kit/track/
    pattern *names*, not dial labels) - now corrected.
  - **Small label font extracted and wired in (2026-09-28) - the gap above is closed.** Wrote
    `tools/mdtrace/build_label_font_atlas.py`: sweeps `PanelEncoder::SoundSelection` (changes the
    assigned machine, confirmed in Phase 3) across 10 known steps, using a hand-transcribed
    `WORDS_BY_FRAME` (read off a 26-frame stacked strip by eye - these are short 3-4 letter real
    machine SYN-param names, e.g. `PTCH DEC RAMP HOLD`, `CLPY TONE HARD RICH`), then auto-slices each
    word into per-letter cells. Confirmed this font's pitch is exactly 4px by comparing the constant
    word "DEC" (appears in every machine, so it's a free alignment check) against "PTCH" - both start
    their letters 4px apart. Got 20 of 26 letters from the machine sweep alone; added a second capture
    of the AMP/EFX page's own fixed labels (`AMD AMF EQF EQG FLTF FLTW FLTQ SRR`) to pick up the two
    missing letters (Q, W) for free, since Phase 2 already established those labels are static.
  - **Real bug found and fixed while wiring this up**: capturing the AMP/EFX page immediately after
    the 22-step `SoundSelection` sweep (in the same mdProbe process) produced garbled, misaligned
    letters - confirmed by direct pixel comparison, not guessed. Root cause: something about that much
    encoder activity leaves the display in a subtly shifted state before the panel-switch settles.
    Fixed by capturing the AMP/EFX labels in a **separate, fresh mdProbe invocation** (still reusing
    the same flashcache, so no extra boot-time cost) rather than appending it to the sweep's action
    list. **Lesson**: when composing many panel actions in one long mdProbe invocation, don't assume
    a later capture is unaffected by unrelated earlier UI interactions - verify pixel-for-pixel against
    an isolated capture before trusting it, the same discipline as the earlier off-by-10 font bug.
  - `compose_skin.py` updated to use this new label font atlas instead of the name-entry one. Re-ran
    the AMP/EFX composite test: **all 8 labels (AMD AMF EQF EQG FLTF FLTW FLTQ SRR) now render fully
    and correctly**, matching the real capture - Phase 4's first working version is complete for this
    screen. `docs/` note: 20-of-26-letters coverage (no J/K/X/Z yet) is enough for every currently
    known screen's fixed labels; per-machine SYN names may need a longer sweep before Phase 4 can
    render arbitrary machine names with full confidence.

  **Scope note**: this is genuinely comparable in size to Monomodule's own `mk_skin.py` (~800
  lines) plus the font/icon extraction Monomodule got for free from upstream and we don't have —
  a multi-session build. All four phases now have a working first pass; the small-label-font
  extraction above is the most concrete remaining gap before the compositor is presentation-ready.

  **How the per-machine SYN labels actually become "dynamic" (2026-09-28) - answers the user's direct
  question, "Monomodule was able to achieve this, how did they do it?".** They're not dynamic at all
  in the sense of a live redraw: `mpc-vst-monomodule/vst/skin/mk_skin.py` bakes **one static overlay
  image per machine** (`image_comp("SYN grid %s" % m["displayName"], ...)`) and places all of them as
  layout components gated by MPC's own native `IndexedEnabling` skin feature -
  `additionalInvalidatingHandles: ["IndexedEnabling/<i>/<N>/Parameter <p>"]` - which the *device's own
  skin engine* uses to show exactly one of the N images based on the live value of parameter `p` (here,
  the machine-select param), confirmed working for VST2 params on real Force hardware
  (`/home/sam/mpc-vst/docs/NOTES.md`, "Conditional visibility works for VST2 params" / "Mode panels").
  Our own toolchain (`mpc-vst-plugins`) already has a layout-level shorthand for exactly this:
  `when=<param>:<option>` in `shadow_skin.py`'s layout.conf syntax, verified on a Force the same way.
  So the plan is: bake N per-machine label images (same technique as this session's font/icon work),
  and let `gen_layout.py` emit one `when=track%d_machine:<id>` component per machine per track,
  reusing our existing dial-pointer filmstrips unchanged (those are value-driven, not machine-driven,
  so Monomodule places them as ordinary components alongside the conditional label image, not
  duplicated per machine).
  - **This is tractable for 135 real Machinedrum machines (a scarier number than Monomachine's own,
    much shorter machine list) because most of them share identical label text**: deduping by the
    exact 8-label tuple across all 135 machines gives only **53 unique combinations** (e.g. 52 of the
    135 - all the ROM/sample-player machine variants - show exactly the same `PTCH DEC HOLD BRR STRT
    END RTRG RTIM`). So only 53 small PNGs need to exist; the `when=` layout entries multiply per
    (track, machine id) - up to 16 x 135 = 2160 lines - but they all point at that same small set of
    53 files, not 2160 separate images.
  - Wrote `tools/mdtrace/gen_syn_label_overlays.py` (committed) and ran it for real: parses
    `mdmachine`'s full listing (135 machines), dedupes by label tuple, renders each unique combo with
    the Phase 4 label font, and writes a `manifest.json` (`machine id -> {name, params, overlay
    filename}`). Confirmed **53 unique overlays produced**, and spot-checked machine 28 (`TRXB2`,
    already independently verified earlier this session) - its overlay reads `PTCH DEC RAMP HOLD /
    TIC NOIS DIRT DIST` correctly (only "TICK" short its K, since K isn't in the 22-letter label font
    yet - a small, known, easily-closed gap, not a structural problem).
  - **AMP/EFX dial icons wired into the real skin build (2026-09-28) - the first atlas output actually
    reaching `vst/gen_layout.py`, not just a standalone verification script.** Wrote
    `tools/mdtrace/build_knob_filmstrip.py` (expands one `dial_atlas.json`'s deduped rotation states
    into the 128-frame vertical filmstrip PNG `mpc-vst-plugins/tools/skin_assets.py`'s `knob
    strip=... frames=128` expects - confirmed the frame-stacking direction and count requirements by
    reading `skin_assets.strip_layout()` directly, not guessing) and
    `tools/mdtrace/build_amp_fx_knob_strips.py` (drives all 8 AMP/EFX dials through capture + convert,
    output named by fx key - `amd.png`, `amf.png`, etc. - so `gen_layout.py` can reference them
    directly). Ran the full pipeline for real: all 8 filmstrips built (42x36 per frame x 128 frames
    each), spot-checked `amd.png`'s frames 0/32/64/96/127 by direct pixel dump - the pointer visibly
    rotates all the way around, matching Phase 2's already-verified rotation sweep.
  - `vst/gen_layout.py` now emits `strip=build/knobs/<fxkey>.png frames=128` on the 8 AMD..SRR knobs
    (MACHINE/VOL/PAN/DIST keep the generic look for now - see next point). **Validated against the
    actual toolchain code**, not just eyeballed: imported `mpc-vst`'s own `shadow_skin.parse_layout()`
    and `skin_assets.look_of()`/`check()` directly and ran them against the generated `layout.conf` -
    all 17 tabs parse, the strip knob's `look_of()` resolves correctly, `check()` returns `None` (no
    validation error), and `skin_assets.strip_layout()` reads back exactly `(42, 4608, 128, True)` -
    128 frames, stacked down, as intended. This is real static asset generation, verified to be
    well-formed by the same code that will actually consume it - a full local `build_so.sh`/Docker
    build and an on-device deploy are the remaining verification steps, not attempted this session
    (no device access requested/available this turn).
  - **How the SYN page's per-machine labels would connect (found this session, not yet wired):**
    Monomodule doesn't dynamically redraw anything - it bakes one static label image **per machine**
    and gates each with MPC's own native `IndexedEnabling` skin feature (confirmed working for VST2
    params on real Force hardware, `/home/sam/mpc-vst/docs/NOTES.md` "Conditional visibility works for
    VST2 params"). Our own toolchain already has a layout-level shorthand for exactly this: the
    `picture` widget kind (`shadow_skin.py`'s docstring: "one image per option of the parameter...a
    when= art line per option"), also verified on device. **The blocker isn't the skin mechanism - it's
    that `cond()` requires the bound parameter to have an `options` list of >=2 real entries in
    params.json**, and `track%d_machine` currently only has `min`/`max` (a raw int range, no options
    list) - so `picture key=track%d_machine files=...` can't be wired until that's added. Any option
    *names* added there must not be real Elektron machine-name strings (`docs/FIRMWARE.md`'s
    firmware-derived-content policy covers this repo's own committed source same as it covers `.syx`/
    `.bin`) - plain placeholders (`"M0".."M191"`) are enough, since `cond()`'s matching falls back to
    the numeric index when the option string isn't a name match anyway.
  - **Not yet done - this is the real next phase of work, likely its own session**: add the placeholder
    `options` list to `track%d_machine` in `gen_params.py`; wire `gen_syn_label_overlays.py`'s output
    into `vst/gen_layout.py` as a `picture key=track%d_machine files=<192 entries from manifest.json>`
    line per track (or the expanded `when=` form directly); this SYN tab is separately blocked on
    exposing SYN1-8 params at all, which needs "touched" tracking in `engine.cpp` first (see
    `gen_params.py`'s own docstring) - a real C++ engine change, not just skin generation.
  - **Label font extended to X and K (2026-09-28), via a new source: the bottom breadcrumb.** Same
    26-frame `SoundSelection` sweep, but reading `TRX►XC►SYNT` and `ROM14►ATAK►S` at the bottom of the
    screen (same small font, confirmed by shape) rather than the header label row - a bright bar
    (white text on black), so the slicer needs its input inverted first. 24 of 26 letters now covered
    (only J, Z missing).

  **New skin design direction (2026-09-28, user-directed): one tab per track, a machine picker, then a
  2x2 layout of the Machinedrum's own SYN/AMP/FX-style sub-pages** (mirroring Monomodule's own
  SYN/AMP/FILT/EFX tab structure, adapted to the Machinedrum's actual 2 real pages - SYN and a combined
  AMP/EFX - organized into 4 logical quadrants for the UI even though the real hardware only has 2
  physical screens). Superseding the earlier flat "12 knobs in one page" `gen_layout.py` layout.

  **Machine picker: duplicating Monomodule's own design (2026-09-28, user-directed), not the generic
  layout `popup` widget.** The generic `popup` widget lays out every option as one flat, non-scrolling
  grid - fine for Monomodule's shorter machine list, unusable for the Machinedrum's 135 real machines
  (get the same page unusably tall). Real Elektron hardware itself groups machines into families for
  exactly this reason. Wrote `tools/mdtrace/build_machine_picker.py` (committed), duplicating
  Monomodule's actual `mk_skin.py` picker technique: one column per category, every row visible with no
  scrolling, a dotted separator between rows (`mpc-vst-monomodule/vst/skin/mk_skin.py` lines ~1005-1050,
  read directly, not guessed).
  - **Categories found empirically from `mdmachine`'s real listing** (grouping by the name's first 3
    characters, not guessed): `ROM(48) MID(16) E12(16) TRX(14) P-I(9) RAM(8) EFM(8) INP(6) CTR(6)` - 9
    categories, 131 real voice machines (excludes ids 0-3, the internal GND-- utility entries). First
    grouping attempt split ROM into two columns because its machines aren't contiguous by id - fixed by
    grouping on prefix globally, not by adjacency.
  - Ran it for real: generated `picker_panel.png` (1280x600, 9 column borders + headers + dotted rows)
    and `manifest.json` (each machine id -> `{name, params, rect}`). All 131 rows fit **with no
    scrolling needed** at `ROW_H=12` (Monomodule's own row height, 46px, assumed rich per-machine
    description text we don't have - dropping the blurb and shrinking to 12px is what makes "every
    category, no scroll" fit here). Visually verified at native res: category columns are clean and
    correctly grouped, row text is legible.
  - **Real gap found by looking at the actual output, not assumed**: many rows in the digit-heavy
    categories (ROM, MID, RAM) render as visually IDENTICAL text (e.g. many "ROM" rows with no visible
    suffix) because the label font is missing most digits - it only has `1` and `4` (picked up
    incidentally from earlier captures). A machine picker where several rows look the same is a real
    usability defect, not cosmetic. Also missing: `2` in the "E12" category header (cosmetic only, its
    row content happens to use no digits at all - checked, not assumed). **Fix not done yet**: extract
    the remaining digits (`0 2 3 5 6 7 8 9`) - checked one obvious opportunistic source (the tempo
    readout "125.0" visible in every AMP/EFX and SYN capture) and it's a **third, different font size**
    (8px tall, vs. 5px for labels and 13px for name-entry) - not directly reusable, would need its own
    short extraction pass the same way as the other two fonts. This is the concrete next step before
    the picker is actually usable, not merely presentable.
  - Not yet done beyond the digit gap: the on/off row BUTTON images per machine (this pass only drew
    static label text into the shared panel background, matching Monomodule's row layout, but not yet
    the per-machine toggle image pair `mk_skin.py` places over each row); wiring any of this into
    `vst/gen_layout.py` (the picker mechanism itself - `IndexedEnabling`/IndexedEnabling-driven kids,
    matching how Monomodule's own "Machine list" panel is placed - see this session's earlier "How the
    per-machine SYN labels actually become dynamic" note, same mechanism); and a real device
    build/test.

  **Tab layout confirmed with the user against a real Monomodule VST screenshot (2026-09-28).** User
  shared an actual screenshot of their Monomodule port: top-left machine picker as a black pill (logo +
  name + dropdown arrow), top-right bank/preset pickers, a 2x2 grid of sub-pages each with its own solid
  black title bar, live numeric value text under every knob, and a bottom tab strip paging between two
  2x2 screens (6 sub-pages total: SYN/AMP/FILT/EFX, then LFO1/LFO2).
  - **What carries over to the Machinedrum port, and what doesn't**: the black title bars, the picker
    pill style, and per-knob live value text all apply directly. The bank/preset pickers do NOT apply -
    Machinedrum machines have no factory-patch library like Monomachine's (just SYN1-8 raw params), so
    there's nothing to put there. The bottom tab-paging layer does NOT apply either - Monomachine needs
    it for 6 sub-pages, but the Machinedrum only has 3 confirmed real per-track pages (SYN, AMP/EFX,
    ROUTE - see the "found the third per-track page" entry above), all fitting in one 2x2 grid already.
  - **Asked the user what goes in the 4th grid quadrant** (since we only have 3 real pages): confirmed
    **leave it reserved/blank for now**, not a kit-info panel and not collapsing to a 1x3 row.
  - Built an updated mockup (`mockup_option_B_alt_titled2.png`, scratch only, not committed - it's a
    disposable visual aid, not a build asset) adding black title bars (own quick drawing, but the REAL
    build gets this for free: `shadow_skin.py`'s `frame title="..."` widget already renders a boxed
    frame with a title bar from the skin's own theme - confirmed by reading its docstring, no new work
    needed there) to the three real quadrants plus the reserved hatched 4th, under the machine-bar pill
    from the previous mockup. This is now the **confirmed target layout** for `gen_layout.py`'s next
    rewrite: top machine-bar pill (tap to open the big picker), 2x2 grid below with SYN/AMP-EFX/ROUTE
    quadrants (native `frame title=` bars) and one reserved quadrant.
  - **Confirmed the live numeric value-under-knob text is automatic**: read `shadow_skin.py`'s own
    `knob` widget code directly - every knob always gets a native `Value` label below it
    (`_value_label`), no flag needed. **Bigger finding while checking this**: knob labels
    (`label="AMD"` etc.) are ALSO always real, native, device-rendered text (`_name_label`'s own
    docstring: "genuinely proportional, device-rendered text... PARAMS[i].name", Titillium Web font) -
    not baked bitmap art at all. So the custom label-font atlas work (Phase 1/4) was never actually
    needed for AMP/EFX's or ROUTE's *fixed* labels - a generic `knob label="AMD"` already renders
    clean text for free. The label font atlas remains genuinely necessary only for what native
    `label=` can't do: the SYN page's per-machine-varying labels, and the machine picker's per-machine
    name text - both cases where the text must change with a runtime parameter value, not a
    compile-time param name.

  **Full gap inventory taken and worked through (2026-09-28), per explicit user direction to "figure
  out all gaps then proceed":**
  1. ~~DIST placement bug~~ **FIXED** - `engine.cpp`'s `kFxKeys` no longer includes `dist`; a new
     `kRouteKeys`/`kRouteRawParam` array (non-contiguous raw indices 16, 19-23) handles
     `dist/del/rev/lfos/lfod/lfom`. Compiles and runs (`md-vst-smoke`: ready 322ms, peak 5643/32767, 0
     underruns) with the corrected 17-slot-per-track layout (was implicitly 12).
  2. ~~DEL/REV/LFOS/LFOD/LFOM not exposed~~ **FIXED** - `gen_params.py`'s new `ROUTE_PARAMS`, wired
     through `engine.cpp`'s new route-key handling. 274 total params now (was fewer).
  3. ~~ROUTE's own dial-pointer icon atlas not captured~~ **FIXED** - `build_route_knob_strips.py`
     (mirrors `build_amp_fx_knob_strips.py`), using `build_dial_atlas.py`'s new `"+"`-joined
     `screen_action` support for ROUTE's double-press navigation. Ran it for real: `dist`(36 states),
     `del`(33), `rev`(36), `lfos`(7 - a real, narrow-range param like `fltw` was), `lfod`(36),
     `lfom`(33). VOL/PAN reuse the generic knob look (no captured icon needed - see gen_layout.py's own
     comment on why).
  4. ~~gen_layout.py still emitting the old flat layout~~ **FIXED** - full rewrite to the confirmed
     design: MACHINE knob at top, 2x2 grid (SYN frame-only / AMP-EFX 8 real knobs / ROUTE 8 knobs, 6
     real + vol/pan generic / one reserved frame). Validated against the real toolchain code
     (`shadow_skin.parse_layout` + `skin_assets.look_of`/`check`): 17 tabs, 224 strip knobs (14 keys x
     16 tracks), zero errors.
  5. **Live-value-text and native-label-text questions** - answered above, no work needed, both
     automatic.
  6. ~~Machine picker button wiring into layout.conf~~ **FIXED (2026-09-28), dry-run verified.**
     Turned out NOT to need either a full `mk_skin.py`-scale rewrite or a new `shadow_skin.py` widget
     kind - `shadow_skin.build()` already returns the exact `(componentDefinitions, tabs, qmap)`
     Python structures `write_skin()` would otherwise serialize; `write_skin()` itself is short enough
     to duplicate. `tools/mdtrace/build_skin_with_picker.py` calls `build()` normally, then injects the
     picker directly with `shadow_skin`'s own primitives (`ss._local`/`_placed`/`_button`/`_bounds`) -
     same technique as `mk_skin.py`, just added on top of the existing pipeline instead of replacing
     it. Per track: a machine-bar field (tap toggles `track%d_machine__open`, added to `gen_params.py`
     with `popup_of` set - confirmed by reading `wrapper/vst2_wrap.c` directly that this is handled
     generically by the wrapper itself, `popup_set()`/`popup_picked()`, no engine.cpp changes needed
     regardless of whether a real `popup` layout widget exists anywhere), one bar image per machine
     shown via `IndexedEnabling` on `track%d_machine`, the static panel shown via `IndexedEnabling` on
     the open flag, and one transparent radio-button per machine (the exact mechanism a layout
     `popup`'s own option buttons use) that sets the machine and auto-closes via `popup_picked()`.
     **Dry-run verified** with a stubbed `art_bin`/PIL (not the full Docker `html_art` renderer): ran
     the whole pipeline end to end, got a valid `TUI.json`, and confirmed TRACK 1's page carries
     exactly 264 machine-related components (131 bar images + 1 bar field + 131 option buttons + 1
     panel), bound to the correct parameter index (`Parameter 274` = `track0_machine__open`, exactly
     where it should land after 274 regular params). **This same dry run caught a real, independent
     bug**: the flat 17-key qlinks list (MACHINE + 8 FX + 8 ROUTE) exceeded MPC's own 16-per-page cap -
     `shadow_skin.build()` raised `SystemExit` on it directly, not a guess. Fixed by splitting each
     track into two Q-Link sub-pages in `gen_layout.py` (confirmed the two subpages share the same
     underlying `componentsData` list object, so the picker only needs adding once per track, not
     once per subpage). **Not yet done**: the real Docker `html_art` build and an on-device test -
     the dry run proves the JSON is well-formed and internally consistent, not that it renders/behaves
     correctly on a real Force.
  7. ~~Digit font gap~~ **FIXED (2026-09-28).** `SoundSelection` turned out to be a dead end for
     reaching arbitrary digits - confirmed by direct testing that it loops within a bounded
     ~13-machine subset regardless of jump size or direction (a real, useful correction to the
     original "compute the tap count" plan). The actual fix: the live numeric readout under a turned
     ROUTE-page knob (e.g. "20" under DIST after `encoder:DataEntryA:20`) is the *same* 5px font, and
     `DataEntryA` reliably reaches any value 0-127 (unlike `SoundSelection`). Captured `0 3 5 7 8` from
     values 30/50/70/78/83 and `2 6 9` from 92/26 - every digit cross-confirmed across multiple frames
     (e.g. "0" identical in all of 30/50/70). **All 10 digits are now in the committed
     `build_label_font_atlas.py` and its atlas output**, fully reproducible from a clean run, not a
     one-off capture. Only `J`/`Z` remain missing from the label font (24 of 26 letters, all 10
     digits) - low priority, neither has come up in any real label/machine-name text yet.
  8. ~~SYN1-8 params + "touched" tracking~~ **DONE (2026-09-28).** `track%d_syn1..8` (appended). The
     engine owns their values (the wrapper's getParameter reads eGet): a machine change resets every
     SYN knob not set since to the machine's own defaults (synchronously in eSet once the machine table
     is loaded, else by the DSP thread when it applies the machine); a value set after the change wins,
     which is exactly a project restore's order (machine has the lower VST index). Each knob's current
     label is served as `track%d_syn%d_name` for a new opt-in wrapper feature, `"dynamic_name"`
     (mpc-vst-plugins branch `claude/dynamic-param-names`: effGetParamName asks the DSP first; MPC
     re-reads names on UpdateDisplay per docs/NOTES.md). `md-vst-smoke` checks all four cases.
  9. **Real build - DONE and verified (2026-09-28); on-device test still pending.** Ran the actual
     build pipeline for the first time this session, both halves:
     - `vst/build_so.sh /tmp/recomp_inc /home/sam/mpc-vst` against the real `md-armhf-builder` Docker
       image: produced a real ARM EABI5 `machinedrum_one.so` (6.3MB, stripped) - this session's
       `engine.cpp`/`gen_params.py` changes compiled and linked for real, not just x86.
     - `build_skin_with_picker.py` against the real `mpc-vst-html-art` Docker image (not the earlier
       dry run's stubbed `art_bin`/PIL): produced a complete skin package - `TUI.json` (314 component
       defs, 33 tab pages), all 14 custom knob filmstrips correctly copied in as
       `sh_knob_r26_<hash>.png` (confirmed by grepping TUI.json - one distinct hash per fx/route key,
       matching our 14 captures exactly), all 131 machine bar images, the picker panel, and - after
       this run surfaced they were missing - `version.xml` + `Q-Links.json`/`Q-Links - 8by1.json`,
       which `build_skin_with_picker.py` hadn't been writing at all (only `write_skin()`'s `TUI.json`
       half was ported over initially; fixed to also emit the rest matching `write_skin()` exactly).
     - **Deployed to the real Force (2026-09-28)**, via `mpc-vst`'s own `tools/release.py` +
       generated `install.sh` (safe, tested installer: backs up `MPC.settings`, validates XML,
       restarts MPC with an error trap) rather than hand-rolled scp/settings edits. md5-verified
       identical to the local build; `MPC.settings` gained exactly one entry; MPC restarted cleanly
       (new PID, device uptime unaffected - a service restart, not a reboot). This is an upgrade of an
       already-once-deployed earlier build (the device already had the user's `.syx` at
       `/sdcard/vst/machinedrum/` and a prior `machinedrum_one.so` from an earlier session). **Not yet
       done**: actually loading the plugin onto a track and playing/exercising it in MPC's own UI -
       the install put the new build in place, but exercising it interactively is the user's own
       next step at the device.
  10. ~~LEV / J-Z~~ **RESOLVED (2026-09-28) by the skin redo below.** LEV is the kit's per-track
      level *setting* (`HostModel::setLevel`, default 100), not a live meter: now a real param,
      `track%d_level`, drawn as Monomodule's own LEV column. J/Z: moot - the skin now draws with
      Monomodule's art.json fonts (the Monomachine OS's own Elektron LCD fonts), which have them.

- **2026-09-28: skin redone as a port of mpc-vst-monomodule's own `vst/skin/mk_skin.py` (user
  direction, after the deployed layout.conf skin came out nowhere near the proposed designs: raw
  white LCD crops as knobs, no visible picker bar, a MACHINE knob clipped under the header, empty SYN).**
  `tools/mdskin/mk_skin.py` is that file (at monomodule 56cb8e0, its "2x2" layout) with its drawing
  code kept: same LCD look, knob cells (filmstrip + invisible touch knob), machine bar, full-width
  picker, LEV column. Changes are only what the MD needs - 16 tabs (one per track, each Monomodule's
  2x2: SYN | AMP/EFX over ROUTE | TRACK n with LEV), machines from `mdmachine` (now a CMake target;
  135 incl. the 4 GND machines, so a new track's GND-- has a bar to tap), machine param = raw id so
  IndexedEnabling/button ids use n=192, ROM split into 16-row picker columns (12 columns, no
  scrolling), SYN dials once per track with a per-machine label overlay over them (transparent over
  live cells, opaque over unused ones) instead of per-machine dials (131x16 would be ~33k components;
  this is 459 per tab). No preset strip (no presets) and "MD" in place of the Shnolk logo.
  Build: `tools/mdskin/build_skin.sh <MD OS.syx> <monomodule vst/build/art.json> [mpc-vst checkout]`
  (writes vst/build/skin + preview PNGs). `vst.json` is now `custom_skin`; the layout.conf path
  (gen_layout.py, build_skin_with_picker.py, build_machine_bar/picker.py) is removed.
  Previewed (track, picker open, a 3-param machine, a fresh GND track): all match Monomodule's look.
  Release 0.2.0 packaged (`/tmp/md_release`); **not deployed yet** - waiting on the user's go-ahead.

  **Phase 1 progress (2026-09-28): rename mechanism confirmed empirically, unblocks capture.**
  - No sysex/data API sets a kit/pattern/track name (checked `mdautomation.cpp`/`mdsysexautomation.cpp`/
    `mdrom.cpp`/`mdromdata.cpp`/`mdflash.cpp`/`mdsim.cpp` — none). Confirmed real hardware behavior:
    names are only editable through the front panel's own character-picker screen.
  - Added an `encoder:NAME[:STEPS]` action to `tools/mdtrace/mdProbe.cpp` (mirrors
    `mdEditor.cpp`'s `Editor::emitEncoderSteps`: `panelEncoderCommand()` + `sendPanelEvent(cmd, 0x01/0xff)`
    per detent) since `mdpanel.h`'s `PanelEncoder` (DataEntryA-H/Level/SoundSelection) looked like the
    likely input for a character picker. **It isn't** — rotating any `DataEntryA` steps had no effect on
    the name-entry screen in testing.
  - What actually works, found by probing live (`mdProbe` + `lcdpng`, converted to PNG and inspected):
    `panel:Kit` → popup with LOAD/SAVE/EDIT/MASTER quadrants → `panel:Right` selects SAVE →
    `panel:Enter` → save-slot list (`1-USERKIT`, `2-EFR UW`, ...) → `panel:Enter` on a slot → an
    "Enter name:" screen with the name on one line and a `«  »` cursor indicator below the selected
    character. From there: **`panel:Up`/`panel:Down` cycles the character at the cursor** (steps by
    ASCII-ish order; a 2048-frame hold auto-repeats ~2 steps — use a short `hold` argument, e.g.
    `panel:Up:128`, for single-character control), **`panel:Left`/`panel:Right` moves the cursor**
    between name positions, `panel:Enter` confirms/saves. Copy of the exact working action sequence
    (from a fresh boot, ROM path is the user's own `elektron_sps1-1uw_os1.63.bin`, not committed):
    `panel:Kit panel:Right panel:Enter panel:Enter panel:Right:128 panel:Up:128 ... panel:Enter`.
  - **Single-step confirmed directly (2026-09-28), correcting an initial misread**: a sequence of six
    `panel:Up:64` calls in one boot, each followed by an `lcdpng` capture, was inspected at 6x
    upscale (small thumbnails were ambiguous - don't trust them for glyph work, always upscale before
    reading) and stepped exactly one character per tap: `T -> U -> V -> ...` Each `panel:Up`/`panel:Down`
    with `hold=64` is a clean, reliable single-character step; no auto-repeat contamination at that
    hold length. This means a sweep script can walk the full character set deterministically by
    counting taps from a known start character - no need to guess distances or verify by trial capture
    each time.
  - **Full charset order walked and captured (2026-09-28)**: 45 sequential `panel:Up:64` taps from
    a start char of "T" were each captured with `lcdpng`, cropped to the name-entry line, and
    stacked into one strip image for direct reading (a much better technique than one-off captures -
    reuse this "stack N frames into a vertical strip, read once" trick for any future sweep, it beats
    inspecting frames one at a time). Confirmed order, continuing forward from T:
    `T U V W X Y Z [2 accented/special glyphs, unconfirmed exact chars - look like "A-ring" and
    a second variant] Ø <space> 0 1 2 3 4 5 6 7 8 9 + - = [Ø again, or a lookalike - verify] /
    ( ) , ! ? A B C D E F G H I J K L M N O` (then presumably continues P Q R S T, wrapping). So the
    cycle is essentially: **A-Z, a couple of accented/special characters, Ø, space, 0-9, a small
    symbol set (`+ - = Ø / ( ) , ! ?`), then wraps to A** - i.e. everything needed for the skin
    (A-Z, 0-9, space, `: . - ►` etc. seen in real captures) is reachable, though the exact symbol
    for `:` and `►` wasn't hit in this 45-tap window and needs a longer walk or starting from a
    different point to confirm their position/glyph shape.
  - **Font atlas built and verified (2026-09-28) - Phase 1's core deliverable is done.** Wrote
    `tools/mdtrace/build_font_atlas.py` (committed - it's code, not firmware-derived data). It drives
    the full corrected 50-entry cycle from a fixed start point (steps to `SPACE` first via the known
    offset from the default "T", then walks forward capturing all 50), and **autocrops the glyph cell
    by pixel-diffing across all 50 frames** (the cell that changes frame-to-frame *is* the glyph, no
    manual pitch measurement needed - this technique generalizes to Phase 2's icon sweep too). Output:
    `font_atlas.json` (`glyph_w`/`glyph_h`/`band_x`/`band_y` + a `glyphs` dict keyed by name, each a
    list of `"01..."` bit-row strings) plus a `font_atlas_grid.png` for visual sanity-checking - both
    build output, gitignored (`build*/` already covers it; ran the script into a scratch dir this
    session, not `vst/build/`, so nothing new needed there).
  - **Found and fixed a real off-by-10 labeling bug the same session it was introduced.** The first
    working version's preloop (to walk from the default kit name's first char "T" back to `SPACE`
    before starting the sweep) tapped `Up` `CYCLE.index("T")` (= 40) times instead of the needed
    `(0 - 40) % 50 = 10` times, landing 30 slots past `SPACE` - so every captured glyph was stored
    under a label 10 slots away from its true character (e.g. the bitmap stored as `"A"` was actually
    `PLUS`'s glyph). **This was invisible in an eyeballed grid image** - relabeling-then-redisplaying
    glyphs by their (wrong) stored key still produces a self-consistent-looking, alphabetically-ordered
    grid, because the display script trusted the same wrong labels. It only surfaced by dumping a few
    specific letters' raw bitmaps as ASCII (`A` looked like a plus sign, not a letter A) and checking
    their *shape* against what that letter should actually look like. **Lesson for any future
    label-from-position capture work**: verify a labeled asset against an independent ground truth of
    what the label should look like, not just against a re-rendering of itself. Fixed: preloop now taps
    `(-CYCLE.index("T")) % 50` times, and the per-frame label lookup no longer double-counts the
    preloop offset. Re-ran and re-verified: `T`, `U`, `I`, `O`, `S`, `L` all now show correct,
    recognizable letterforms; the natural capture-order grid (`font_atlas_grid.png`) also reads
    correctly as `0-9, + - = Ø / ( ) , ! ?, A-Z, Å Ä Ø` without any relabeling.
  - Corrected an earlier misreading in this same log: the cycle is exactly 50 entries, not 51 - what
    an earlier partial 45-tap walk logged as a trailing "PERIOD" was actually just the wrap back to
    `SPACE` (which looks blank, hence the confusion). `CYCLE` in `build_font_atlas.py` is now the
    checked-good ordered list.
  - **The font is proportional, not fixed-pitch** - this corrects the original Phase 1 plan text's
    assumption ("The font is fixed-pitch... compare letter spacing to get the exact cell pitch").
    Swept the cursor across 8 name positions (`panel:Right:64` x8) with the fixed suffix "RX UW"
    showing and diffed consecutive frames: the selection box's position deltas were `8,6,6,5,6,9,5`
    px - not constant, meaning each glyph's natural width varies, not a fixed grid.
  - **Per-glyph `advance` width now measured reliably.** The first attempt's `ink_x0`/`ink_x1`/
    `advance` were contaminated by the cursor box's own dashed corner ticks (present in every glyph's
    crop, touching column 0 and the right edge) and reported every glyph as full-width. Fix: intersect
    all 50 captured glyph bitmaps to find pixels that are ink in literally every one - this isolated
    the ticks to rows 0-2 and 11-12 of the 13-row crop band, with the actual character body confined
    to rows 3-10 (`GLYPH_BODY_ROWS`). Restricting ink-column trimming to that row range gives sane,
    varied results: `SPACE` advance 2px, most letters 6-7px, none pinned at the full 8px band width
    anymore. This is real, physically-measured per-glyph spacing, ready for Phase 3/4 text layout.

## Relationship between the projects

Monomodule (Shnolk) and gearmulator-md-mm (Joe Landers) share no code and neither credits the other. md-mm is
full-system emulation (Musashi ColdFire + two DSP56303s, 8 MB flash image); Monomodule runs one DSP from the
public OS file with a hand-written host model. Their dsp56300 fixes don't overlap: Monomodule added SR.SM
saturation, MPYRI and PFLUSH; md-mm added MERGE and DMA/ESSI fixes. The MD engine may need both sets; check when
the voice program first runs.

## Firmware handling

The user owns the hardware and has supplied the OS `.syx` and a full flash `.bin` (MD OS 1.63). They're kept
outside the repo. `.gitignore` blocks `*.syx`, `*.bin` and `*.inl`; never commit firmware or anything derived
from it.

- **2026-09-28: CPU glitching with a 4-track kit, a GLOBAL tab, master FX parked.**
  - Measured on the Force (per-thread /proc sampling while the user's pattern played): the engine
    thread at 89% of its core with EFM BD / EFM XT / E12 SD / P-I MT, at FIFO 30 above MPC's
    AudioWorker on the same core, so overload glitched all of MPC. **The Force has 4 cores**
    (/proc/cpuinfo; MPC's threads run on cpu 0-3) - `nproc` = 2 only reflects the ssh shell's
    affinity, correcting the "2 cores" note above.
  - Stage timing (x86, recompiled DSP): most of the cost is fixed per block, not per voice - all
    silent 41 us/block vs 64 with the kit; per-track FX on 16 tracks was ~15 us of it. Fix: a
    settled silent track's FX is skipped (exact: verified identical over 60000 blocks against the
    unskipped path), -16..18% on the kit. Engine thread now FIFO 5 (below MPC audio RR 20 and
    MIDI out RR 10), named `md-engine`. Deployed as 0.2.1. **Not yet re-measured on the device.**
    Next lever if still short: voices on a second core via a persistent thread pair.
  - **Master FX (reverb/delay/etc.) parked (user direction):** the engine computes the REV/DEL send
    mixes but no master effect consumes them, so REV/DEL do nothing yet.
  - GLOBAL tab (the Monomodule-style skin had dropped it): MIXER (16 LEV bars) + VOICES; tempo now
    follows MPC's (vst.json HAS_LFO_BPM -> "lfo_bpm"; the tempo param is only a fallback).
  - Force "drum pads" investigation: DrumSynth Multi's 16-pad layout isn't a stored flag (diffed a
    saved project: same program type 3, same padNoteMap; only per-pad colours differ) or the
    settings category (tried category="Drum": no effect, reverted). "Drum test 2" (DrumSynth's pad
    colours on the Machinedrum track) awaits the user's check.

- **2026-09-28: per-track LFO page; CPU fix confirmed on the device; drum-pad layout ruled out.**
  - Measured on the Force with the user's 4-track kit after 0.2.1/0.2.2: `md-engine` at **40.6%** of
    its core (was 89%), load average ~1.0, no more glitching reported.
  - "Drum test 2" (DrumSynth's per-pad colours on the Machinedrum track): still chromatic. The 16-pad
    drum layout is tied to DrumSynth's internal plugin identity - not reachable from a VST. Closed.
  - LFO page, captured from the real MD in the emulator (reached with FUNCTION + SYN/EFX/ROUTE; new
    mdProbe `combo:HELD+TAPPED` action): TRACK PARAM SHP1 SHP2 / UPDTE SPEED DEPTH SHMIX. TRACK is
    one of 16 named tracks (01-BD .. 16-M4), PARAM the destination's 24 params named by its machine
    (0-7) or the fixed AMD..LFOM, 6 shapes (tri, saw, square, ramp, exp, random), UPDTE FREE/TRIG/HOLD;
    SPEED/DEPTH/SHMIX are LFOS/LFOD/LFOM. New params per track: lfo_track/param/shp1/shp2/type
    (appended) -> HostModel::setLfo. PARAM's text is the destination's live label via a new opt-in
    wrapper feature, "dynamic_display" (mpc-vst-plugins claude/dynamic-param-names: effGetParamDisplay
    asks the DSP for "<key>_display"), drawn by MPC's native value label. The LFO page replaces the
    per-track LEV column (LEV stays on the GLOBAL mixer and the track's Q-Links).

- **2026-09-28: kits, randomise, GLOBAL first, picker trimmed; renamed Machinedrum Module.**
  - Renamed to **Machinedrum Module** (UID `MdOn` and `machinedrum_one.so` kept, so saved projects still load).
    Skin: one big LCD (colours sampled from a photo of the real one: 5e0c0c on ff4836) inside a thin
    glossy bezel on a brushed faceplate. GLOBAL is the first tab (MIXER, GLOBAL, KITS pages).
  - Machine picker: MID/CTR (no audio), INP (no audio input) and RAM (no sampling) are no longer
    offered; their bars remain for kits that use them. **ROM machines are silent in this port** (the
    sample flash isn't loaded - smoke test peak 0 for ROM01): open question for the user, since most
    factory kits put ROM machines on tracks 13-16.
  - **Kits**: the MD's own kit sysex ($52, 1233 bytes). Layout (verified against the emulated MD's own
    dumps): name at $0a (16 bytes, $7F first = empty slot), 16x24 params at $1a (HostModel raw
    order), 16 levels at $19a, 7-bit-encoded machines at $1aa (74 bytes -> 16 big-endian words, id =
    low byte), 7-bit-encoded LFOs at $1f4 (664 bytes -> 16x36: dest track, dest param, shp1, shp2,
    update, ...). Factory kits: `tools/mdkits/make_factory.py` boots the emulated MD from the user's
    flash image, sends the MD's kit request ($53) for all 64 slots (new mdProbe `kitdump:DIR`) and
    keeps the 16 named ones (TRX UW .. SEACLONES) as FACTORY.syx - per-user build output, installed to
    <data>/factory (release.py --extra). User packs: any .syx in <data>/kits or
    "Force Documents/Machinedrum Kits", rescanned every ~3 s (Monomodule's scan design); BANK = file.
    KIT prev/next loads all 16 tracks (machine, 24 params, LEV, LFO) with SYN "touched" so the kit's
    values win; the name shows " *" once edited. Master FX in kits are ignored (parked).
  - Randomise (GLOBAL toggles, Monomodule's momentary hold-1.5 s design): machines on all / 1-8 / 9-16
    tracks from TRX/EFM/E12/P-I (SYN reset to the new machine's defaults), and RND KIT = a random kit
    from the current bank.

- **2026-09-28: ROM machines play.** The UW's ROM slots play samples the MD copies from its sample flash
  into the voice DSP (DSP2) at boot - absent from the OS file, so they were silent here. Found by dumping
  the emulated MD's DSP2 P memory after boot (new mdProbe `dspdump:DSP:FROM:TO:FILE`) and diffing it
  against VoiceEngine's own: the sample data at $150000-$18fc12 (260K words) plus a **sample directory
  at $147e00** (4 words per slot 0-47: start, length, loop, flags) that the boot writes over the
  program's own defaults - both needed (data alone stays silent). `tools/mdkits/mdsamples` extracts
  every differing word from $140000 up (below that it's runtime state) as ROM_SAMPLES.bin records;
  `make_factory.py` now makes it alongside FACTORY.syx from one emulator boot. The engine loads it
  (VoiceEngine::writeP after construction - reset() reloads the program's own records, the directory
  among them) and randomise includes ROM slots whose directory length is non-zero. The factory image
  fills ROM01-32; ROM33-48 are empty (silent there too). Not yet checked: bit-exactness of ROM output
  against the emulated MD.

- **2026-09-29: CPU work on the Force, ROM machines fixed, GLOBAL fixes. Machinedrum Module 0.4.x, deployed to the device.**
  - **Bank/kit steppers did nothing**: on the GLOBAL tab the kit/bank components were listed *before* the full-page
    Background image, so the opaque background covered them and took their clicks (later components draw and take
    clicks on top). Fixed in `mk_skin.py` (Background first). Track pages already had it the right way round.
  - **ROM machines were silent-ish/identical/glitchy - two real bugs.** (1) The ROM machines' coefficient function
    divides by a word at internal SRAM `$0100150c` that the OS boot fills (`$bb8`; `$0100150x` = `06060606 x3 00000bb8`).
    `MachineRunner` only had the OS image, so it took a divide-by-zero, returned -1 and `HostModel` sent an all-zero
    slot: every ROM track played the same glitchy sound. Now seeded in `MachineRunner`'s constructor (16 bytes). (2) The
    recompiler's discovery pass never ran the ROM playback code (zero words, no samples), so on the Force ROM ran on the
    slow interpreter. `mdrecomp_discover` now takes `ROM_SAMPLES.bin` as a 3rd argument: 840 -> 1081 blocks.
  - **Whole-MPC freeze diagnosed**: an overloaded engine thread (SCHED_FIFO 5) never sleeps, so it kept its core 100% busy
    and starved MPC's normal-priority threads on it; the MPC main thread sat in `rt_mutex_schedule` (a lock held by a
    starved thread). Proof: demoting the thread to SCHED_OTHER (python `os.sched_setscheduler`) woke MPC at once.
    Fix: a duty cap (`kMaxDuty` 0.7 of its core's CPU time over ~30 ms windows, `MD_DUTY` overrides for tests): overload
    now costs the plugin dropped blocks (crackle, counted as underruns, `naps` in the stats), never MPC.
  - **Where the time goes on the Force** (per 128-frame host block, budget 2902 us; `md-cost` on the device): idle was
    774-1088 us; OS tick ~190 us (the OS's own smoothing/LFO routines over all 16 tracks - not cuttable without changing
    behaviour), voice DSP 444 us, mixer 130 us, track FX 19 us. One playing voice adds **ROM ~430 us, other machines
    ~240 us** (x86 DSP-instruction ratio: ROM 3,500 instr vs TRX-BD ~1,830). Two exact savings, both verified with
    `md-hash` (identical audio hash `34a5ad68b72a7282` on x86, ARM recompiled and ARM plain interpreter):
    (a) the mixer skips tracks whose input block is all zeros (`engine/Mixer.cpp`; `MD_MIX_NOSKIP` runs everything);
    (b) the voice DSP loop sends a 1-word flag per voice and the 32 samples only for voices that rendered
    (`VoiceEngine::installHarness` kSkipStub/kSkipNormal/kSkipSilent, `readBlock`); idle DSP 1987 -> 659 instr/block.
    Idle on the Force is now ~290 us, voice DSP 87, mixer 8.
  - **Voice budget** (`max_voices`, the VOICES knob, same key/param): now a cost budget in units, most machines 1, ROM
    machines 2 (`HostModel::voiceCost`), oldest sounding tracks cut when a trigger would exceed it. **Default 8** (the
    user's choice for testing). Measured on the Force with the standalone engine (ring depth 2, tracks retriggering every
    125 ms): 4 non-ROM tracks 1.6 ms/block, 0 underruns; 6 or 8 non-ROM tracks 3.8 ms, 700-1500 underruns; 2 ROM fine; 4 ROM
    (8 units) 92 underruns. **Recommend 5 as the shipped default** (4 for dense patterns). A saved project keeps its own
    saved VOICES value (the user's old one was probably 16 = uncapped, which left the engine in permanent overload).
  - **ROM on/off switch** (GLOBAL tab, param `rom_enabled`, appended last): off = tracks on a ROM machine play the empty
    machine (engine.cpp swaps it in per track, `appliedEff`; the track keeps its ROM setting and its SYN values), the
    randomiser leaves ROM out. Kits do NOT carry ROM samples: a kit `.syx` holds machine numbers only, samples come from the
    flash image at build time (`ROM_SAMPLES.bin`, ROM01-32 filled, 33-48 empty), so someone else's ROM kit plays *your*
    samples in those slots.
  - **A fresh instance loads the first kit** (`Inst::maybeDefaultKit`, ~1.5 s after create, only if every track is still on
    GND--, so a restored project is never touched). x86 smoke: `MD_SMOKE_FRESH=1` (and `MD_SMOKE_RESTORED=1`).
  - **Memory**: MPC grew ~590 MB with the plugin. The DSP interpreter's per-program-word opcode cache made P memory of 8M
    words cost ~400 MB; P is now 2M words (`kSizeP`, the MD map stays below `$200000`, samples end `$18fc12`), output
    identical, ~225 MB less in MPC. The skin no longer draws bars/grids for machines that cannot sound here (MID, CTR, INP,
    RAM, ROM33-48): 437 -> 317 components per track page. Tab switching is still slower on first load, then fine.
  - **Ring depth**: 4 blocks (kRing 4, kAhead 2 = 5.8 ms) as originally (latency matters for feel). A deeper ring
    (kAhead 4, kRing 8, +5.8 ms) rode out CPU spikes in the x86 churn test (158 underruns at 2, 0 at 3+): the fallback if
    crackle returns on a heavy kit. The plugin does not report its latency to MPC (not verified).
  - **Tried and dropped: release a voice after 1.5 s of exact silence.** It works and frees CPU, but a retrigger from the
    released state is NOT the same as the original: 72 of 107 sounding machines (all ROM, E12, P-I, most EFM, some TRX)
    respond differently (SNR 11-20 dB in a random pattern). Reverted. Do not retry without a way to keep the machine's
    state exact.
  - **Tools added**: `md-cost` (`tools/mdcost`: idle stage breakdown + DSP instr and wall time per playing voice for every
    machine, run on the Force), `md-hash` (`tools/mdhash`: audio hash of a fixed 16-track pattern; equal hashes = identical
    audio; compare recompiled vs interpreter, ARM vs x86, skip paths on/off - it does NOT load ROM samples, so ROM audio
    is not covered), `md-vst-smoke` env: `MD_SMOKE_CHURN`, `MD_SMOKE_VOICES`, `MD_SMOKE_ROMOFF`, `MD_SMOKE_FX`, `MD_SMOKE_KIT`,
    `MD_SMOKE_FRESH`. Engine stats: `touch /tmp/md-stats-on` on the device (or `MD_STATS=1`), then read
    `/tmp/md-stats.<MPC pid>` (underruns, naps, worst/mean render us per second). `/tmp` is cleared by every reboot.
  - README rewritten as the user-facing pre-release page (features, limits, build, credits).

- **2026-09-29 (later): what actually limits polyphony on the Force, and two things tried and dropped.**
  - **Live stage breakdown** (engine stats now print `active= rom= budget= tick= dsp= fx= mix=`, us per 128-frame host block,
    with `touch /tmp/md-stats-on`): a regular drum pattern with 5-6 voices sounding sat at ~1.85 ms/block (dsp ~900, fx ~600-715,
    tick ~195, mix ~30) with worst blocks 2.4-2.8 ms - i.e. **already at the edge of the 2.9 ms block**. The 7th sounding voice
    (track 7, EFM RS) added ~780 us of DSP (dsp 900 -> 1680, later up to 2500) and tipped it into ~80 underruns/s. **Track FX
    costs ~120 us per playing track** (fx ~730 us with 6 tracks) - the second-largest cost after the voice DSP.
  - Per-voice DSP cost is roughly constant while a machine is sounding (~1,900-3,600 DSP instr per 32-sample block; no trigger
    spike: measured with a retrigger every 125 ms) and drops when it has decayed. On the Force that is ~340 us/voice on a quiet
    device and 2-3x that when MPC is busy (`md-cost` run while the plugin was playing: EFM RS 830 us, P-ISD 1,380 us, ROM
    ~1,500 us). So capacity is ~4-5 sounding voices, whatever the machine mix; no fixed voice count is right.
  - **Tried and dropped: overload governor** (cut one voice, decayed first, when the smoothed render time passed 2.1 ms). It
    fired ~4 times a second, the pattern retriggered the cut tracks at once so load stayed high, and the cuts themselves were
    the glitching - worse than before. Removed. **Also dropped: releasing a voice after 1.5 s of silence** (not exact: 72 of
    107 machines sound different when retriggered from the released state, SNR 11-20 dB).
  - The engine/HostModel keep the exact savings (mixer skip, DSP idle-voice flag), the voice budget, the duty cap, the ROM
    switch and the stats.
  - **What would actually raise polyphony (not done):** (1) split the 16 voice slots over two `VoiceEngine` instances on two
    persistent threads (Force has 4 cores; per 32-sample block sync; never per-block thread creation - that rebooted the
    device once), (2) move track FX + mixer to a pipelined thread (exact, +0.7 ms latency at 32-sample granularity),
    (3) speed up `TrackFx` (native C++ on 32-bit ARM with 64-bit accumulators, ~120 us/track; NEON or skipping neutral stages),
    (4) a cheaper OS tick (~190 us, OS routines, not cuttable exactly). Until then: VOICES ~5 and prefer light machines.

- **2026-09-29 (end of day): v0.1.0 release flow.** `release/build_release.sh <OS.syx> <flash.bin> [-v ver] [-d ip] [-m plugins]`
  is the one-command build (mirrors Monomodule's `release/release.sh`): x86 tools -> factory kits + ROM samples -> recompiled DSP
  (discovery is now a CMake option, `-DMD_DISCOVERY=ON`, in `build-release/`) -> **bit-exactness gate** (x86 interpreter vs
  recompiled `md-hash`, with the ROM samples; aborts on mismatch) -> skin -> ARM plugin -> installer zip in `dist/`; `-d` copies
  it to the Force and runs `install.sh -y`. First full run built `dist/Machinedrum-Module-0.1.0-mpc-armv7.zip` (2.2 MB, 339
  files). Default VOICES is now **5** (measured safe; 8 crackles). LCD background now sits `lcd_margin` (28 px) outside the page
  panels (`mk_skin.py`: `ALU_H` derived from `OX + PAGES_X0`). Dependency to resolve before anyone else can build: the plugin
  needs the wrapper's `dynamic_name`/`dynamic_display`, on the unmerged mpc-vst-plugins branch `claude/dynamic-param-names`; the
  script refuses a wrapper without it. `md-hash` takes the ROM samples as a 2nd argument.

- **2026-09-29 (evening): catalog conformance, pushes.** mpc-vst-plugins now has catalog rules (`docs/CATALOG*.md`, `PORTING.md`
  section 5, CLAUDE.md): releases must carry `mpc-plugin.json` (`release.py --id --repo --license --requires`) and pass
  `tools/catalog_check.py <zip> --catalog --expect-id --expect-repo`; engines must locate data next to the `.so`
  (`MODULE_SUBDIR`, `wrapper/plugin_dir.h`), never a fixed `/sdcard`. Done here: `vst.json` defines `MODULE_SUBDIR "machinedrum"`
  (MODULE_DIR kept as the fallback), `build_release.sh` passes `--id machinedrum-module --repo sd88me/mpc-vst-machinedrum --license
  AGPL-3.0-only --requires ...` and runs `catalog_check` as its last step. **Policy conflict, needs the user/maintainer:** the catalog
  only lists zips with no closed binaries or copyrighted ROMs; ours contains firmware-derived code and data (recompiled DSP, ROM
  samples, the OS file) so it must never be a public GitHub release and cannot be listed as a download (same for Monomodule).
  No registry PR was made; a "build it yourself" catalog entry type would need to be proposed. The wrapper dependency is resolved:
  `claude/dynamic-param-names` is already merged into mpc-vst-plugins `main` (the build script just needs a checkout at `main`).
  Optional `tested.json` (`[{version, device, firmware, date}]`) not added: the Force's MPC OS version is not recorded here.

## RESUME HERE (2026-09-29)

State:
- Branch `claude/trusting-maxwell-hx3i7b` (pushed to origin; the `v0.1.0` tag is on it). Device (Force at 192.168.1.44, DHCP - ask if it
  changes) runs this build: `machinedrum_one.so` + skin + factory kits + ROM samples in `/sdcard/vst/machinedrum/`.
- The link to the Force is flaky: big `scp` and plain tar copies stall for minutes. What works: `gzip -c file | ssh
  root@.. 'gunzip -c > /sdcard/vst/x.new'`, then check `md5sum`, then `mv`; skin via `tar -czf - | ssh 'tar -xzf -'`
  (5 s). Device busybox has no `chrt`/`timeout`/`join`; use python3 there. Never `pkill -f <pattern in your own command>`.
- Build steps (all outputs are per-user, never committed):
  1. discovery: build `tools/mdrecomp/mdrecomp_discover.cpp` against `libs/dsp56300` with
     `-DDSP56K_RECOMP_DISCOVERY -DDSP56K_NO_JIT_RUNTIME` (a scratch CMake project: dsp56300 asmjit/dsp56kBase/dsp56kEmu/
     vtuneSdk + `libs/gearmulator-md-mm/source/mc68k` + the engine sources), run
     `mdrecomp-discover <OS.syx> disc.txt <ROM_SAMPLES.bin>`, `nm -C` it, then
     `python3 libs/dsp56300/tools/arm32jit_prototype/recomp/recomp_gen2.py disc.txt nm.txt > <dir>/dsp56k_recomp.inl`.
     **Redo this whenever `VoiceEngine::installHarness` or the ROM handling changes** (blocks are keyed on P words; a
     stale `.inl` falls back to the slow interpreter for changed code). Current: 1081 blocks, 97.9% coverage.
  2. `python3 tools/mdkits/make_factory.py <mdProbe> build-vst-x86/mdsamples <flash.bin> <OS.syx> vst/build/factory`
  3. `tools/mdskin/build_skin.sh <OS.syx> <monomodule art.json> ../mpc-vst`
  4. `vst/build_so.sh <dir with the .inl> ../mpc-vst`; `tools/mdskin` needs Docker `mpc-vst-html-art`.
  5. `python3 ../mpc-vst/tools/release.py --so vst/build/machinedrum_one.so --skin "vst/build/skin/sd88me - VST - Machinedrum
     Module" --entry vst/build/pluginlist-entry.xml --version 0.4.x --extra vst/build/factory:vst/machinedrum/factory`
- Verify before a release: x86 `md-hash` = `34a5ad68b72a7282`; on the Force the recompiled `md-hash` and an interpreter-only
  build (`cmake` without `-DDSP56K_RECOMP`) give the same value; `md-cost` idle ~290 us.

Open, in the order I would take them:
1. **Decide the shipped VOICES default** (currently 8; measured: 5 is safe, 8 is not; ROM counts double) and whether to build
   the two-thread split above - the user's patterns hit the ~5-voice ceiling with ordinary EFM/E12/TRX kits.
2. **OS tick ~190 us per block** is now the largest fixed cost: the OS's smoothing/LFO routines run over all 16 tracks.
   Not cuttable exactly; a persistent-thread pool for a second core (Force has 4; the earlier per-block thread spawn
   rebooted the device) is the real lever for polyphony.
3. Bank/kit **picker list** (like the machine picker; rows via `dynamic_display`), parked for the next version.
4. **Master FX** (reverb/delay/etc.): `MixerRef::runMaster` (tools/mdmix) runs `$342->$971`; decision pending between
   emulating that section and a native translation; REV/DEL currently do nothing.
5. ROM output is not verified bit-exact against the emulated MD; the ARM recompiled ROM path is covered by no hash.
6. `libs/gearmulator-md-mm` has local mdtrace patches (uncommitted in the submodule); `mdProbe` needs them (tools/mdtrace/README.md).

- **2026-09-29 (v0.2 work, branch claude/v0.2-roadmap): voice split over persistent threads.**
  - `ParallelVoiceEngine` now keeps persistent worker threads (condvar wake per block) instead of spawning per block;
    ROM samples are written to every group; `tuneWorkers()` sets name/FIFO 5/core on each worker after boot.
    The plugin uses `ParallelEngine` with `kDefaultGroups = 2` (`MD_GROUPS=1..4` overrides; 1 = the old single thread).
  - Exact: `md-hash` (with ROM) = `15746c0610a40ddb` for 1, 2 and 4 groups on x86, and on the Force (ARM recompiled) for 1-3.
  - Force, offline `md-hash` (12 s, dense 16-track pattern, MPC running): 1 group 42 s, 2 groups 30 s, 3 groups 30 s.
    So 2 groups is about 1.4x; a third adds nothing (voice `v % n` assignment is static, and MPC uses the other cores).
    Memory: each group is its own DSP2 instance (its own P memory). **Not yet run inside MPC** (needs an MPC restart).
  - **Track effects on the group threads** (Engine::fxTrack, one TrackFx per group; tracks run on their voice's group thread right
    after its voices render, no extra latency). Still bit-exact (same md-hash for 1/2/4 groups). On the Force, E12 kit, ROM on,
    5 voices: 1 group ~3.5 ms/block and ~65 underruns/s; 2 groups with serial FX ~2.5 ms, ~35/s; 2 groups with FX split
    **1.9 ms mean, worst ~5 ms, underruns flat (no new ones)**. A/B on the device: `echo N > /tmp/md-groups`, re-insert the plugin.
  - **Ring lead default 3 blocks (8.7 ms)**, user-confirmed on the Force: block gaps reach 7-8.5 ms, 2 blocks (5.8 ms) glitched.
    `/tmp/md-ahead` (1-3) overrides for A/B, like `/tmp/md-groups` (voice threads 1-4, default 2).
  - **Voice budget did nothing before (fixed 2026-09-29).** Two bugs: (1) the cut wrote a silence slot that the same tick's
    `updateVoice` overwrote (now `m_silenceNext`, applied in the victim's own updateVoice; a victim triggered earlier in
    the same block has its trigger cancelled); (2) the harness skip check `cmp a,b` never matched persisted code 1 (the
    empty machine GND--), so a silenced voice kept rendering; now `sub b,a / tst a`. Offline (`MD_BUDGET=n md-hash`):
    budget 5 -> mean 3 rendered voices (was 16). Unlimited budget keeps hash `15746c0610a40ddb`. Tails of stolen tracks are
    now really cut.
  - **Cores:** MPC's AudioWorker1 (core 1) is steadily 4x busier than the others; the engine now ranks cores by load since boot
    (`rankCores`, core 0 last): engine on the least busy (core 3 on the test Force), voice group g on the g-th.
  - **Group balancing:** a track that was not sounding picks the voice group with the least measured cost (per-voice DSP
    instructions from the harness flags, moving average in `HostModel::m_costEma`); `ParallelVoiceEngine::moveVoice`. Stats
    line `groups: g0_us g0_voices ...` (voices are summed over the 4 engine blocks per host block: /4).
  - **Force capacity, TRX kit, MPC busy:** ~600-900 us per sounding voice, and each group has a fixed ~300-600 us (idle voice
    loop + wake), so 2 groups give ~4 voices without crackle, not 8. Latest run: 6 voices, g0 4 voices 2.98 ms, g1 2 voices
    1.86 ms per block, still 3.9 ms mean, underruns ~90/s. The lever left is the per-voice cost itself (the recompiled DSP).
  - **Duty cap default 0.95 (was 0.7), user-tested on the Force:** at 0.7 the main thread (2.7 ms of a 2.9 ms block) hit the cap
    ~25x/s and the naps caused the glitching. With the cap off and 2 threads, TRX kit: budget 3-5 clean (0-6 underruns per
    10-40 s), budget 6 not (~300 underruns, mean 3.0 ms). Shipped defaults: VOICES 5, ring lead 3 (4 also fine: 11.6 ms,
    ring is 5 slots), ROM off. Device overrides: `/tmp/md-groups`, `/tmp/md-ahead`, `/tmp/md-duty`.
  - **Five TRX machines were silent (found 2026-09-29, user report): TRX XT, CP, MA, CL, XC.** The OS sets `out[0]` (the trigger
    flag) before calling a machine function; these five start with `tst.l (a1)` and return 0 words when it is 0, so they only
    compute on a trigger tick. `MachineRunner::compute` cleared out[0] and HostModel skipped the slot on n == 0. Fixed:
    `compute(..., trigger)` and n == 0 still writes the trigger word. Also `kSlotWords` 13 -> 32 (EFM-CY returns 15 words). Found
    by comparing with the emulated MD (mdProbe: `sysex:f000203c02005b<track><machine>00f7`, `note:0:36:110`, `wait:N` prints
    the peak; `trace:on` + `tools/mdtrace/analysis/voice.py` shows the slot words the OS sends). New audio hashes (ROM samples):
    `e2b514e70c173c33` (1 and 2 voice groups); without ROM samples `04ef1db4fe767721`. `MD_SWEEP=1 md-hash` plays every machine
    on a fresh engine; the release build now fails if any offered machine is silent (all 82 offered ones make sound).
    The recompiler discovery now uses the same trigger protocol (code = id + 1) so these machines' DSP code is covered.

## STATE AT END OF SESSION (2026-09-29, late) - read this first if resuming
- Branch `claude/v0.2-roadmap` (off `claude/trusting-maxwell-hx3i7b`; PR #2 to main is still open/unmerged; nothing of v0.2 is pushed).
  Everything below is committed locally except this note.
- **Deployed on the Force (192.168.1.44)**: `machinedrum_one.so` md5 `fa1876b2781de4bb48d37a9fd6975aed` (VOICES default 4) (built by the full
  `release/build_release.sh`, gates passed: recompiled == interpreter, every offered machine sounds) and the rebuilt skin in
  `/sdcard/Synths/sd88me - VST - Machinedrum Module` (ROM toggle now shows OFF). Re-insert the plugin in MPC to load both; the skin
  needs an MPC restart to be re-read if the GLOBAL tab still shows ROM ON. **Not yet listened to** on the device after the
  silent-machine fix: play TRX XT/CP/MA/CL/XC, then re-test the busy TRX pattern at budget 4-5.
- Defaults now: VOICES 4 (user-tested: 4 clean, 5 glitchy on the busy TRX pattern), ROM off, 2 voice threads (track FX on the same threads), ring lead 3 blocks (ring is 5 slots), duty cap 0.95.
  Device overrides (files in /tmp, cleared by reboot, then re-insert the plugin): `md-groups` (1-4), `md-ahead` (1-4), `md-duty`
  (0.3-1.0); stats: `/tmp/md-stats-on` -> `/tmp/md-stats.<pid>` (with per-group `groups:` lines).
- What this session found (details in the entries above): the voice budget never cut anything (two bugs, fixed); TRX
  XT/CP/MA/CL/XC were silent (trigger flag, fixed); MPC's AudioWorker1 makes core 1 busy so cores are ranked by load; the 70% duty
  cap caused glitches; two threads help but capacity is still ~4-5 TRX/EFM voices (600-900 us per voice on a busy Force).
- `dist/Machinedrum-Module-0.1.0-7-g884948d-mpc-armv7.zip` was built before the version fix below (its version string is not X.Y.Z
  so the catalog check refused it): rerun `release/build_release.sh <OS.syx> <flash.bin>` (about 25 min, one core mostly: it compiles the
  1.1 MB generated DSP file several times) to get a conformant zip; the script now derives X.Y.Z from `git describe`.
- Open, in order: (1) listen-test the fix; (2) cut the per-voice DSP cost (the recompiled DSP) - the only route to more polyphony;
  (3) faster release builds (build the two gate binaries in parallel, split the generated .inl); (4) bank/kit picker list;
  (5) master FX; (6) push `claude/v0.2-roadmap` and open a PR when happy (the v0.1.0 tag predates all of this).
- 2026-09-29 late: user confirmed TRX XT/CP/MA/CL/XC now play and sound right; VOICES default changed 5 -> 4.
  **Pending:** the rebuilt skin (VOICES knob shows 4) is in `vst/build/skin` but could not be copied: the Force went unreachable
  (No route to host). Deploy with: `cd vst/build/skin && tar -czf - "sd88me - VST - Machinedrum Module" | ssh root@192.168.1.44
  'cd /sdcard/Synths && tar -xzf -'` (then restart MPC to re-read the skin). Only the knob's first-paint image differs.

- **2026-09-30: master FX cost measured; recommendation is to emulate, not translate.** `MixerRef::runMaster` ($342->$971, rhythm
  echo/gate/EQ/dynamix/reverb, delay lines in external memory at `$1439xx`) costs a steady **~9,740 DSP instructions per 32-sample
  block = ~13.4 M instr/s** (x86 interpreter/JIT build of md-mm's dsp56300, scratch test, input-independent so far). That is about
  a third of DSP2's busy load, so emulating DSP1's master section (recompiled on ARM) is affordable, and far cheaper to build than a
  ~1,200-word bit-exact translation. Disassembly: build `tools/mddis` (see build_proto.sh flags) and run `mddis OS.syx 2 342 972`.
  Skin device: rebuilt skin (VOICES 4) deployed to the Force 2026-09-30; user listen-test passed.
  **Next:** (1) `md::MasterFx` = MixerRef-style DSP1 with only the master section run; inputs dry L/R (`X:$180`) and sends
  (`X:$1c0`, `X:$600`), params `Y:$150-$18c` (decode how the tick fills them from the 32 kit master-FX bytes at `$1000d7c+16`);
  (2) compare against md-mm's final audio; (3) wire REV/DEL into `Engine`, then the FX plugin/skin.
  Build note: MixerRef.cpp needs `setInterpreterEnabled`, so it only builds against `libs/dsp56300`, not md-mm's fork.

- **2026-09-30: multi-output spike result: MPC (Force) uses only the first stereo pair of a VST2 instrument.** `~/mpc-vst/poc/multiout.c`
  (8 outputs, pluginList `numOutputs="8"`): MPC probes `effGetOutputProperties` up to pin 7 and passes 8 valid `processReplacing`
  buffers, but the track only offers one audio-out setting (1,2) and only the main pair is heard; the mixer I/O section has no
  input selector for return/sub buses. So outputs 3+ are dropped: native per-track submix from ONE plugin instance is not possible.
  Untried alternative: several instances in one process sharing a single engine (each MPC track = one instance tapping one MD track).
- **2026-09-30: shared-engine spike (`~/mpc-vst/poc/shared.c`, log `/tmp/shared.log`) PASSED.** Three instances on three tracks:
  one process (same pid), one copy of the library's statics (process-wide id counter 0,1,2 and live count shared; `close` decrements it),
  `effOpen` is called twice per instance (harmless), `processReplacing` n=128 every ~2.9 ms per instance. **Audio callbacks run on MPC's
  worker threads (4 of them), and the thread for a given instance changes between calls**, so instances are processed concurrently and in no
  fixed order: a shared engine must be producer/consumer (the existing engine thread + ring already is), taps read their own read
  pointer, never drive the engine from a callback. Design sketch: primary instance (lowest live id, hands over on close) owns engine +
  MIDI; tap instances pick a source (track 1-16 mono-as-stereo, reverb send, delay send, main) and read it from the shared ring.
- **2026-09-30: "Machinedrum Tap" works on the Force (user-tested: routing a track to its own MPC track).** Built, not yet committed:
  `vst/tap_shared.h` (state shared in-process), `vst/tap/` (tap_engine.cpp, vst.json uid `MdTp`, params.json: one `source` choice:
  Off, Track 1-16, Reverb send, Delay send), primary changes in `vst/engine.cpp` (first Inst to claim `g_tap.owner` publishes each track's
  mono signal = `Mixer::solo` (sample x VOL << 4, no pan) plus rev/del stereo into an 8-slot ring per 128-frame block; a track with a
  tap is routed to individual out 0 so it leaves the main mix; `md_tap_shared()` exported), `Mixer::solo`, CMake target
  `machinedrum_tap`, `build_so.sh` builds and strips both. The tap finds the primary with `dlopen(<own dir>/machinedrum_one.so, RTLD_NOLOAD)`
  + `dlsym`, so both .so must be in the same folder (/sdcard/vst) and the primary loaded first. Registered on the device by hand
  (`MPC.settings` backup `.bak-tap`); the old `MultiOut Spike` entry (poc/multiout.c, now holding poc/shared.c) is still registered.
  **Open:** (1) tap skin/layout (plain MPC param screen now); (2) tap-to-primary offset is +-1 block (2.9 ms) by call order, check for
  phasing between a tap and the main mix; (3) sends: tapped tracks have no REV/DEL send (individual routes carry none), the send taps
  carry only untapped main-routed tracks; (4) installer/release: ship the tap .so and its plugin-list entry (release/build_release.sh,
  catalog manifest); (5) remove the two spike entries from MPC.settings; (6) README section; (7) two primaries: the second does not publish.
- **2026-09-30 (later): tap FX + alignment verified on the Force.** `machinedrum_tapfx.so` (uid `MdTf`, effect, params: Source default Reverb send,
  Input through) registered and user-confirmed working (REV send out of a return track). Sends now include tapped tracks (Mixer `dryMute`:
  tapped tracks leave only the dry main). Alignment: the tap takes hostRead-1 if the primary was called within half a period, else hostRead.
  Measured (`/tmp/md-stats-on` -> `/tmp/md-tap-stats.<pid>`): 345 calls/s, the primary always ran first, 166-256 us before the tap, far from
  the 1451 us threshold, so tap and main are sample-aligned. Spike entries removed from MPC.settings.
  **Gotcha (caused a crash loop, fixed):** MPC.settings PLUGIN elements span several lines; delete with `sed '/name="X"/,/\/>/d'`, never one line.
  Backups: `MPC.settings.bak-tap`, `.bak-tapfx`. **Next:** LCD-style skin for the taps (tools/mdskin), release packaging (installer, catalog).
- **2026-09-30 (later still): taps are multi-select; LCD skin for the taps; README section.** Each source is its own on/off param (`src1`..`src16`,
  `src_rev`, `src_del`, `through` on the FX build); a tap sums whatever is on (mask in `Tap`, per-source counts in `Shared`). Skin generator
  `tools/mdskin/mk_tap_skin.py` (same LCD look, own copy of the drawing code): `docker run ... mpc-vst-html-art python3 tools/mdskin/mk_tap_skin.py
  vst/build/art.json vst/tap/params.json vst/tap/build/skin tap` (and `tapfx ... fx`). No master FX in this version, by decision: the sends leave
  through the taps and MPC's own effects do the job.
  **Future, Gen 2 / higher-power devices:** a faithful build with the MD's master section included. Measured 9,740 DSP instr/block (~13.4 M/s)
  for `$342-$970`; plan = emulate DSP1's master section (MixerRef-style, Y:$150-$18c params from the kit's master-FX bytes, external delay memory)
  as an optional `MasterFx` stage after the mixer, off by default, gated on device speed. See the 2026-09-30 master-FX entry above.

- **2026-09-30: silence release (the main fix for glitching when played live).** Measured: once triggered, a voice cost its full DSP time
  for good (e.g. TRX BD +2,138 instr/block at 70 ms and still at 4 s, output digital zero from ~0.5 s): only a budget steal ever freed it.
  `HostModel::setSilenceRelease(threshold, blocks)`: a sounding voice whose DSP2 output stays within +-128 (-96 dBFS, below one LSB of the
  plugin's 16-bit output) for 138 blocks (100 ms) is freed like a budget cut (m_silenceNext -> GND--, leaves m_activeOrder). Off by default
  in HostModel (hash e2b514e70c173c33 unchanged); the plugin turns it on (`/tmp/md-release` = hold in blocks, 0 = off; stats line
  `released=`). Default machine settings fall below -96 dB within 0.2-2 s (TRX CY slowest). Offline (md-hash, unlimited budget): mean
  rendered voices 10.6 -> 6.4. Hit after a release vs on a running voice (every non-ROM machine): identical for most; noise-based machines
  (TRX CP/RS/CB/CH/OH/CY/MA, several P-I) differ by a fresh noise sequence and +-1.6 dB, the same as two normal consecutive hits do.
  Scratch tools for this were decay.cpp / rehit.cpp (not committed). **Next levers:** A/B 3 voice groups now that groups are cost-balanced;
  pipeline the OS tick (~190 us); cut each group's fixed idle-slot loop cost; recompiler quality (2.1x vs Monomachine's 3.8x).
- **2026-09-30: silence release + 3 voice threads measured on the Force (user: "definitely a lot better", then "better" with 3 threads).**
  Old build, after playing: 3 dead voices kept the engine at 3.07 ms per 2.9 ms block and ~390 underruns/s forever. With silence release
  (2 threads): idle load drops back; VOICES 5, ROM off, 2.2-2.6 ms mean and underruns flat (1,998 -> 2,002 over ~2 min); VOICES 6-7 or ROM
  on still overloaded (3.1-3.6 ms, groups 2.0-2.7 ms each). With 3 threads (`/tmp/md-groups` = 3): groups ~1.4-1.6 ms each, VOICES 6-8
  with ROM on at 2.3-2.9 ms mean, underruns mostly flat (1,062 -> 1,100 over the last minute of play); one unexplained burst (51 -> 939)
  at VOICES 5-6. **Default now 3 groups** (kDefaultGroups). VOICES default still 4: 6 looks safe now, pending the user's call.
- **2026-10-01: drum-pad layout for a plugin track, traced in the MPC binary (3.9.1.2): not reachable for a 16-track drum plugin. Closed.**
  `PluginProgram` (vtable 0x692fbac) overrides the base `Program` virtuals at slot 7 (drum-style pads; base = program type 0 or 2) and
  slot 6 (chromatic; base = keygroup/plugin/MIDI/CV) with a flag set when the plugin instance loads: `description.name == "DrumSynth:Multi"`
  (setter 0x24fe2dc, helper 0x2494954). The description is the one JUCE fills from the live plugin, so the name that counts is the VST's own
  `effGetEffectName`. The MPC.settings `name` alone did nothing, and neither did `category`. Tested on the Force by byte-patching
  PLUG_NAME in the .so: the track gets the drum layout, but only **8 pads, sending notes 0-7** (pads 9-16 send nothing), the same as
  Akai's 8-voice DrumSynth Multi. The browser lists it as "DrumSynth:Multi", and the skin is looked up as `<vendor> - VST - DrumSynthMulti`
  (colon dropped). No other lever: plugin folder files (`plugin-meta.xml`, `version.xml`, `Presets/*.xpl`) carry identity/state only,
  MPC sends VSTs only stock JUCE canDo/opcodes, and the `AudioPluginInstanceExtended*` interfaces are C++ (built-in plugins only, none
  about pads). A Drum track routed to the plugin through MPC's JUCE virtual MIDI ports (`aconnect` loopback) would work, but needs the
  connection remade at each MPC start. User: neither the 8-pad edition nor the loopback is worth it.
- **2026-10-01 (later): drum pads reopened as an MPC runtime patch; the 8-pad limit found.** MPC 3.9.1.2, `/usr/bin/MPC` Build ID
  `b3a38f398cea4814618320b2251d9dd07580e393` (ARM mode, PIE but loaded with file offset == vaddr for .text).
  - **Drum flag:** set once per plugin load at `0x24fe2dc` (`bl 0x2494954` -> `strb r0,[r4,#3100]`). `0x2494954` is a 16-byte helper,
    `juce::String == "DrumSynth:Multi"` (`e59f1004 e08f1001 ea925022` + literal), and that call is its only caller. Replacing its body
    with `ldr pc,[pc,#-4]; .word fn` sends the check to our own `bool fn(const juce::String*)` (juce::String = one pointer to UTF-8).
  - **Pad count:** `0x14d2734(padModel, n)` sets the pad count (+224). Six sites choose it as
    `modeFlag ? 16 : (isPluginDrum(program) ? 8 : 128)`. `isPluginDrum` = `0x24948ec` (type 3 and slot 6 false). The 8 is
    `movne r1,#8` = `13a01008` at `0x18ff260 0x18ffbd8 0x18ffc84 0x1900e04 0x1900e98 0x19016fc`; `13a01010` gives 16.
    In drum mode, pad n sends note n-1 (measured: pads 1-8 -> notes 0-7), so the plugin must take notes 0-15 as tracks 1-16.
  - Related: `0x2494840` = plugin-drum or CV-drum (shared code). False lead: `0xf90038` (32/8/64 = timing divisions, not pads).
  - Plan: a small LD_PRELOAD module (like force_shadow) that checks the Build ID, patches these in memory at startup
    (`mprotect`), and does nothing on any other build. The firmware file is not modified.
  - **Plan for the next session: `docs/HANDOFF-mpc-drum-pads.md`** (an on-disk patch applied on the device, offered as an advanced build option; the in-memory
    preload is the alternative).
