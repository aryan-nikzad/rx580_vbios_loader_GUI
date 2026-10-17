# RX580 Large BAR DXE — experimental candidate

This directory contains an experimental 8 GiB Resizable BAR (BAR0) path for AMD RX 580 PCI ID 1002:67DF.

## Combined implementation

- [Early DXE + late loader fallback](early/README.md): buildable EDK2 source intended to request 8 GiB before resource collection, with runtime EFI logging.
- [ReBAR diagnostics](REBAR_DIAGNOSTICS.md): details how to compare the live PCIe capability chain and BAR5 mirror.

The existing loader's rebar.inc now also attempts a late ECAM-based BAR0 size request after ASIC_Init, but only if the live config space advertises the 8 GiB size. This late path does not move the BAR's base or reprogram bridge windows; use Linux pci=realloc for a controlled test when late selection succeeds.

## Verification status

**Not hardware-verified.** The module has not been built in a real EDK2 workspace or tested on the ASUS B250 MINING EXPERT. The DXE driver must be integrated into the motherboard firmware and dispatched before PCI resource collection; launching it from GRUB as an ordinary EFI application is too late for the early request.

Neither path creates a missing PCIe capability or invents unsupported-size bits. If the affected cards do not expose ReBAR until after ASIC_Init, the late request may change the selector but still depends on operating-system PCI resource reassignment. If the live post-init capability remains absent, this branch is diagnostic, not a complete hardware fix.

Do not flash altered motherboard firmware or apply speculative register writes until the module has been reviewed, built, and tested with one card and a recovery path available.
