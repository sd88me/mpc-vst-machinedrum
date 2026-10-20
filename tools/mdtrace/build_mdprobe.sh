#!/usr/bin/env bash
# Builds mdProbe (the tool that boots the emulated Machinedrum from your flash image) inside libs/gearmulator-md-mm: fetches the
# sources it needs, applies this repo's two patches, copies mdProbe.cpp in and builds it. Safe to run again (it skips what is done).
# release/build_release.sh runs this by itself when mdProbe is missing. Needs git, cmake and ninja (see the README, "What you need").
#   tools/mdtrace/build_mdprobe.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
GM=$ROOT/libs/gearmulator-md-mm
for t in git cmake ninja; do command -v $t >/dev/null || { echo "missing tool: $t (macOS: brew install cmake ninja git; Linux: sudo apt install cmake ninja-build git)" >&2; exit 1; }; done
[ -f "$GM/CMakeLists.txt" ] || git -C "$ROOT" submodule update --init libs/gearmulator-md-mm
cd "$GM"
git submodule update --init --depth 1 source/dsp56300 source/mc68k source/cpp-terminal source/3rdparty/freetype source/3rdparty/RmlUi
(cd source/dsp56300 && git submodule update --init --depth 1 source/asmjit)
# apply a patch once: if it can be reversed cleanly it is already in
apply() { if git -C "$1" apply --reverse --check "$2" >/dev/null 2>&1; then echo "already patched: $(basename "$2")"; else git -C "$1" apply "$2"; fi; }
apply "$GM" "$HERE/gearmulator-md-mm.patch"
apply "$GM/source/dsp56300" "$HERE/dsp56300-md-mm.patch"
cp "$HERE/mdProbe.cpp" "$HERE/mdTraceTool.cpp" source/elektron/md/mdLibTest/
# MDPROBE_GENERATOR overrides the generator (default Ninja; on Windows run from a Visual Studio developer prompt, or let CI set it up)
cmake -S . -B build -G "${MDPROBE_GENERATOR:-Ninja}" -DCMAKE_BUILD_TYPE=RelWithDebInfo -Dgearmulator_BUILD_JUCEPLUGIN=OFF -Dgearmulator_BUILD_JUCEPLUGIN_CLAP=OFF -DBUILD_TESTING=ON >/dev/null
cmake --build build --config RelWithDebInfo --target mdProbe
echo "mdProbe: $GM/build/source/elektron/md/mdLibTest/mdProbe"
