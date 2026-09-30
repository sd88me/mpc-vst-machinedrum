#!/usr/bin/env bash
# Builds machinedrum_one.so for the Force (armhf), self-contained (just this repo's submodules + the
# CMakeLists.txt at its root), inside the md-armhf-builder image (built automatically from
# tools/Dockerfile.armhf-builder on first use).
#   build_so.sh <dir holding dsp56k_recomp.inl> <mpc-vst-plugins checkout>
# The recompiled DSP embeds firmware words: it is generated from the user's own OS file
# (libs/dsp56300/tools/arm32jit_prototype/recomp/) and never shipped -- REC below must point at your own
# build of it (see HANDOFF.md, "recompiler discovery pipeline" - tools/mdrecomp/mdrecomp_discover.cpp +
# recomp_gen2.py).
set -euo pipefail
rp() { (cd "$1" && pwd); }   # no realpath on older macOS
REC=$(rp "$1"); MV=$(rp "$2")
ROOT=$(cd "$(dirname "$0")/.." && pwd)   # repo root (this script lives in vst/)
if [ ! -f "$ROOT/libs/dsp56300/source/dsp56kEmu/dsp.h" ] || [ ! -f "$ROOT/libs/gearmulator-md-mm/source/mc68k/CMakeLists.txt" ]; then
  echo "submodules missing -- run: git -C '$ROOT' submodule update --init --recursive" >&2
  exit 1
fi
docker image inspect md-armhf-builder >/dev/null 2>&1 || docker build -q -t md-armhf-builder -f "$ROOT/tools/Dockerfile.armhf-builder" "$ROOT" >/dev/null
python3 "$MV/tools/gen_vst.py" "$ROOT/vst/vst.json" --params-h
python3 "$MV/tools/gen_vst.py" "$ROOT/vst/tap/vst.json" --params-h
python3 "$MV/tools/gen_vst.py" "$ROOT/vst/tapfx/vst.json" --params-h
mkdir -p "$ROOT/vst/build/obj"
docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT":/r -v "$REC":/rec:ro -v "$MV":/mv:ro md-armhf-builder sh -c "
  cmake -S /r -B /r/vst/build/obj -G Ninja -DCMAKE_TOOLCHAIN_FILE=/r/tools/armhf.cmake -DCMAKE_BUILD_TYPE=Release \
    -DMPC_VST_DIR=/mv '-DCMAKE_CXX_FLAGS=-DDSP56K_RECOMP -I/rec' >/dev/null &&
  ninja -C /r/vst/build/obj machinedrum_one machinedrum_tap machinedrum_tapfx md-vst-smoke 2>&1 | tail -5 &&
  arm-linux-gnueabihf-strip -o /r/vst/build/machinedrum_one.so /r/vst/build/obj/machinedrum_one.so &&
  arm-linux-gnueabihf-strip -o /r/vst/build/machinedrum_tap.so /r/vst/build/obj/machinedrum_tap.so &&
  arm-linux-gnueabihf-strip -o /r/vst/build/machinedrum_tapfx.so /r/vst/build/obj/machinedrum_tapfx.so"
# md5sum on Linux, md5 -r on macOS
for f in machinedrum_one machinedrum_tap machinedrum_tapfx; do if command -v md5sum >/dev/null 2>&1; then md5sum "$ROOT/vst/build/$f.so"; else md5 -r "$ROOT/vst/build/$f.so"; fi; done
