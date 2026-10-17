# ReBAR diagnostics and early/late sizing path

The early-DXE branch adds two explicit 8 GiB BAR0 request points:

- A host-bridge resource-allocation hook targets AMD 1002:67DF before resource collection. It writes only if the live PCIe ReBAR capability advertises 8 GiB for BAR0.
- The existing RX 580 loader attempts a late ECAM-based BAR0 size request after successful AtomBIOS ASIC_Init, in case initialization exposes the ReBAR capability only at that stage.

The loader also reports the BAR5-mirrored extended-capability chain. Enable loader tracing to save config snapshots such as cfg_<BDF>_pre.bin, cfg_<BDF>_post.bin, and mir_cfg_<BDF>_post-init-before-patch.bin.

## What can be established

- Whether PCIe extended capability ID 0x0015 is visible at each observation point.
- The advertised supported-size mask for each ReBAR entry.
- Which BAR the entry describes and its current size selector.
- Whether an 8 GiB selector request was accepted by live ECAM readback.

A selector being set is not proof that an 8 GiB MMIO address range has been allocated. The early DXE hook runs before resource collection so the platform can size the BAR at allocation time. The loader's late request runs after motherboard allocation and cannot retroactively move the BAR or adjust the bridge windows.

## How to test

1. First build the loader branch and the early DXE driver. Do not flash the DXE module until the binary and integration changes have been reviewed.
2. With the DXE module integrated, boot with one affected card and read the volatile UEFI log using tools/read_rebar_efi_log.py.
3. Enable loader tracing and retain loader_trace.txt plus matching config and BAR5-mirror snapshots for one healthy and one affected card.
4. Use tools/compare_rebar.py --good <healthy-snapshot.bin> --bad <affected-snapshot.bin> to compare what was visible.
5. If only the post-ASIC_Init path can select 8 GiB, test Linux with pci=realloc added to the current kernel command line. Preserve the rest of the parameters and verify the final resource size using lspci -vv -s BB:DD.F.

Do not add guessed writes to rebar_patch.txt. Registers 0xFC3, 0x1559, and 0x15FC have appeared as hypotheses in prior investigation but are not verified strap controls. Writes without confirmation could destabilize the GPU or hang the machine.

## Limitation

If the capability is absent before PCI resource collection but appears after ASIC_Init, the current early DXE module cannot fix that by itself: it does not execute AtomBIOS or emulate the dead SPI during the early boot phase. If the capability remains absent after ASIC_Init, or the advertised size mask lacks the 8 GiB bit, neither size selector path can safely manufacture the missing support. That requires a further device-specific finding from register/config comparisons before an additional write can be justified.

Do not flash modified motherboard firmware or run speculative register writes until the DXE package is built and reviewed and the relevant diagnostics establish the capability/size behavior.
