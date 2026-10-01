# Handoff: 16 drum pads for Machinedrum Module (MPC OS patch, advanced option)

Status (2026-10-01): **analysis done, nothing built yet.** This file is the plan for the next session. The addresses and
byte values below were read from the Force's `/usr/bin/MPC` and checked. The patch code itself has not been written or
run.

## Goal

When Machinedrum Module is loaded on a track, MPC should show the **16-pad drum layout** (one pad per Machinedrum track,
drum view in the sequencer) instead of the melodic one. The plugin keeps its own name, skin and sound.

This ships as an **advanced, opt-in option** of the build/deploy. Users must be told plainly:
- it modifies the factory MPC OS (`/usr/bin/MPC`);
- it works only on MPC OS **3.9.1.2** (one exact binary, checked by md5) and refuses anything else;
- a firmware update replaces the binary and removes the patch; re-run the option after updating (it will refuse until
  this project supports the new build);
- it is undone with the uninstall step (which restores the original bytes).

It must not depend on MockbaMod. Many users only have root SSH, which the installer already needs.

## What MPC does today (traced, MPC 3.9.1.2)

Binary: `/usr/bin/MPC`, 112,209,764 bytes, **md5 `592eebc8e1ce0797dc8c98e7002143b8`**, GNU Build ID
`b3a38f398cea4814618320b2251d9dd07580e393`. ARM (A32) code, PIE. In the first LOAD segment the **file offset equals
the virtual address**, so every address below is also a file offset.

1. **Drum flag.** When a plugin loads, `0x24fe2dc: bl 0x2494954` then `strb r0,[r4,#3100]` stores
   `description.name == "DrumSynth:Multi"`. `PluginProgram` vtable `0x692fbac` returns that flag at slot 7 (drum pads)
   and its inverse at slot 6 (chromatic). The name is the one JUCE reads from the live plugin (`effGetEffectName`); the
   MPC.settings name alone does nothing (tested).
   - `0x2494954` is a 16-byte helper and `0x24fe2dc` is its **only** caller:
     ```
     2494954: e59f1004  ldr r1, [pc, #4]      @ literal at 0x2494960
     2494958: e08f1001  add r1, pc, r1        @ r1 = 0x4ab2760 "DrumSynth:Multi"
     249495c: ea925022  b   0x9289ec          @ bool juce::String::operator==(const char*) (r0 = String*)
     2494960: 0261de00  (literal)
     ```
2. **The 8-pad limit.** `0x14d2734(padModel, n)` sets the pad count (field +224). Six sites choose `n` as
   `modeFlag ? 16 : (isPluginDrum(program) ? 8 : 128)` (`isPluginDrum` = `0x24948ec`: type 3 and slot 6 false). The 8
   is `movne r1, #8` = **`13a01008`** at:
   `0x18ff260  0x18ffbd8  0x18ffc84  0x1900e04  0x1900e98  0x19016fc`.
   `13a01010` (`movne r1, #16`) gives 16. 128 is a normal drum program (8 banks of 16).
3. **Notes.** In this layout pad *n* sends MIDI note *n-1*. Measured with the plugin named DrumSynth:Multi: pads 1-8
   sent notes 0-7, and pads 9-16 sent nothing.
4. **Code cave.** 80 zero bytes at **`0x4a7b330`-`0x4a7b37f`** (after `.text` ends at `0x4a7b328`, before `.rodata` at
   `0x4a7b380`), inside the R+X LOAD segment and backed by the file. Nothing references them.
5. False lead, for the record: `0xf90038` returns 32/8/64, but these are timing divisions (note repeat), not pads.

## The patch (7 places, < 100 bytes)

### A. Drum flag for our plugin (helper redirect + cave)

The cave is `0x4a7b330`-`0x4a7b37f`, 80 bytes: alignment padding between `.fini` (`0x4a7b328`, 8 bytes) and `.rodata`
(`0x4a7b380`, 128-aligned). Confirmed with `readelf -S`. Distances are all beyond the +-32 MB range of `B`/`BL`
(helper -> cave about +39.7 MB; cave -> `0x9289ec` about -68 MB). So every jump is a PC-relative `ldr ip` + `add pc, pc, ip`.

Rewrite the whole 16-byte helper, whose only caller is `0x24fe2dc`:
```
2494954: ldr   r2, [r0]          @ juce::String -> const char* (a String is one pointer to its UTF-8 text)
2494958: ldr   ip, [pc, #0]      @ ip = word at 0x2494960
249495c: add   pc, pc, ip        @ pc = 0x2494964 + ip  -> cave
2494960: .word cave - 0x2494964
```
The cave (r0 = String*, r2 = text, lr = return into 0x24fe2e0):
```
cave:   adr   r1, name
1:      ldrb  r3, [r2], #1
        ldrb  ip, [r1], #1
        cmp   r3, ip
        bne   2f
        cmp   r3, #0
        bne   1b
        mov   r0, #1             @ "Machinedrum Module" -> drum layout
        bx    lr
2:      ldr   r1, L1             @ not ours: the original check, as a tail call
        add   r1, pc, r1         @ r1 = 0x4ab2760 "DrumSynth:Multi"
        ldr   ip, L2
        add   pc, pc, ip         @ -> 0x9289ec  bool juce::String::operator==(const char*)  (r0 still the String*, lr intact)
L1:     .word 0x4ab2760 - (pc of the add + 8)
L2:     .word 0x9289ec  - (pc of the add + 8)
name:   .asciz "Machinedrum Module"
```
Budget: 13 instructions (52 bytes) + 2 literals (8) + 19 bytes of string = **79 of 80 bytes**. Build it with
`arm-linux-gnueabihf-as` (link at `0x4a7b330` so the literals resolve), then check it with objdump against the real
addresses. If it doesn't fit, use a shorter marker name. Akai's DrumSynth:Multi still takes the original path, so it
keeps its drum layout.

Confirm `0x9289ec` really is `(const juce::String&, const char*) -> bool` before relying on it. The original helper
already calls it with r0 = String* and r1 = char*, and the caller only uses r0 (`strb r0,[r4,#3100]`).

### B. 16 pads

At the six addresses above: check each is `08 10 a0 13` and write `10 10 a0 13`. That's one byte per site, at
address+0: `08` -> `10`.

Side effect: Akai's DrumSynth Multi also gets 16 pads; its pads 9-16 send notes 8-15, which it ignores. Acceptable;
document it.

### C. Plugin side (this repo)

`vst/engine.cpp` `eMidi`: also accept notes 0-15 as tracks 1-16 (36-51 stays). This was done and tested on the Force
(it worked for pads 1-8), then reverted when the idea was dropped. Re-apply it:
```cpp
int track = static_cast<int>(msg[1]) - kBaseNote;
if(msg[1] < kTracks) track = msg[1];   // drum-pad layout sends pad n as note n-1
```
Update README's MIDI note map line.

## Delivery: on-disk patch, applied on the device (no MockbaMod needed)

Ship **only our bytes and offsets, never Akai's binary or a patched copy**. The device does the patching.

Files (new): `release/mpc_patch/`
- `mpc-3.9.1.2.patch` (or a shell-readable list): stock md5, patched md5, and for each region the offset and our new
  bytes (hex). The patched md5 is computed at build time by applying the patch to the user's own stock binary
  (`scp` it from the device into the scratch dir; it is never committed or packaged).
- `install.sh` / `uninstall.sh` (busybox `sh`, `dd`, `md5sum`; no python on the Force).
- A build step in `release/build_release.sh`, e.g. `-p` "advanced: also patch MPC OS for 16 drum pads", **off by
  default**. With `-d` it asks for a typed confirmation (e.g. `PATCH`) after printing the warnings.

`install.sh` on the device, in order:
1. `md5sum /usr/bin/MPC` must equal the stock md5. Otherwise stop and change nothing. The exception is when it already
   equals the patched md5: then say "already patched" and stop.
2. Find the **factory file**, not an overlay copy. Without MockbaMod `/usr` is plain rootfs (`/dev/mmcblk0p6`, mounted
   `ro`, only ~16 MB free, so no room for a second 112 MB copy and the patch must be in place). With MockbaMod, `/usr`
   is an overlay (upper `/media/az01-internal/system/usr/overlay`): patch the lower file through a bind mount
   (`mount --bind / /tmp/mdroot`, then `/tmp/mdroot/usr/bin/MPC`). **Never** leave a patched MPC in the overlay's upper
   dir: it would survive a firmware update and run on the wrong OS. Refuse if an `MPC` already exists in an upper dir.
3. Back up first. Copy the whole original to the SD card (`/sdcard/MPC-backup/MPC-3.9.1.2.orig`, 112 MB; the card has
   room, rootfs doesn't). Also save the original bytes of every region we change (`orig-regions.txt`). Check the backup's
   md5.
4. `systemctl stop acvs` (a running executable can't be written: ETXTBSY). Then `mount -o remount,rw /`.
5. Write each region with `dd of=... bs=1 seek=<offset> conv=notrunc`. Then `sync`.
6. `md5sum` must equal the patched md5. If not, write the original regions back at once, re-check the stock md5, and
   report failure.
7. `mount -o remount,ro /`, then `systemctl start acvs`. Verify MPC is up (`pidof MPC`).

`uninstall.sh`: same checks in reverse. If md5 = patched: stop acvs, remount rw, write back the saved original regions
(or copy the full backup back if the regions file is missing; that needs space, so do it via `cat backup > MPC` in
place), check the stock md5, remount ro, start acvs.

Recovery if MPC won't start after patching: SSH still works (sshd is separate from acvs). Run `uninstall.sh`, or copy
the SD backup back by hand. Put this in the README section.

### Optional alternative: in-memory patch (MockbaMod users)

The same 7 regions can be applied in memory at startup by an `LD_PRELOAD` module (same `mprotect` + write, Build ID
check, no-op otherwise). It leaves the firmware file untouched, but needs MockbaMod's preload hook (`run_*.sh` in
`AddOns/`, see `force-shadow/addon/run_ForceShadow.sh` for the idempotent `$mmLD_PRELOAD_VAR` pattern). Not the first
target; mention it in the README as a possible later option.

## Test plan (on the Force, with the user; ask before each restart)

1. Offline first: apply the patch to a scratch copy of the device's MPC with a host-side script. Check the stock and
   patched md5s. Disassemble every patched region with `arm-linux-gnueabihf-objdump` and read it back.
2. Device: run `install.sh`. Check:
   - **Machinedrum Module on a new track:** drum layout, 16 pads lit, pads 1-16 play tracks 1-16. Add a
     `/tmp/md-notes-on` style note log while testing; it was used for the measurement above, then reverted.
   - **Akai's DrumSynth Multi:** still gets the drum layout, with 16 pads.
   - **Another VST** (e.g. Monomodule): still melodic.
   - **An existing project with a Machinedrum track:** loads, and shows the drum layout because the flag is set when
     the plugin loads.
   - **Every screen of that track:** program edit, pad mixer, Q-Links, sequencer grid and list edit, note repeat, the
     plugin skin. Look for crashes. `0x24948ec` has 11 callers (note-repeat settings and others) that now see a drum
     program; one of them may expect Akai's DrumSynth object.
3. Run `uninstall.sh`: md5 back to stock; the track is melodic again after a restart.
4. Re-run `install.sh` on an already patched device ("already patched"). Run it on a modified binary (refuses).

## Open questions

- What is the "16" mode flag at `[obj+420]+392` in the six sites? (It forces 16 pads; possibly a 16-levels or
  pad-perform mode.) Not needed for the patch.
- Does a firmware update really rewrite `/dev/mmcblk0p6`? Expected, but unverified. Note it in the README as expected
  behaviour and check on the next real update.
- Should the name list be configurable (e.g. other sd88me drum plugins)? The cave fits one name; more names need a
  bigger cave or a different design.

## Constraints (unchanged)

- Ask before restarting MPC. Stage deploys. Never overwrite a loaded binary in place (stop acvs first).
- Nothing firmware-derived is committed or shipped. That includes the stock MPC binary and any patched copy. The
  patch file holds only our bytes, offsets and md5s.
