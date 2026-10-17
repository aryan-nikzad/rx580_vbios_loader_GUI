# RX580 Large BAR DXE — review candidate

This directory contains an experimental companion UEFI DXE-driver package intended to request an 8 GiB Resizable BAR (BAR0) for AMD RX 580 PCI ID 1002:67DF before PCI resource collection.

## Contents

- `rx580_rebar_dxe_review_package.zip`: source, EDK2 INF/DSC files, and implementation notes.
- [ReBAR diagnostics and safe next steps](REBAR_DIAGNOSTICS.md): explains the new BAR5-mirror probe, what it can establish, and why guessed register writes are not enabled by default.

## Verification status

**Review candidate only; not verified for hardware use.** The package has not been compiled in a real EDK2 workspace or tested on the motherboard. It must be integrated into the motherboard firmware and dispatched before PCI bus enumeration; launching it from GRUB as an ordinary EFI application is too late to affect the initial resource allocation. The driver checks the device's advertised supported-size mask before requesting 8 GiB, but successful allocation also depends on firmware resource windows and bridge allocation.

The `fix/rebar-mirror-diagnostics` branch adds diagnostic reads of the GPU's BAR5 config-space mirror after ASIC initialization. These diagnostics do not create a missing PCIe extended capability or change resource allocation. Use the mirror dump from a healthy card and an affected card to establish whether the capability is actually absent in the GPU-visible mirror.

Do not flash altered motherboard firmware or apply speculative register writes based solely on this candidate. Review and build it in an EDK2 workspace first. If the device genuinely does not advertise ReBAR, a DXE driver that only requests a larger advertised size is not sufficient.

No changes have been merged into `main`.
