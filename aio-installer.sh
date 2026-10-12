#!/usr/bin/env bash
# AIO installer: dependencies, companion ROMs, patched kernel, EFI build/install.
set -Eeuo pipefail
ROOT="$(cd -- "$(dirname -- "$0")" && pwd)"
WORKDIR="$ROOT/.aio-work"
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc)"
DO_KERNEL=1
DO_EFI=1
MANUAL=0
DRY_RUN=0
if [ -n "$(printenv AIO_WORKDIR 2>/dev/null || true)" ]; then WORKDIR="$(printenv AIO_WORKDIR)"; fi
usage() { cat <<'EOF'
Usage: sudo bash ./aio-installer.sh [--efi-only|--kernel-only] [--manual] [--dry-run] [--workdir PATH] [--jobs N]
Default: install dependencies, clone/update ../rx580_dumps, copy ROMs without overwriting existing files,
build/install a patched kernel from the exact installed Ubuntu kernel source, build EFI, then run install_linux.sh.
--efi-only     Skip kernel build/install.
--kernel-only  Skip EFI build/install.
--manual       Ask before major system changes.
--dry-run      Print intended commands without changing the system.
--workdir PATH Kernel source/build work directory.
--jobs N       Parallel build jobs.
EOF
}
log() { printf '\n[AIO] %s\n' "$*"; }
die() { printf '\n[AIO] ERROR: %s\n' "$*" >&2; exit 1; }
run() { if [ "$DRY_RUN" = 1 ]; then printf '[DRY-RUN]'; printf ' %q' "$@"; printf '\n'; else "$@"; fi; }
confirm() { if [ "$MANUAL" = 1 ]; then local a; read -r -p "$1 [y/N] " a; case "$a" in y|Y|yes|YES) ;; *) die "Cancelled."; esac; fi; }
while [ "$#" -gt 0 ]; do
  case "$1" in
    --efi-only) DO_KERNEL=0 ;;
    --kernel-only) DO_EFI=0 ;;
    --manual) MANUAL=1 ;;
    --dry-run) DRY_RUN=1 ;;
    --workdir) [ "$#" -ge 2 ] || die "--workdir needs a path"; WORKDIR="$2"; shift ;;
    --jobs) [ "$#" -ge 2 ] || die "--jobs needs a number"; JOBS="$2"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "Unknown option: $1" ;;
  esac
  shift
done
case "$JOBS" in ''|*[!0-9]*|0) die "--jobs must be a positive integer." ;; esac
[ "$(id -u)" -eq 0 ] || die "Run with sudo: sudo bash ./aio-installer.sh"
[ -f "$ROOT/Makefile" ] && [ -f "$ROOT/install_linux.sh" ] || die "Makefile/install_linux.sh not found; run from the project."
[ -f "$ROOT/patches/linux-amdgpu-vfct-exact-bdf.patch" ] || die "Kernel patch missing."

log "Installing all build dependencies through apt."
confirm "Run apt-get update and install dependencies?"
run apt-get update
run env DEBIAN_FRONTEND=noninteractive apt-get install -y build-essential gcc g++ make binutils gnu-efi efibootmgr git patch bc bison flex libssl-dev libelf-dev libncurses-dev dwarves pahole fakeroot dpkg-dev debhelper devscripts cpio kmod rsync curl ca-certificates python3 perl xz-utils zstd lz4

DUMPS_DIR="$(dirname "$ROOT")/rx580_dumps"
log "Fetching ROM repository to sibling directory: $DUMPS_DIR"
if [ -d "$DUMPS_DIR/.git" ]; then
  run git -C "$DUMPS_DIR" pull --ff-only
elif [ -e "$DUMPS_DIR" ]; then
  die "$DUMPS_DIR exists but is not a Git repository; move it or fix it first."
else
  run git clone --depth 1 https://github.com/aryan-nikzad/rx580_dumps.git "$DUMPS_DIR"
fi
if [ "$DRY_RUN" != 1 ]; then
  [ -d "$DUMPS_DIR/vbioses" ] || die "No vbioses directory in rx580_dumps."
  mkdir -p "$ROOT/vbioses"
  rsync -av --ignore-existing --include='*.rom' --include='*/' --exclude='*' "$DUMPS_DIR/vbioses/" "$ROOT/vbioses/"
  ROMCOUNT="$(find "$ROOT/vbioses" -maxdepth 1 -type f -iname '*.rom' | wc -l)"
  [ "$ROMCOUNT" -gt 0 ] || die "No .rom files found in project vbioses/."
  log "$ROMCOUNT ROM file(s) available; existing ROM files were not overwritten."
fi

enable_deb_src() {
  local f changed=0
  mkdir -p "$WORKDIR"
  shopt -s nullglob
  for f in /etc/apt/sources.list.d/*.sources; do
    if grep -qE '^Types:[[:space:]]*deb([[:space:]]|$)' "$f" && ! grep -qE '^Types:.*deb-src' "$f"; then
      confirm "Enable deb-src in $f to retrieve exact kernel source?"
      if [ "$DRY_RUN" != 1 ]; then
        cp -an "$f" "$f.aio-backup"
        sed -i -E '/^Types:[[:space:]]*deb([[:space:]]|$)/ s/deb([[:space:]]|$)/deb deb-src\1/' "$f"
      fi
      changed=1
    fi
  done
  if [ -f /etc/apt/sources.list ] && grep -qE '^[[:space:]]*deb[[:space:]]' /etc/apt/sources.list && ! grep -qE '^[[:space:]]*deb-src[[:space:]]' /etc/apt/sources.list; then
    confirm "Add deb-src entries from /etc/apt/sources.list?"
    if [ "$DRY_RUN" != 1 ]; then
      cp -an /etc/apt/sources.list /etc/apt/sources.list.aio-backup
      sed -E 's/^([[:space:]]*)deb[[:space:]]/\1deb-src /' /etc/apt/sources.list | grep -E '^[[:space:]]*deb-src[[:space:]]' > "$WORKDIR/aio-deb-src.list" || true
      if [ -s "$WORKDIR/aio-deb-src.list" ]; then install -m 0644 "$WORKDIR/aio-deb-src.list" /etc/apt/sources.list.d/aio-deb-src.list; changed=1; fi
    fi
  fi
  shopt -u nullglob
  if [ "$changed" = 1 ] && [ "$DRY_RUN" != 1 ]; then apt-get update; fi
}

if [ "$DO_KERNEL" = 1 ]; then
  RUNNING="$(uname -r)"
  IMAGE_PKG="linux-image-$RUNNING"
  dpkg-query -W "$IMAGE_PKG" >/dev/null 2>&1 || die "Cannot identify installed package $IMAGE_PKG; refusing to guess source version."
  SRC_INFO="$(dpkg-query -W -f='$''{source:Package} $''{source:Version}' "$IMAGE_PKG" 2>/dev/null || true)"
  SRC_PKG="$(printf '%s\n' "$SRC_INFO" | awk '{print $1}')"
  SRC_VER="$(printf '%s\n' "$SRC_INFO" | awk '{print $2}')"
  [ -n "$SRC_PKG" ] && [ -n "$SRC_VER" ] && [ "$SRC_VER" != "(none)" ] || die "Could not determine exact source package/version for $IMAGE_PKG."
  log "Kernel $RUNNING uses source $SRC_PKG=$SRC_VER."
  enable_deb_src
  run apt-get update
  confirm "Download/build patched kernel source $SRC_PKG=$SRC_VER? This may take hours and needs substantial free disk space."
  mkdir -p "$WORKDIR/kernel-source"
  if [ "$DRY_RUN" = 1 ]; then
    run apt-get source "$SRC_PKG=$SRC_VER"
  else
    (cd "$WORKDIR/kernel-source" && apt-get source "$SRC_PKG=$SRC_VER") || die "Could not download exact source. No kernel was installed."
    SRC_DIR="$(find "$WORKDIR/kernel-source" -mindepth 1 -type f -path '*/drivers/gpu/drm/amd/amdgpu/amdgpu_bios.c' -printf '%h\n' | sed 's#/drivers/gpu/drm/amd/amdgpu$##' | head -n 1)"
    [ -n "$SRC_DIR" ] && [ -f "$SRC_DIR/Makefile" ] || die "Cannot locate extracted kernel source."
    [ -r "/boot/config-$RUNNING" ] || die "Missing /boot/config-$RUNNING."
    [ -x "$SRC_DIR/scripts/config" ] || die "Kernel source has no scripts/config."
    patch --dry-run -s -p1 -d "$SRC_DIR" < "$ROOT/patches/linux-amdgpu-vfct-exact-bdf.patch" || die "VFCT patch does not apply cleanly to exact kernel source $SRC_PKG=$SRC_VER. Stopping safely."
    confirm "Apply patch and build kernel; current kernel remains installed as fallback?"
    patch -s -p1 -d "$SRC_DIR" < "$ROOT/patches/linux-amdgpu-vfct-exact-bdf.patch"
    cp "/boot/config-$RUNNING" "$SRC_DIR/.config"
    "$SRC_DIR/scripts/config" --file "$SRC_DIR/.config" --disable SYSTEM_TRUSTED_KEYS
    "$SRC_DIR/scripts/config" --file "$SRC_DIR/.config" --disable SYSTEM_REVOCATION_KEYS
    "$SRC_DIR/scripts/config" --file "$SRC_DIR/.config" --disable DEBUG_INFO
    make -s -C "$SRC_DIR" olddefconfig
    NEWREL="$(make -s -C "$SRC_DIR" LOCALVERSION=-vfctfix kernelrelease)"
    [ "$NEWREL" != "$RUNNING" ] || die "New kernel release equals current kernel; refusing to overwrite."
    log "Compiling kernel $NEWREL with $JOBS jobs."
    make -C "$SRC_DIR" -j"$JOBS" LOCALVERSION=-vfctfix KDEB_PKGVERSION="$SRC_VER+vfctfix1" bindeb-pkg
    PARENT="$(dirname "$SRC_DIR")"
    IMAGE_DEB="$(find "$PARENT" -maxdepth 1 -type f \( -name "linux-image-unsigned-$NEWREL""_*.deb" -o -name "linux-image-$NEWREL""_*.deb" \) -print -quit)"
    HEADER_DEB="$(find "$PARENT" -maxdepth 1 -type f -name "linux-headers-$NEWREL""_*.deb" -print -quit)"
    [ -n "$IMAGE_DEB" ] && [ -n "$HEADER_DEB" ] || die "Build finished but expected image/header packages were not found; inspect $PARENT."
    confirm "Install new kernel packages? Existing kernel $RUNNING will not be removed."
    dpkg -i "$IMAGE_DEB" "$HEADER_DEB"
    [ -e "/boot/vmlinuz-$NEWREL" ] || die "Kernel package did not install /boot/vmlinuz-$NEWREL."
    update-initramfs -c -k "$NEWREL" 2>/dev/null || update-initramfs -u -k "$NEWREL"
    update-grub
    log "Patched kernel $NEWREL installed. No reboot performed; select it from GRUB Advanced options for first test."
  fi
fi

if [ "$DO_EFI" = 1 ]; then
  log "Building EFI loader."
  confirm "Clean and compile vbios_loader.efi?"
  run make -C "$ROOT" clean
  run make -C "$ROOT" -j"$JOBS"
  if [ "$DRY_RUN" != 1 ]; then
    [ -s "$ROOT/vbios_loader.efi" ] || die "EFI build did not produce vbios_loader.efi."
    confirm "Install EFI loader? install_linux.sh updates UEFI boot order to start the loader first."
    (cd "$ROOT" && bash ./install_linux.sh)
  else
    run bash "$ROOT/install_linux.sh"
  fi
fi
log "Completed. The system was not rebooted automatically."
