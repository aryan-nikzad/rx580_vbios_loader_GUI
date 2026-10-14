#!/bin/sh
# Install the RX580 UEFI loader and integrate it into GRUB.
# Run from the extracted project directory: sudo sh install_linux.sh
# Existing vbios_loader.cfg on the EFI partition is never overwritten.
set -eu

[ "$(id -u)" = 0 ] || { echo "Run with sudo: sudo sh install_linux.sh"; exit 1; }

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ESP=${ESP:-/boot/efi}
DEST="$ESP/EFI/vbios_loader"
GRUB_HOOK=/etc/grub.d/06_rx580_vbios_loader
MARKER=RX580_VBIOS_LOADER_MANAGED

[ -f "$HERE/vbios_loader.efi" ] && [ -d "$HERE/vbioses" ] || {
  echo "Build vbios_loader.efi and create vbioses/ containing your ROM dumps first."
  exit 1
}
mountpoint -q "$ESP" || {
  echo "EFI partition is not mounted at $ESP (set ESP=/path if it is elsewhere)."
  exit 1
}
command -v update-grub >/dev/null 2>&1 || {
  echo "GRUB's update-grub command was not found. This installer requires GRUB."
  exit 1
}
command -v blkid >/dev/null 2>&1 || { echo "blkid is required."; exit 1; }

SRC=$(findmnt -n -o SOURCE --target "$ESP")
ESP_UUID=$(blkid -s UUID -o value "$SRC")
[ -n "$ESP_UUID" ] || {
  echo "Could not determine the EFI filesystem UUID for $SRC."
  exit 1
}

if [ -e "$GRUB_HOOK" ] && ! grep -q "$MARKER" "$GRUB_HOOK"; then
  echo "Refusing to overwrite existing $GRUB_HOOK because it is not marked as managed by this installer."
  exit 1
fi

mkdir -p "$DEST"
cp "$HERE/vbios_loader.efi" "$DEST/"
rm -rf "$DEST/vbioses"
cp -r "$HERE/vbioses" "$DEST/vbioses"

if [ -f "$DEST/vbios_loader.cfg" ]; then
  echo "Keeping your existing $DEST/vbios_loader.cfg"
elif [ -f "$HERE/vbios_loader.cfg" ]; then
  cp "$HERE/vbios_loader.cfg" "$DEST/vbios_loader.cfg"
  echo "Installed your vbios_loader.cfg"
elif [ -f "$HERE/vbios_loader.cfg.example" ]; then
  cp "$HERE/vbios_loader.cfg.example" "$DEST/vbios_loader.cfg"
  echo "Created $DEST/vbios_loader.cfg from the example"
fi

# The GRUB entry runs the EFI app first. If the app returns to GRUB, a
# runtime-only flag prevents the entry from being offered again in the
# reloaded menu; GRUB then continues to the normal default OS entry.
TMP_HOOK=$(mktemp)
cat > "$TMP_HOOK" <<EOF
#!/bin/sh
# $MARKER
cat <<'RX580_GRUB_EOF'
if [ "\${rx580_loader_ran}" = "1" ]; then
    set timeout=3
    set timeout_style=menu
fi

if [ "\${rx580_loader_ran}" != "1" ]; then
menuentry 'RX580 vBIOS Loader (run before Linux)' --id rx580-vbios-loader {
    insmod part_gpt
    insmod fat
    insmod chain
    search --no-floppy --fs-uuid --set=esp $ESP_UUID
    chainloader (\$esp)/EFI/vbios_loader/vbios_loader.efi
    boot
    set rx580_loader_ran=1
    configfile \$prefix/grub.cfg
}
fi
RX580_GRUB_EOF
EOF
chmod 755 "$TMP_HOOK"
mv "$TMP_HOOK" "$GRUB_HOOK"

if ! update-grub; then
  echo "ERROR: update-grub failed. The GRUB hook remains at $GRUB_HOOK; remove it and run update-grub to roll back."
  exit 1
fi

# Remove only the old NVRAM entry created by the previous installer version.
# GRUB is now responsible for launching the loader; do not put it before GRUB
# in UEFI BootOrder, which returns control to motherboard firmware after exit.
if command -v efibootmgr >/dev/null 2>&1; then
  for n in $(efibootmgr 2>/dev/null | awk '/RX580 loader$/ {gsub(/Boot|\*/, "", $1); print $1}'); do
    efibootmgr -q -B -b "$n" || true
  done
fi

sync
echo
echo "Installed RX580 loader files at $DEST"
echo "Added the RX580 loader as the first GRUB menu entry."
echo "With GRUB_DEFAULT=0, it runs automatically; after it exits, GRUB reloads"
echo "the menu without repeating the loader and continues to the normal default OS."
echo "If GRUB_DEFAULT=saved, select the RX580 entry manually unless your saved"
echo "default is configured to use it."
echo "To undo the GRUB integration: sudo rm -f $GRUB_HOOK && sudo update-grub"
