# ReBAR diagnostics and safe next steps

This branch adds a post-ASIC_Init diagnostic read of the Polaris BAR5 config-space mirror. It records the mirrored 4 KiB config-space area as `mir_cfg_<BDF>_post-init-before-patch.bin` when loader tracing is enabled, and prints the extended-capability chain and ReBAR fields to the loader log.

## What this change does

- Reads the BAR5 mirror while the loader is still working on the same GPU.
- Reports whether the mirrored extended-capability chain contains PCIe capability ID `0x0015` (Resizable BAR).
- Logs the ReBAR capability dword, control dword, supported-size mask, BAR index, and selected size.
- Saves the mirrored 4 KiB area for comparison with a healthy RX 580.

It is diagnostic only. It does not manufacture a PCIe capability, request a larger BAR, change PCI resource allocation, or guarantee that Linux will expose an 8 GiB BAR.

## How to collect a useful comparison

1. Build and install this branch on the test system.
2. Enable loader tracing in the existing loader settings.
3. Boot once with a healthy card and collect its `loader_trace.txt` and `mir_cfg_*_post-init-before-patch.bin`.
4. Boot with one affected card and collect the same two files after the loader has initialized it.
5. Compare the same offsets in the two mirror dumps. Keep each file labelled with the physical card and PCI BDF.

Do not add guessed register writes to `rebar_patch.txt` yet. The proposed writes to registers `0xFC3`, `0x1559`, and `0x15FC` are a hypothesis, not verified strap controls. Writing them without proving their meaning could destabilize the GPU or hang the machine.

## Important architectural limitation

Resizable BAR allocation happens during PCI resource enumeration. A normal EFI application launched by GRUB runs after the motherboard firmware has already enumerated devices and assigned resources; changing BAR registers at that point cannot retroactively enlarge the allocated BAR.

The companion DXE package on `experiment/rx580-large-bar-dxe-candidate` is explicitly unverified. A DXE driver must be integrated into the motherboard firmware and dispatched before PCI resource collection. Even then, it can only request a size the device actually advertises and the platform can allocate. If the affected cards' PCIe extended-capability chain truly lacks the ReBAR capability, a driver that only changes the ReBAR control register cannot enable it; the source of the missing capability must first be established.

Do not flash modified motherboard firmware or run experimental register writes until the mirror comparison establishes a specific, documented register-level fix and the DXE driver has been built and reviewed.
