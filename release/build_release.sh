#!/usr/bin/env bash
# Builds Machinedrum Module's installer zip from YOUR OWN Machinedrum files (nothing of Elektron's is in this repo, and the
# result contains firmware-derived code and data, so it is for your own devices only - never share or publish it).
#
#   release/build_release.sh <OS .syx> [flash .bin] [-v version] [-d device-ip] [-m mpc-vst-plugins checkout] [-p]
#
#   The flash image is optional: with it you get the ROM sample machines and the 16 factory kits; without it, every other machine
#   and any kit .syx you add (the ROM machines stay silent, there are no factory kits).
#   -v  version string (default: from `git describe`, e.g. 0.1.0)
#   -d  after building, copy the zip to the Force and run its installer (stops and restarts MPC: save your project first)
#   -m  mpc-vst-plugins checkout (default: $MPC_VST_DIR, ../mpc-vst, else cloned to ~/.cache); its main has the wrapper's
#       "dynamic_name"/"dynamic_display" support this plugin needs, and the catalog checker
#   -p  ADVANCED, off by default: also stage dist/mpc-os-patch/, an on-device patch of the factory MPC OS (3.9.1.2 only) that gives the
#       plugin the 16-pad drum layout. It modifies /usr/bin/MPC, a firmware update removes it, uninstall.sh undoes it. With -d it is
#       run on the device after the plugin install (asks you to type PATCH). See docs/HANDOFF-mpc-drum-pads.md.
# Other input: MDPROBE (a ready-built mdProbe; if not set and not built, tools/mdtrace/build_mdprobe.sh builds it first).
# Needs Docker (the md-armhf-builder image is built on first use; python:3.11-slim is pulled). Output: dist/Machinedrum-Module-, Machinedrum-Tap- and Machinedrum-Tap-FX-<version>-mpc-armv7.zip (install all three with -d).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
# absolute path without GNU realpath (macOS has no `realpath -m`, older macOS no realpath at all); a missing file stays as given
rp() { if [ -d "$1" ]; then (cd "$1" && pwd); elif [ -e "$1" ]; then echo "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; else echo "$1"; fi; }
# q: run a build command quietly into build-release/build.log; if it fails, say which one and show the end of the log
q() { "$@" >>"$ROOT/build-release/build.log" 2>&1 || { echo "FAILED: $*" >&2; echo "--- last 30 lines of build-release/build.log ---" >&2; tail -n 30 "$ROOT/build-release/build.log" >&2; echo "--- (send this, the step number and 'uname -m' with your report) ---" >&2; exit 1; }; }
usage() { sed -n '2,18p' "$0"; exit 2; }
[ $# -ge 1 ] || usage
OS=$(rp "$1"); shift
FLASH=""; if [ $# -ge 1 ] && [ "${1#-}" = "$1" ]; then FLASH=$(rp "$1"); shift; fi
VERSION=""; DEVICE=""; MV="${MPC_VST_DIR:-}"; PATCHOS=0
while getopts "v:d:m:ph" o; do case $o in p) PATCHOS=1;; v) VERSION=$OPTARG;; d) DEVICE=$OPTARG;; m) MV=$OPTARG;; *) usage;; esac; done
if [ -z "$VERSION" ]; then VERSION=$(git -C "$ROOT" describe --tags --always 2>/dev/null | sed -E 's/^v//; s/^([0-9]+\.[0-9]+\.[0-9]+)-.*/\1/'); fi   # commits after a tag: still X.Y.Z (the catalog check needs it); pass -v to name a release
case "$VERSION" in [0-9]*) ;; *) VERSION="0.0.0-dev.$VERSION";; esac
if [ -z "$MV" ]; then
  if [ -d "$ROOT/../mpc-vst" ]; then MV="$ROOT/../mpc-vst"
  else
    MV="$HOME/.cache/mpc-vst-machinedrum/mpc-vst-plugins"
    [ -d "$MV" ] || { mkdir -p "$(dirname "$MV")"; git clone -q https://github.com/sd88me/mpc-vst-plugins.git "$MV"; }
  fi
fi
MV=$(rp "$MV")
PROBE=$(rp "${MDPROBE:-$ROOT/libs/gearmulator-md-mm/build/source/elektron/md/mdLibTest/mdProbe}")
if [ ! -e "$PROBE" ] && [ -z "${MDPROBE:-}" ]; then echo "== mdProbe not built yet: building it (first run only, a few minutes)"; "$ROOT/tools/mdtrace/build_mdprobe.sh"; fi
[ -e "$PROBE" ] || { echo "missing: $PROBE (mdProbe: build it with tools/mdtrace/build_mdprobe.sh, or point MDPROBE at it)" >&2; exit 1; }
for f in "$OS" ${FLASH:+"$FLASH"} "$PROBE" "$MV/tools/release.py" "$MV/tools/gen_vst.py"; do [ -e "$f" ] || { echo "missing: $f" >&2; exit 1; }; done
grep -q "dynamic_name" "$MV/wrapper/vst2_wrap.c" || { echo "$MV's wrapper has no dynamic_name support: use mpc-vst-plugins main (or later)" >&2; exit 1; }
# Docker is needed from step 5 (skin) on: check now, not after the 30 minutes of steps 1-4
command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1 || {
  echo "Docker isn't available. Start Docker Desktop and wait until it says it is running, then run this again." >&2
  echo "  Windows (WSL): Docker Desktop > Settings > Resources > WSL integration > turn on your Ubuntu, then Apply & restart;" >&2
  echo "                 then close and reopen the Ubuntu window. Test it with:  docker run --rm hello-world" >&2
  echo "  Linux: sudo apt install docker.io, then sudo usermod -aG docker \$USER and log out and in again." >&2
  exit 1; }
cd "$ROOT"
[ -f libs/dsp56300/source/dsp56kEmu/dsp.h ] || git submodule update --init --recursive
WORK=$ROOT/build-release; mkdir -p "$WORK" vst/build dist; : > "$WORK/build.log"
echo "Machinedrum Module $VERSION  (plugins checkout: $MV)"

echo "== 1/7 x86 tools (mdsamples, mdmachine)"
# On the plain interpreter (no JIT): these tools only read memory, and the JIT (x86-64/arm64 hosts, e.g. an Apple Silicon Mac) can
# crash while VoiceEngine initialises where the interpreter does not. Same dir as the tools' output: build-vst-x86/.
q cmake -Wno-dev -S . -B build-vst-x86 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-DDSP56K_NO_JIT_RUNTIME
q ninja -C build-vst-x86 mdsamples mdmachine mdartdump

if [ -n "$FLASH" ]; then
  echo "== 2/7 factory kits and ROM samples (from the flash image, by booting the emulated MD)"
  python3 tools/mdkits/make_factory.py "$PROBE" build-vst-x86/mdsamples "$FLASH" "$OS" vst/build/factory
  ROMS=(vst/build/factory/ROM_SAMPLES.bin)
else
  echo "== 2/7 no flash image given: building without the ROM sample machines and the factory kits"
  rm -rf vst/build/factory; mkdir -p vst/build/factory; ROMS=()
fi

echo "== 3/7 recompiled voice DSP (traced from your OS file and the ROM samples; not shipped as source)"
echo "   this step is quiet and takes 10-25 minutes: it has not stopped"
q cmake -Wno-dev -S . -B "$WORK/discovery" -G Ninja -DCMAKE_BUILD_TYPE=Release -DMD_DISCOVERY=ON
q ninja -C "$WORK/discovery" mdrecomp-discover
mkdir -p "$WORK/recomp"
if ! "$WORK/discovery/mdrecomp-discover" "$OS" "$WORK/recomp/disc.txt" ${ROMS[@]+"${ROMS[@]}"} > "$WORK/recomp/discover.log" 2>&1; then
  echo "step 3 failed: the tracing tool stopped. Its last output:" >&2; tail -n 20 "$WORK/recomp/discover.log" >&2
  echo "(send this, and the result of 'uname -m', with your report)" >&2; exit 1
fi
# demangled symbol list: GNU nm has -C; on macOS use LLVM's (brew install llvm) - Apple's nm has no -C
NM=nm; for c in llvm-nm gnm /opt/homebrew/opt/llvm/bin/llvm-nm /usr/local/opt/llvm/bin/llvm-nm; do command -v "$c" >/dev/null 2>&1 && { NM=$c; break; }; done
"$NM" -C "$WORK/discovery/mdrecomp-discover" > "$WORK/recomp/nm.txt" 2>/dev/null || { echo "this step needs an nm that supports -C (macOS: brew install llvm)" >&2; exit 1; }
# macOS (Mach-O): nm lists addresses from the image base (0x100000000, the address of __mh_execute_header), while the trace has offsets from
# the load address, so the recompiler finds no symbol for any handler ("KeyError: no symbol for handler offset ..."). Shift the list to start at 0.
python3 - "$WORK/recomp/nm.txt" <<'PY'
import re, sys
p = sys.argv[1]
lines = open(p).read().splitlines()
base = next((int(m.group(1), 16) for m in (re.match(r'([0-9a-f]+) [A-Za-z] _?_mh_execute_header$', l.strip()) for l in lines) if m), 0)
if base:
    out = []
    for l in lines:
        m = re.match(r'([0-9a-f]{8,16}) (.*)$', l.strip())
        out.append("%016x %s" % (int(m.group(1), 16) - base, m.group(2)) if m and int(m.group(1), 16) >= base else l)
    open(p, "w").write("\n".join(out) + "\n")
    print("   macOS binary: symbol addresses shifted by 0x%x" % base)
PY
python3 libs/dsp56300/tools/arm32jit_prototype/recomp/recomp_gen2.py "$WORK/recomp/disc.txt" "$WORK/recomp/nm.txt" > "$WORK/recomp/dsp56k_recomp.inl"

echo "== 4/7 bit-exactness gate: the recompiled voice DSP must give the same audio as the plain interpreter"
for v in interp recomp; do
  flags="-DDSP56K_NO_JIT_RUNTIME"; [ $v = recomp ] && flags="$flags -DDSP56K_RECOMP -I$WORK/recomp"
  q cmake -Wno-dev -S . -B "$WORK/gate-$v" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_FLAGS=$flags"
  q ninja -C "$WORK/gate-$v" md-hash
done
H_INTERP=$("$WORK/gate-interp/md-hash" "$OS" ${ROMS[@]+"${ROMS[@]}"} 2>/dev/null | grep '^hash')
H_RECOMP=$("$WORK/gate-recomp/md-hash" "$OS" ${ROMS[@]+"${ROMS[@]}"} 2>/dev/null | grep '^hash')
echo "   interpreter: $H_INTERP"; echo "   recompiled:  $H_RECOMP"
ROMGATE='!($3 ~ /^ROM/ && substr($3,4)+0 > 32)'; [ -n "$FLASH" ] || ROMGATE='$3 !~ /^ROM/'   # no flash image: no ROM machine is expected to sound
# Every machine the plugin offers must make sound (TRX XT/CP/MA/CL/XC were silent until their function was given the trigger flag).
SILENT=$(MD_SWEEP=1 "$WORK/gate-recomp/md-hash" "$OS" ${ROMS[@]+"${ROMS[@]}"} 2>/dev/null | awk '/^SWEEP/ && $5+0 < 100 && $3 !~ /^(GND--|INP|MID|CTR|RAM)/ && '"$ROMGATE"' {print $3}' | tr '\n' ' ')
[ -z "$SILENT" ] || { echo "GATE FAILED: these offered machines make no sound: $SILENT" >&2; exit 1; }
[ -n "$H_INTERP" ] && [ "$H_INTERP" = "$H_RECOMP" ] || { echo "GATE FAILED: the recompiled build does not match the interpreter - not building for the device" >&2; exit 1; }

echo "== 5/7 skin"
tools/mdskin/build_skin.sh "$OS" "$MV" | tail -1

echo "== 6/7 plugin (armhf)"
vst/build_so.sh "$WORK/recomp" "$MV"

echo "== 7/7 installer"
# the plugin reads your OS file from its data dir under this exact name, so the installer carries it (your own file, per-user zip)
rm -rf vst/build/payload && mkdir -p vst/build/payload && cp -r vst/build/factory vst/build/payload/factory && cp "$OS" vst/build/payload/Elektron_SPS1-1UW_OS1.63.syx
python3 "$MV/tools/gen_vst.py" vst/vst.json >/dev/null   # its pluginlist-entry.xml (custom skin: no skin from gen_vst)
python3 "$MV/tools/release.py" --so vst/build/machinedrum_one.so \
  --skin "vst/build/skin/sd88me - VST - Machinedrum Module" --entry vst/build/pluginlist-entry.xml \
  --version "$VERSION" --extra vst/build/payload:vst/machinedrum \
  --id machinedrum-module --repo sd88me/mpc-vst-machinedrum --license AGPL-3.0-only \
  --requires "Your own Machinedrum OS 1.63 file (and, for the ROM machines and factory kits, flash image): this zip is built from them, contains Elektron-derived data and is for your own devices only" \
  --about "Machinedrum Module: the Elektron Machinedrum UW sound engine as an MPC OS instrument (built from your own firmware)" -o dist
ZIP=$(ls dist/Machinedrum-Module-"$VERSION"-*.zip); ls -l "$ZIP"
# catalog conformance (mpc-vst-plugins docs/CATALOG_SPEC.md): the manifest, layout, checksums, ELF/glibc limits
python3 "$MV/tools/catalog_check.py" "$ZIP" --catalog --expect-id machinedrum-module --expect-repo sd88me/mpc-vst-machinedrum

# The taps: one zip each (release.py packages one plugin per zip). No firmware in them; they read the Module in the same project.
ZIPS="$ZIP"
for t in "tap:machinedrum_tap:Machinedrum Tap:machinedrum-tap:instrument" "tapfx:machinedrum_tapfx:Machinedrum Tap FX:machinedrum-tap-fx:effect"; do
  IFS=: read -r dir so name id kind <<<"$t"
  python3 "$MV/tools/gen_vst.py" "vst/$dir/vst.json" >/dev/null   # its pluginlist-entry.xml (custom skin: no skin from gen_vst)
  python3 "$MV/tools/release.py" --so "vst/build/$so.so" --skin "vst/$dir/build/skin/sd88me - VST - $name" --entry "vst/$dir/build/pluginlist-entry.xml" \
    --version "$VERSION" --id "$id" --repo sd88me/mpc-vst-machinedrum --license AGPL-3.0-only \
    --requires "Machinedrum Module (same version) in the same project" \
    --about "$name: puts Machinedrum Module tracks and its reverb/delay sends on their own MPC track ($kind)" -o dist
  Z=$(ls dist/"${name// /-}"-"$VERSION"-*.zip); ls -l "$Z"
  python3 "$MV/tools/catalog_check.py" "$Z" --catalog --expect-id "$id" --expect-repo sd88me/mpc-vst-machinedrum
  ZIPS="$ZIPS $Z"
done

if [ -n "$DEVICE" ]; then
  echo "== installing on $DEVICE (stops and restarts MPC once per zip)"
  for Z in $ZIPS; do
    d=$(basename "$Z" -mpc-armv7.zip)
    ssh "root@$DEVICE" 'cat > /tmp/machinedrum-release.zip' < "$Z"
    ssh "root@$DEVICE" "cd /tmp && rm -rf '$d' && unzip -o -q machinedrum-release.zip && cd '$d' && sh install.sh -y"
  done
fi
if [ "$PATCHOS" = 1 ]; then
  echo "== advanced: MPC OS drum-pad patch (3.9.1.2 only; modifies the factory /usr/bin/MPC; a firmware update removes it)"
  rm -rf dist/mpc-os-patch; mkdir -p dist/mpc-os-patch; cp release/mpc_patch/install.sh release/mpc_patch/uninstall.sh release/mpc_patch/mpc-3.9.1.2.patch dist/mpc-os-patch/
  echo "staged: dist/mpc-os-patch/ (copy to the Force and run: sh install.sh; undo: sh uninstall.sh)"
  if [ -n "$DEVICE" ]; then
    ssh "root@$DEVICE" 'rm -rf /tmp/mpc-os-patch && mkdir /tmp/mpc-os-patch' && scp -q dist/mpc-os-patch/* "root@$DEVICE:/tmp/mpc-os-patch/"
    ssh -t "root@$DEVICE" 'sh /tmp/mpc-os-patch/install.sh'
  fi
fi
