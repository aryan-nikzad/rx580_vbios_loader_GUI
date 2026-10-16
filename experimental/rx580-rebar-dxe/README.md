# RX580 Large BAR DXE — review candidate

This branch adds a ZIP package containing a candidate companion UEFI DXE driver intended to request an 8 GiB Resizable BAR (BAR0) for AMD RX 580 PCI ID 1002:67DF before PCI resource collection.

## Contents

- `rx580_rebar_dxe_review_package.zip`: source, EDK2 INF/DSC files, and implementation notes.

## Verification status

**Review candidate only; not verified for hardware use.** The package has not been compiled in a real EDK2 workspace or tested on the motherboard. It must be integrated into the motherboard firmware and dispatched before PCI bus enumeration; launching it from GRUB as an ordinary EFI application is too late to affect the initial resource allocation. The driver checks the device's advertised supported-size mask before requesting 8 GiB, but successful allocation also depends on firmware resource windows and bridge allocation.

Do not flash altered motherboard firmware based solely on this candidate. Review and build it in an EDK2 workspace first.

No changes have been merged into `main`.
