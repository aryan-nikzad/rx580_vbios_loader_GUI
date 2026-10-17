# ReBAR diagnostics and early/late sizing path

The early-DXE branch adds two explicit 8 GiB BAR0 request points:

- A forced host-bridge resource-allocation hook targets AMD `1002:67DF` before resource collection. It requires the real ReBAR capability and BAR0 entry, but deliberately writes selector exponent 13 even if the size mask does not advertise 8 GiB.
- The existing RX 580 loader has an opt-in late ECAM-based BAR0 size request after successful AtomBIOS ASIC_Init. It is disabled by default; set rebar_late=1 in vbios_loader.cfg only for a controlled Linux test with pci=realloc.

The loader also reports the BAR5-mirrored extended-capability chain. Enable loader tracing to save config snapshots such as cfg_<BDF>_pre.bin, cfg_<BDF>_post.bin, and mir_cfg_<BDF>_post-init-before-patch.bin.

## What can be established

- Whether PCIe extended capability ID 0x0015 is visible at each observation point.
- The advertised supported-size mask for each ReBAR entry.
- Which BAR the entry describes and its current size selector.
- Whether an 8 GiB selector request was accepted by register readback, including cases where the advertised mask omits 8 GiB. Readback acceptance is not proof the device can function at 8 GiB.

A selector being set is not proof that an 8 GiB MMIO address range has been allocated. The early DXE hook runs before resource collection so the platform can size the BAR at allocation time. The loader's late request runs after motherboard allocation and cannot retroactively move the BAR or adjust the bridge windows.

## How to test

1. Build the loader branch and forced early DXE driver. Its `BEFORE` dependency targets the PciBus FFS GUID `3C1DE39F-D207-408A-AACC-731CFB7F1DD7`; check the actual image in UEFITool and confirm the module is in the same DXE volume as PciBus. Do not flash until both FFS and integrated BIOS have been inspected.
2. With the DXE module integrated, boot with one affected card and read the volatile UEFI log using tools/read_rebar_efi_log.py.
3. Enable loader tracing and retain loader_trace.txt plus matching config and BAR5-mirror snapshots for one healthy and one affected card.
4. Use tools/compare_rebar.py --good <healthy-snapshot.bin> --bad <affected-snapshot.bin> to compare what was visible.
5. The forced early path does not run ASIC_Init. If the ReBAR capability is absent until post-ASIC_Init, this DXE path cannot apply the selector. For the separate late loader path, first add `pci=realloc` to the existing Linux kernel command line (preserve all current parameters), then set `rebar_late=1` in `EFI/vbios_loader/vbios_loader.cfg` and run a controlled test on one card. Verify the final resource size using lspci -vv -s BB:DD.F. Set rebar_late=0 again if the test fails.

Do not add guessed writes to rebar_patch.txt. Registers 0xFC3, 0x1559, and 0x15FC have appeared as hypotheses in prior investigation but are not verified strap controls. Writes without confirmation could destabilize the GPU or hang the machine.

## Limitation

If the capability is absent before PCI resource collection but appears after ASIC_Init, the current early DXE module cannot fix that by itself: it does not execute AtomBIOS or emulate the dead SPI during the early boot phase. If the capability remains absent after ASIC_Init, neither path can safely manufacture that missing capability. If the capability is present but the advertised size mask lacks bit 13, this forced early build deliberately tries the selector anyway; that is a risky experiment, not proof that the device supports 8 GiB. The late loader path still refuses an unsupported-size request.

Do not flash modified motherboard firmware or run speculative register writes until the DXE package is built and reviewed and the relevant diagnostics establish the capability/size behavior.
