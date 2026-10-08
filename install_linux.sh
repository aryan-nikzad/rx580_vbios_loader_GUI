#!/bin/sh
# Installs the RX 580 loader on the EFI System Partition (\EFI\vbios_loader) and puts it FIRST in the UEFI boot order.
# Run from the unzipped folder:   sudo sh install_linux.sh
# An existing vbios_loader.cfg on the EFI partition is never overwritten.
set -e
[ "$(id -u)" = 0 ] || { echo "Run with sudo:  sudo sh install_linux.sh"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
ESP=${ESP:-/boot/efi}
DEST="$ESP/EFI/vbios_loader"
[ -f "$HERE/vbios_loader.efi" ] && [ -d "$HERE/vbioses" ] || { echo "Build vbios_loader.efi and create a vbioses/ directory containing your own ROM dumps first"; exit 1; }
mountpoint -q "$ESP" || { echo "EFI partition is not mounted at $ESP (set ESP=/path if it is elsewhere)"; exit 1; }

mkdir -p "$DEST"
cp "$HERE/vbios_loader.efi" "$DEST/"
rm -rf "$DEST/vbioses"; cp -r "$HERE/vbioses" "$DEST/vbioses"
if [ -f "$DEST/vbios_loader.cfg" ]; then
  echo "Keeping your existing $DEST/vbios_loader.cfg"
elif [ -f "$HERE/vbios_loader.cfg" ]; then
  cp "$HERE/vbios_loader.cfg" "$DEST/vbios_loader.cfg"; echo "Installed your vbios_loader.cfg"
elif [ -f "$HERE/vbios_loader.cfg.example" ]; then
  cp "$HERE/vbios_loader.cfg.example" "$DEST/vbios_loader.cfg"; echo "Created $DEST/vbios_loader.cfg from the example (edit it any time)"
fi
sync
echo "Copied to $DEST ($(ls "$DEST/vbioses" | wc -l) ROM files)"
[ -d "$ESP/EFI/vbios" ] && echo "NOTE: the old beta folder $ESP/EFI/vbios is no longer used; you can delete it."

SRC=$(findmnt -n -o SOURCE "$ESP")                       # e.g. /dev/nvme0n1p1 or /dev/sda1
NAME=$(basename "$SRC")
PART=$(cat "/sys/class/block/$NAME/partition")
DISK="/dev/$(basename "$(readlink -f "/sys/class/block/$NAME/..")")"
echo "EFI partition: $SRC  (disk $DISK, partition $PART)"

# remove an older entry of ours, then create a new one
for n in $(efibootmgr | awk '/RX580 loader/ {gsub(/Boot|\*/,"",$1); print $1}'); do efibootmgr -q -B -b "$n"; done
efibootmgr -q --create --disk "$DISK" --part "$PART" --label "RX580 loader" --loader '\EFI\vbios_loader\vbios_loader.efi'
NEW=$(efibootmgr | awk '/RX580 loader/ {gsub(/Boot|\*/,"",$1); print $1; exit}')
CUR=$(efibootmgr | awk -F': ' '/^BootOrder/ {print $2}' | tr -d ' ')
REST=$(echo "$CUR" | tr ',' '\n' | grep -v "^$NEW$" | grep -v '^$' | paste -sd, -)
efibootmgr -q -o "$NEW${REST:+,$REST}"
echo; efibootmgr | head -8
echo; echo "Done. Reboot: the status page shows your cards, AUTO starts by itself after the delay (press S there for settings), then your normal OS boots."
