#!/usr/bin/env bash
# Builds the skin folders (vst/build/skin/, and the two taps' in vst/tap/build/skin/ and vst/tapfx/build/skin/) from the user's own OS files, with tools/mdskin/mk_skin.py (a port of
# mpc-vst-monomodule's own skin generator). Needs Docker (a stock python image, for Pillow) and an x86 build
# of this repo (build-vst-x86/, for mdmachine and mdartdump).
#   build_skin.sh <MD OS.syx> [mpc-vst checkout] [skin=... ink=... paper=...]
# The Elektron LCD fonts and dial the skin is drawn with are read from the same OS file (mdartdump -> vst/build/art.json).
# If you also have mpc-vst-monomodule's art.json (its own dump of your Monomachine OS; MNM_ART, default
# ../mpc-vst-monomodule/vst/build/art.json), that is used instead: it has the LCD's real OFF/ON toggle icon, which the MD OS
# lacks (mdartdump draws a stand-in), and the original skin's 1 and 9 digits.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
rp() { if [ -d "$1" ]; then (cd "$1" && pwd); else echo "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; fi; }   # no GNU realpath on macOS
OS=$(rp "$1"); MV=$(rp "${2:-$HOME/mpc-vst}")
mkdir -p "$ROOT/vst/build"
ninja -C "$ROOT/build-vst-x86" mdmachine mdartdump >/dev/null
"$ROOT/build-vst-x86/mdmachine" "$OS" > "$ROOT/vst/build/machines.txt" 2>/dev/null
"$ROOT/build-vst-x86/mdartdump" "$OS" "$ROOT/vst/build/art.json"
MNM_ART=${MNM_ART:-$ROOT/../mpc-vst-monomodule/vst/build/art.json}
if [ -f "$MNM_ART" ]; then cp "$MNM_ART" "$ROOT/vst/build/art.json"; echo "skin art: $MNM_ART (Monomodule's)"; fi
rm -rf "$ROOT/vst/build/skin"
# Pillow in a stock python image (no browser needed: this skin is drawn with Pillow only), so nothing has to be built or pulled from elsewhere
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -e MPC_VST_TOOLS=/mv/tools -v "$ROOT":/r -v "$MV":/mv:ro -w /r python:3.11-slim sh -c \
  "pip install -q --no-warn-script-location --target /tmp/p pillow >/dev/null 2>&1; PYTHONPATH=/tmp/p python3 tools/mdskin/mk_skin.py vst/build/art.json vst/build/machines.txt vst/params.json vst/build/skin ${*:3} &&
   rm -rf vst/tap/build/skin vst/tapfx/build/skin &&
   PYTHONPATH=/tmp/p python3 tools/mdskin/mk_tap_skin.py vst/build/art.json vst/tap/params.json vst/tap/build/skin tap ${*:3} &&
   PYTHONPATH=/tmp/p python3 tools/mdskin/mk_tap_skin.py vst/build/art.json vst/tapfx/params.json vst/tapfx/build/skin fx ${*:3}"
