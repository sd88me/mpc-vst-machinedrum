# Roadmap

Planned work, newest release first. Delete an item when it ships.

## Next release

- **Engine memory (from Osirus 1.0.0, 2026-11-22).** Osirus went from 244 to 108 MB per instance, same audio, by shrinking the DSP56300
  audio rings (`RingBufferSize` 8192*4 -> 4096 in `dsp56kEmu/audio.h`) and dropping the `mlock` of the DSP memory mirror (`memorybuffer.cpp`).
  Here `libs/dsp56300/source/dsp56kEmu/audio.h` already has 8192 and no `mlock`, so try 4096: measure the MPC's resident memory with one and two instances before and
  after, check DROPOUTS stays 0 under load, and compare a rendered note bit for bit (offline engine test).
