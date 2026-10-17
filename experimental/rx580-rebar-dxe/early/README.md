# RX 580 ReBAR: early allocation + late BIOS-loader fallback

This directory contains an experimental combined path for the ASUS B250 MINING EXPERT and Polaris 10 RX 580 cards:

1. Early DXE request: Rx580ReBarEarlyDxe.c hooks the PCI host-bridge resource-allocation protocol before resource collection. If the live GPU advertises PCIe Resizable BAR capability ID 0x0015 and BAR0's size mask advertises 8 GiB, it selects size exponent 13 so firmware can allocate it at the correct time.
2. Late loader fallback: after the existing loader completes AtomBIOS ASIC_Init, rebar.inc checks the live configuration through ACPI MCFG/ECAM. If initialization has exposed a valid ReBAR capability for BAR0 and 8 GiB is explicitly supported, it changes only the BAR0 size selector and verifies readback.
3. Diagnostics: the DXE module writes a volatile runtime variable named RX580ReBarEarlyLog; tools/read_rebar_efi_log.py prints it in Linux. tools/compare_rebar.py compares the loader's 4 KiB PCI config snapshots.

## Important limit: not yet hardware-verified

Neither code path fabricates a PCIe extended capability or invents device-supported sizes. If a card does not expose ReBAR before enumeration but only exposes it after ASIC_Init, the early DXE path will log that fact and the late loader path will attempt a selector change. Since the late path runs after the motherboard allocator, Linux may have to reassign PCI resources. For testing the late path, add pci=realloc to the existing Linux kernel command line while preserving all existing parameters.

If the post-ASIC_Init ECAM snapshot still has no ReBAR capability, or its supported-size mask lacks bit 13, that is evidence of a GPU register/strap initialization difference. Do not compensate with guessed writes.

The early module does not execute AMD ASIC_Init before PCI resource allocation. Doing so requires a deeper DXE port of the loader's AtomBIOS/ROM emulation and a valid temporary BAR5/MMIO mapping before platform resources have been assigned. That is not equivalent to changing a selector, and this package does not pretend otherwise.

## Build the DXE module

This is an EDK2 DXE driver, not a standalone EFI application. It must be integrated into the motherboard's DXE volume so it dispatches before PCI bus resource collection. Launching it from GRUB is too late for the early path.

On Linux, set EDK2_ROOT to a configured Tianocore edk2 checkout and run:

    EDK2_ROOT=~/src/edk2 TOOLCHAIN=GCC5 sh experimental/rx580-rebar-dxe/early/build_in_edk2.sh

The firmware integration path varies by board. Before flashing, inspect the generated module and integrate it with UEFITool/UEFIPatch following the existing board-specific workflow. Keep a known-good full BIOS backup and an external CH341A programmer available.

## Log variable

After booting the DXE-enabled firmware, run:

    python3 tools/read_rebar_efi_log.py

A log exists only if the module dispatched and the firmware exposed runtime EFI variables. No entry for device 1002:67DF means the driver may not have dispatched at the right stage, the host protocol may not have been hooked, or the target device was not discovered through this root-bridge path.

## Loader late fallback and Linux resource reassignment

The late fallback in rebar.inc requests BAR0=8 GiB only after ASIC_Init, after reading the live ECAM configuration and confirming that the GPU advertises that size. It does not rewrite BAR0's base address or reprogram bridge windows. Verify with lspci -vv -s BB:DD.F and sudo lspci -xxxx -s BB:DD.F that PCI resource size and ReBAR control agree.

## Safety and rollback

- The early source targets only AMD device 1002:67DF and BAR0. Other devices are untouched.
- If 8 GiB is not advertised, the early driver makes no size write.
- The volatile UEFI log is diagnostic; its absence is not proof that the card lacks ReBAR.
- Do not flash the motherboard BIOS until the source is reviewed, built, and the platform's resource windows are verified. If the machine fails to POST, recover from the existing full dump with an external programmer.
- Hardware behavior is not validated in CI. First test one RX 580, not all four at once.

## Source attribution

The early host-bridge hook follows the architecture of xCuri0/ReBarUEFI, distributed under the MIT license. See LICENSE-ReBarUEFI.txt.
