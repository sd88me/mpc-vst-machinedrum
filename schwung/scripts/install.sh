#!/usr/bin/env bash
# Installs dist/machinemodule onto a Move over ssh (Schwung's sound_generators folder), keeping the user's uploads (os/, factory/).
# A running instance keeps the old dsp.so mapped: restart Move (or remove and re-add the module) to load the new one.
#   schwung/scripts/install.sh [user@host]      default ableton@move.local
set -euo pipefail
S=$(cd "$(dirname "$0")/.." && pwd)
DEV=${1:-ableton@move.local}
DST=/data/UserData/schwung/modules/sound_generators/machinemodule
[ -f "$S/dist/machinemodule/dsp.so" ] || { echo "build first: schwung/scripts/build.sh" >&2; exit 1; }
ssh "$DEV" "mkdir -p $DST/os $DST/flash $DST/kits $DST/factory $DST/bin"
scp -q "$S/dist/machinemodule/bin/"* "$DEV:$DST/bin/"
scp -q "$S/dist/machinemodule/help.json" "$DEV:$DST/help.json"
scp -q "$S/dist/machinemodule/module.json" "$S/dist/machinemodule/dsp.so" "$S/dist/machinemodule/md-engine" "$DEV:/data/UserData/"
ssh "$DEV" "cd /data/UserData && mv module.json $DST/module.json && mv dsp.so $DST/dsp.so.new && mv $DST/dsp.so.new $DST/dsp.so && mv md-engine $DST/md-engine.new && mv $DST/md-engine.new $DST/md-engine && chmod 755 $DST/dsp.so $DST/md-engine && ls -la $DST"
echo "==> installed in $DEV:$DST (restart Move, or remove and re-add the module, to load a new dsp.so)"
