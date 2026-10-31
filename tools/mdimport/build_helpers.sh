#!/usr/bin/env bash
# Builds the flash-image import helpers (md-import, mdProbe, mdsamples) and installs them where the desktop plugin looks for them: the
# plugin's data folder, <data folder>/bin (the plugin also looks next to its own binary, where the desktop build copies them when it is
# configured with -DMM_MDPROBE=<mdProbe>). Run once; safe to run again.
#   tools/mdimport/build_helpers.sh [install dir]
# Needs git, cmake and ninja (see desktop/README.md); no JUCE or Linux dev packages; mdProbe also needs the gearmulator-md-mm submodules, which
# tools/mdtrace/build_mdprobe.sh fetches (about 10 minutes the first time).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
PROBE=$ROOT/libs/gearmulator-md-mm/build/source/elektron/md/mdLibTest/mdProbe
if [ -n "${1:-}" ]; then DEST=$1
else
  case "$(uname -s)" in
    Darwin) DEST="$HOME/Library/Application Support/Machinemodule/bin" ;;
    MINGW*|MSYS*|CYGWIN*) DEST="$APPDATA/Machinemodule/bin" ;;
    *) DEST="${XDG_CONFIG_HOME:-$HOME/.config}/Machinemodule/bin" ;;
  esac
fi
[ -x "$PROBE" ] || "$ROOT/tools/mdtrace/build_mdprobe.sh"
# the repo's root project builds the two small helpers without JUCE (the desktop project would configure JUCE and need its dev packages)
cmake -S "$ROOT" -B "$ROOT/build-helpers" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$ROOT/build-helpers" --target md-import mdsamples
mkdir -p "$DEST"
cp "$ROOT/build-helpers/md-import" "$ROOT/build-helpers/mdsamples" "$DEST/"
cp "$PROBE" "$DEST/mdProbe"
command -v strip >/dev/null && strip "$DEST/mdProbe" "$DEST/md-import" "$DEST/mdsamples" 2>/dev/null || true
echo "import helpers installed in $DEST"
