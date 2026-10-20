#!/usr/bin/env bash
# Measures the Machinemodule engine on an Ableton Move: cross-builds md-hash and md-cost for aarch64 (static, so the Move's
# glibc 2.35 does not matter) with the dsp56300 JIT, copies them with the user's own OS and ROM samples to the device, and runs
#   md-hash   12 s of a dense 16-track pattern: the hash (must be e2b514e70c173c33) and the wall time on one core
#   md-cost   per-machine cost of one playing voice, against the 2902 us budget of a 128-frame block
#   usage: schwung/tools/move_bench.sh <OS 1.63 .syx> <ROM_SAMPLES.bin> [user@host]
# The binaries go to /data/UserData/md-bench on the device (never /tmp there: the rootfs is full). Results print here.
set -euo pipefail
OS=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); ROM=$(cd "$(dirname "$2")" && pwd)/$(basename "$2"); DEV=${3:-ableton@move.local}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${BENCH_DIR:-$ROOT/schwung/build/bench}
mkdir -p "$OUT"
docker image inspect md-move-builder >/dev/null 2>&1 || docker build -q -t md-move-builder -f "$ROOT/schwung/tools/Dockerfile.move-builder" "$ROOT" >/dev/null
docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT":/r:ro -v "$OUT":/o md-move-builder sh -c "
  cmake -S /r -B /o/jit -G Ninja -DCMAKE_TOOLCHAIN_FILE=/r/schwung/tools/aarch64.cmake -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS=-mcpu=cortex-a72 -DCMAKE_C_FLAGS=-mcpu=cortex-a72 -DCMAKE_EXE_LINKER_FLAGS=-static >/dev/null &&
  ninja -C /o/jit md-cost md-hash 2>&1 | grep -v 'dlopen\|Wno-dev' | tail -2"
ssh "$DEV" 'mkdir -p /data/UserData/md-bench'
scp -q "$OUT/jit/md-hash" "$OUT/jit/md-cost" "$DEV:/data/UserData/md-bench/"
scp -q "$OS" "$DEV:/data/UserData/md-bench/os.syx"
scp -q "$ROM" "$DEV:/data/UserData/md-bench/ROM_SAMPLES.bin"
ssh "$DEV" 'cd /data/UserData/md-bench
  echo "== md-hash, one core (core 2)"
  /usr/bin/time -f "wall %e s for 12 s of audio" taskset -c 2 ./md-hash os.syx ROM_SAMPLES.bin 2>&1 | grep -v "write\|ESSI\|read16\|updateClock"
  echo "== md-hash, silence release on (what the module will run)"
  MD_RELEASE=138 /usr/bin/time -f "wall %e s for 12 s of audio" taskset -c 2 ./md-hash os.syx ROM_SAMPLES.bin 2>&1 | grep -v "write\|ESSI\|read16\|updateClock"
  echo "== md-cost (core 2)"
  taskset -c 2 ./md-cost os.syx ROM_SAMPLES.bin 2>&1 | grep -v "write\|ESSI\|read16\|updateClock\|Jit@\|MemoryBuffer"'
