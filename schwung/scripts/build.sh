#!/usr/bin/env bash
# Builds the Machinemodule Schwung module for Ableton Move in Docker (md-move-builder: ubuntu 22.04's aarch64 toolchain, glibc 2.35 as on
# the Move) and packages it:
#   dist/machinemodule/                  module.json, dsp.so, md-engine
#   dist/machinemodule-module.tar.gz     the same, for Schwung Manager's "Install from File" (or schwung/scripts/install.sh)
# The firmware (the OS file, factory kits, ROM samples) is never part of it: the user uploads their own.
#   VERSION=0.1.0-test.2 schwung/scripts/build.sh     (default: module.json's version)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
S=$ROOT/schwung
BUILD=${BUILD_DIR:-$S/build/arm}
DIST=${DIST_DIR:-$S/dist}
if [ ! -f "$ROOT/libs/dsp56300/source/dsp56kEmu/dsp.h" ]; then
  echo "submodules missing -- run: git -C '$ROOT' submodule update --init --recursive" >&2; exit 1
fi
docker image inspect md-move-builder >/dev/null 2>&1 || docker build -q -t md-move-builder -f "$S/tools/Dockerfile.move-builder" "$S/tools" >/dev/null
MDPROBE_PREPARE_ONLY=1 bash "$ROOT/tools/mdtrace/build_mdprobe.sh" >/dev/null	# the patched emulator sources for mdProbe
REL=${BUILD#$ROOT/}
docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT":/r md-move-builder sh -ec "
  cmake -S /r/schwung -B /r/$REL -G Ninja -DCMAKE_TOOLCHAIN_FILE=/r/schwung/tools/aarch64.cmake -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS=-mcpu=cortex-a72 -DCMAKE_C_FLAGS=-mcpu=cortex-a72 -Wno-dev >/dev/null &&
  ninja -C /r/$REL dsp md-engine md-hosttest md-busybench md-import mdsamples &&
  cmake -S /r/libs/gearmulator-md-mm -B /r/$REL-mdprobe -G Ninja -DCMAKE_TOOLCHAIN_FILE=/r/schwung/tools/aarch64.cmake -DCMAKE_BUILD_TYPE=Release \
    -Dgearmulator_BUILD_JUCEPLUGIN=OFF -Dgearmulator_BUILD_JUCEPLUGIN_CLAP=OFF -DBUILD_TESTING=ON -DCMAKE_EXE_LINKER_FLAGS=-static \
    -DCMAKE_CXX_FLAGS=-mcpu=cortex-a72 -Wno-dev >/dev/null &&
  ninja -C /r/$REL-mdprobe mdProbe &&
  for f in md-import mdsamples; do aarch64-linux-gnu-strip -o /r/$REL/\$f.stripped /r/$REL/\$f; done &&
  aarch64-linux-gnu-strip -o /r/$REL/mdProbe.stripped /r/$REL-mdprobe/source/elektron/md/mdLibTest/mdProbe &&
  aarch64-linux-gnu-strip -o /r/$REL/dsp.stripped.so /r/$REL/dsp.so &&
  aarch64-linux-gnu-strip -o /r/$REL/md-engine.stripped /r/$REL/md-engine &&
  if aarch64-linux-gnu-objdump -T /r/$REL/dsp.so | grep -q 'GLIBC_2\.3[6-9]'; then echo 'dsp.so needs a glibc newer than the Move has' >&2; exit 1; fi"
rm -rf "$DIST/machinemodule"
mkdir -p "$DIST/machinemodule"
VERSION=${VERSION:-$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['version'])" "$S/module/module.json")}
python3 - "$S/module/module.json" "$DIST/machinemodule/module.json" "$VERSION" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1]))
m["version"] = sys.argv[3]
s = json.dumps(m, indent=2)
assert len(s) <= 8192, "module.json over Schwung's 8 KB limit"
open(sys.argv[2], "w").write(s + "\n")
EOF
cp "$BUILD/dsp.stripped.so" "$DIST/machinemodule/dsp.so"
cp "$S/module/help.json" "$DIST/machinemodule/help.json"
cp "$BUILD/md-engine.stripped" "$DIST/machinemodule/md-engine"
mkdir -p "$DIST/machinemodule/bin"	# the flash import's helpers (md-import starts the other two from its own folder)
for f in md-import mdsamples mdProbe; do cp "$BUILD/$f.stripped" "$DIST/machinemodule/bin/$f"; done
chmod 755 "$DIST/machinemodule/dsp.so" "$DIST/machinemodule/md-engine" "$DIST/machinemodule/bin/"*
tar -C "$DIST" -czf "$DIST/machinemodule-module.tar.gz" machinemodule
echo "==> done: $DIST/machinemodule-module.tar.gz ($VERSION)"
