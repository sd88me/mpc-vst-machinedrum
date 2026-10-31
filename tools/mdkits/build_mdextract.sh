#!/usr/bin/env bash
# Cross-builds mdextract for the Gen2 MPCs (aarch64): the helper the plugin runs on the device, the first time it finds the user's
# flash image next to the OS file, to make the factory kits and the ROM sample memory from it (vst/engine.cpp, extractFlash). Built
# in the md-aarch64-builder image from gearmulator-md-mm, which tools/mdtrace/build_mdprobe.sh has patched and given mdExtract.cpp.
# Contains none of Elektron's data. Output: the stripped executable.
#   tools/mdkits/build_mdextract.sh <out file>
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
[ -f "$ROOT/libs/gearmulator-md-mm/source/elektron/md/mdLibTest/mdExtract.cpp" ] || "$ROOT/tools/mdtrace/build_mdprobe.sh" >/dev/null
docker image inspect md-aarch64-builder >/dev/null 2>&1 || docker build -q -t md-aarch64-builder -f "$ROOT/tools/Dockerfile.aarch64-builder" "$ROOT" >/dev/null
docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT":/r -v "$(dirname "$OUT")":/o md-aarch64-builder sh -c "
  cd /r/libs/gearmulator-md-mm &&
  cmake -S . -B build-aarch64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=/r/tools/aarch64.cmake -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXE_LINKER_FLAGS='-static-libstdc++ -static-libgcc' -Dgearmulator_BUILD_JUCEPLUGIN=OFF -Dgearmulator_BUILD_JUCEPLUGIN_CLAP=OFF -DBUILD_TESTING=ON >/dev/null 2>&1 &&
  ninja -C build-aarch64 mdExtract >/dev/null &&
  aarch64-linux-gnu-strip -o /o/$(basename "$OUT") build-aarch64/source/elektron/md/mdLibTest/mdExtract"
