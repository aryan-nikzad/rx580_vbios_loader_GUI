#!/usr/bin/env python3
"""Print the volatile UEFI diagnostic variable created by Rx580ReBarEarlyDxe."""
from pathlib import Path
import sys

EFIVARS = Path("/sys/firmware/efi/efivars")
PATTERN = "RX580ReBarEarlyLog-*"

def main() -> int:
    matches = sorted(EFIVARS.glob(PATTERN))
    if not matches:
        print(
            "No RX580ReBarEarlyLog variable found. The DXE driver may not have run, "
            "runtime EFI variables may be unavailable, or this boot used legacy mode.",
            file=sys.stderr,
        )
        return 1

    path = matches[-1]
    data = path.read_bytes()
    if len(data) < 4:
        print(f"Variable is truncated: {path}", file=sys.stderr)
        return 2

    # efivarfs prepends the 4-byte EFI variable attributes field.
    print(data[4:].decode("utf-16-le", errors="replace").rstrip("\x00"))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
