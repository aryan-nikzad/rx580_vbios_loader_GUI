#!/usr/bin/env python3
"""Compare 4 KiB PCI config snapshots produced by rx580_vbios_loader_GUI."""
from pathlib import Path
import argparse
import struct
import sys

REBAR_ID = 0x0015
REBAR_8G_EXP = 13

def dword(data: bytes, offset: int) -> int:
    if offset < 0 or offset + 4 > len(data):
        return 0xFFFFFFFF
    return struct.unpack_from("<I", data, offset)[0]

def summary(label: str, data: bytes) -> list[tuple[int, int, int, int]]:
    if len(data) < 4096:
        print(f"{label}: warning: file is {len(data)} bytes (expected 4096)", file=sys.stderr)
    vendor_device = dword(data, 0)
    bars = [dword(data, 0x10 + 4 * i) for i in range(6)]
    print(f"\n[{label}] bytes={len(data)} VID:DID={vendor_device & 0xffff:04x}:{vendor_device >> 16:04x}")
    print("  BARs: " + " ".join(f"BAR{i}={value:08x}" for i, value in enumerate(bars)))
    if len(data) < 4096 or dword(data, 0x100) in (0, 0xffffffff):
        print("  Extended config/capability chain unavailable in this snapshot.")
        return []

    found = []
    off = 0x100
    visited = set()
    for _ in range(64):
        if off in visited or off < 0x100 or off > 0xffc:
            print(f"  Invalid/cyclic extended-capability pointer at 0x{off:03x}")
            break
        visited.add(off)
        header = dword(data, off)
        if header in (0, 0xffffffff):
            break
        cap_id = header & 0xffff
        nxt = (header >> 20) & 0xffc
        print(f"  extcap @0x{off:03x}: ID=0x{cap_id:04x} version={(header >> 16) & 0xf} next=0x{nxt:03x}")
        if cap_id == REBAR_ID:
            first_ctrl = dword(data, off + 8)
            count = (first_ctrl >> 5) & 7
            count = max(1, min(count, 6))
            for i in range(count):
                cap = dword(data, off + 4 + 8 * i)
                ctl = dword(data, off + 8 + 8 * i)
                bar = ctl & 7
                mask = (cap >> 4) & 0xfffff
                size_exp = (ctl >> 8) & 0x1f
                supports_8g = bool(mask & (1 << REBAR_8G_EXP))
                current_gib = (2 ** size_exp) / 1024
                print(
                    f"    BAR{bar}: cap={cap:08x} ctrl={ctl:08x} supported-mask={mask:05x} "
                    f"current=2^{size_exp} MiB ({current_gib:g} GiB) supports-8GiB={supports_8g}"
                )
                found.append((off + 4 + 8 * i, bar, mask, size_exp))
        if not nxt or nxt <= off:
            break
        off = nxt
    if not found:
        print("  ReBAR capability absent in this snapshot.")
    return found

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--good", required=True, type=Path, help="healthy-card cfg_...bin snapshot")
    parser.add_argument("--bad", required=True, type=Path, help="affected-card cfg_...bin snapshot")
    args = parser.parse_args()
    good = args.good.read_bytes()
    bad = args.bad.read_bytes()
    summary("HEALTHY", good)
    summary("AFFECTED", bad)
    print("\n[Comparison]")
    print(f"  snapshot lengths: healthy={len(good)} affected={len(bad)}")
    common = min(len(good), len(bad))
    differing = sum(dword(good, off) != dword(bad, off) for off in range(0, common - 3, 4))
    print(f"  differing dwords: {differing}")
    print("  Compare BAR0 supported-mask and selected size. A size change after UEFI resource")
    print("  allocation may still require Linux PCI resource reallocation (pci=realloc).")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
