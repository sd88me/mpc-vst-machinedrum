#!/bin/sh
# ADVANCED: patches the factory /usr/bin/MPC (MPC OS 3.9.1.2 only) so Machinedrum Module gets the 16-pad drum layout.
# Run ON the device as root:  sh install.sh        (undo: sh uninstall.sh)
# Touches the factory OS. A firmware update replaces the file and removes the patch. Nothing Akai's is shipped here:
# the patch file holds only our bytes and md5s, applied to your own copy.
set -u
HERE=$(cd "$(dirname "$0")" && pwd); PATCH=$HERE/mpc-3.9.1.2.patch
BK=/sdcard/MPC-backup; MNT=/tmp/mdroot; MODE=${1:-install}
die() { echo "ERROR: $*"; exit 1; }
md5() { md5sum "$1" | cut -d' ' -f1; }
STOCK=$(awk '$1=="stock_md5"{print $2}' "$PATCH"); PATCHED=$(awk '$1=="patched_md5"{print $2}' "$PATCH")
[ -n "$STOCK" ] && [ -n "$PATCHED" ] || die "bad patch file"
# the factory file: through a bind mount of the real root, so a MockbaMod overlay on /usr is bypassed
mkdir -p $MNT; mountpoint -q $MNT 2>/dev/null || mount --bind / $MNT || die "bind mount"
F=$MNT/usr/bin/MPC
for u in /media/*/system/usr/overlay /media/*/*/system/usr/overlay; do
  [ -e "$u/bin/MPC" ] && die "an MPC exists in the MockbaMod overlay ($u/bin/MPC); remove it first"; done
CUR=$(md5 $F)
REG=$BK/orig-regions.txt
apply() { # $1 = new: write the patch; orig: write back the originals saved in $REG
  if [ "$1" = new ]; then L=$(grep -v '^[#sp]' "$PATCH"); else [ -f "$REG" ] || return 1; L=$(cat "$REG"); fi
  echo "$L" | while read o h; do
    { while [ -n "$h" ]; do r=${h#??}; c=${h%"$r"}; printf "\\$(printf %03o $((0x$c)))"; h=$r; done; } | dd of=$F bs=1 seek=$((0x$o)) conv=notrunc 2>/dev/null; done; }
save_orig() { : > $REG; grep -v '^[#sp]' "$PATCH" | while read o h; do
    echo "$o $(dd if=$F bs=1 skip=$((0x$o)) count=$((${#h}/2)) 2>/dev/null | od -An -tx1 -v | tr -d ' \n')" >> $REG; done; }
finish() { sync; mount -o remount,ro /; systemctl start acvs; sleep 5; pidof MPC >/dev/null && echo "MPC is running" || echo "WARNING: MPC not running; run uninstall.sh"; }
if [ "$MODE" = uninstall ]; then
  [ "$CUR" = "$STOCK" ] && { echo "already stock"; exit 0; }
  [ "$CUR" = "$PATCHED" ] || die "MPC is neither stock nor our patched build (md5 $CUR); not touching it"
  systemctl stop acvs; mount -o remount,rw / || die "remount rw"
  apply orig; [ "$(md5 $F)" = "$STOCK" ] || { echo "restoring from full backup"; cat $BK/MPC-3.9.1.2.orig > $F; }
  [ "$(md5 $F)" = "$STOCK" ] && echo "restored stock MPC" || echo "ERROR: md5 not stock; copy $BK/MPC-3.9.1.2.orig over /usr/bin/MPC"
  finish; exit 0
fi
[ "$CUR" = "$PATCHED" ] && { echo "already patched"; exit 0; }
[ "$CUR" = "$STOCK" ] || die "this is not MPC OS 3.9.1.2 (md5 $CUR). Refusing; nothing changed."
echo "This modifies the FACTORY MPC OS (/usr/bin/MPC) on this device, for MPC OS 3.9.1.2 only."
echo "A firmware update replaces it and removes the patch. Undo with: sh uninstall.sh"
printf "Type PATCH to continue: "; read a; [ "$a" = PATCH ] || die "cancelled"
mkdir -p $BK && cat $F > $BK/MPC-3.9.1.2.orig && [ "$(md5 $BK/MPC-3.9.1.2.orig)" = "$STOCK" ] || die "backup to $BK failed"
save_orig; [ -s $REG ] || die "could not save original bytes"
echo "backup: $BK/MPC-3.9.1.2.orig and $REG"
systemctl stop acvs; mount -o remount,rw / || { systemctl start acvs; die "remount rw"; }
apply new
if [ "$(md5 $F)" = "$PATCHED" ]; then echo "patched OK"; else
  echo "ERROR: md5 mismatch after patch; restoring"; apply orig
  [ "$(md5 $F)" = "$STOCK" ] || cat $BK/MPC-3.9.1.2.orig > $F
  echo "restored md5 $(md5 $F)"; fi
finish
