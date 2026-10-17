#!/bin/sh
set -eu

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
EDK2_ROOT=${EDK2_ROOT:-}
TOOLCHAIN=${TOOLCHAIN:-GCC5}
TARGET=${TARGET:-DEBUG}

if [ -z "$EDK2_ROOT" ] || [ ! -f "$EDK2_ROOT/edksetup.sh" ]; then
  echo "Set EDK2_ROOT to a configured Tianocore edk2 checkout." >&2
  echo "Example: EDK2_ROOT=~/src/edk2 TOOLCHAIN=GCC5 $0" >&2
  exit 2
fi

cd "$EDK2_ROOT"
export PACKAGES_PATH="$EDK2_ROOT:$REPO_ROOT${PACKAGES_PATH:+:$PACKAGES_PATH}"
export WORKSPACE="$EDK2_ROOT"
export PYTHON_COMMAND=python3
. ./edksetup.sh
build -p "$REPO_ROOT/experimental/rx580-rebar-dxe/early/Rx580ReBarEarly.dsc" -a X64 -t "$TOOLCHAIN" -b "$TARGET"

echo
echo "Build command completed. Check Build/Rx580ReBarEarly/${TARGET}_${TOOLCHAIN}/X64/ for the DXE output."
