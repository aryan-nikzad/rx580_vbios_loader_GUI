# RX 580 ReBAR: early allocation + late BIOS-loader fallback

This directory contains an experimental combined path for the ASUS B250 MINING EXPERT and Polaris 10 RX 580 cards:

1. Forced early DXE request: the driver is dispatched immediately before the motherboard's PCI bus driver (`PciBus`) and hooks the PCI host-bridge resource-allocation protocol before resource collection. For AMD `1002:67DF`, it requires a real PCIe Resizable BAR extended capability ID `0x0015` and a BAR0 entry, then requests size exponent 13 (8 GiB) **even when BAR0's advertised-size mask does not include 8 GiB**. This is intentionally out-of-spec experimentation.
2. Optional late loader fallback: after AtomBIOS ASIC_Init, rebar.inc can inspect the live configuration through ACPI MCFG/ECAM. This write is disabled by default. Set rebar_late=1 in EFI/vbios_loader/vbios_loader.cfg only for a controlled Linux test with pci=realloc already added to the kernel command line. If the live capability advertises 8 GiB for BAR0, it changes only the selector and verifies readback.
3. Diagnostics: the DXE module writes a volatile runtime variable named RX580ReBarEarlyLog; tools/read_rebar_efi_log.py prints it in Linux. tools/compare_rebar.py compares the loader's 4 KiB PCI config snapshots.

## Important limit: not yet hardware-verified

The forced early path does not fabricate a missing PCIe extended capability or BAR0 entry, and it does not run ASIC_Init. If ReBAR only appears after ASIC_Init, the early driver logs that it cannot find the capability and cannot apply the force. If the capability is present but its mask omits 8 GiB, the driver still attempts selector exponent 13; a successful readback does not prove the hardware can operate at that size. The optional late loader path remains guarded by the advertised-size mask and off by default; set `rebar_late=1` only for a controlled Linux test with `pci=realloc` already added to the existing kernel command line.

If the post-ASIC_Init ECAM snapshot still has no ReBAR capability, or its supported-size mask lacks bit 13, that is evidence of a GPU register/strap initialization difference. Do not compensate with guessed writes.

The early module does not execute AMD ASIC_Init before PCI resource allocation. Doing so requires a deeper DXE port of the loader's AtomBIOS/ROM emulation and a valid temporary BAR5/MMIO mapping before platform resources have been assigned. That is not equivalent to changing a selector, and this package does not pretend otherwise.

## Build the DXE module

This is an EDK2 DXE driver, not a standalone EFI application. It must be integrated into the same motherboard DXE firmware volume as the PCI bus driver. Its dependency expression uses `BEFORE gEfiPciBusDriverFileGuid` (file GUID `3C1DE39F-D207-408A-AACC-731CFB7F1DD7`) so the dispatcher places it immediately before PciBus once PciBus is ready to dispatch. Launching it from GRUB is too late for the early path.

On Linux, set EDK2_ROOT to a configured Tianocore edk2 checkout and run:

    EDK2_ROOT=~/src/edk2 TOOLCHAIN=GCC5 sh experimental/rx580-rebar-dxe/early/build_in_edk2.sh

The CI workflow packages the compiled driver as an FFS artifact named `rx580-rebar-force-8gb-dxe-ffs`. That FFS is a driver payload, not a complete motherboard BIOS; integrate it into the DXE volume with UEFITool/UEFIPatch and confirm it dispatches before PCI resource collection. The firmware integration path varies by board. Before flashing, inspect the generated module and integrate it with UEFITool/UEFIPatch following the existing board-specific workflow. Keep a known-good full BIOS backup and an external CH341A programmer available.

## Log variable

After booting the DXE-enabled firmware, run:

    python3 tools/read_rebar_efi_log.py

A log exists only if the module dispatched and the firmware exposed runtime EFI variables. No entry for device 1002:67DF means the driver may not have dispatched at the right stage, the host protocol may not have been hooked, or the target device was not discovered through this root-bridge path.

## Loader late fallback and Linux resource reassignment

When rebar_late=1 is explicitly configured, the late fallback in rebar.inc requests BAR0=8 GiB only after ASIC_Init, after reading the live ECAM configuration and confirming that the GPU advertises that size. It does not rewrite BAR0's base address or reprogram bridge windows. Verify with lspci -vv -s BB:DD.F and sudo lspci -xxxx -s BB:DD.F that PCI resource size and ReBAR control agree.

## Safety and rollback

- The early source targets only AMD device 1002:67DF and BAR0. Other devices are untouched.
- The early forced driver ignores the supported-size mask. If 8 GiB is not advertised, this unsupported selector request may fail, hang the GPU, hang boot, or prevent POST.
- The volatile UEFI log is diagnostic; its absence is not proof that the card lacks ReBAR.
- Treat this forced build as higher risk than normal ReBARUEFI: unsupported BAR-size selection can cause a GPU lockup, boot hang, or failure to POST. Keep the known-good full SPI dump and a working CH341A/external programmer ready before considering a flash. Inspect the FFS and integrated BIOS, and test with one affected RX 580 only.
- Hardware behavior is not validated in CI. First test one RX 580, not all four at once.

## Source attribution

The early host-bridge hook follows the architecture of xCuri0/ReBarUEFI, distributed under the MIT license. See LICENSE-ReBarUEFI.txt.
