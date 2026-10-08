# AGENTS.md

## Project goal

`rx580_vbios_loader` is a low-level UEFI application for initializing AMD
Polaris/RX 580 GPUs when the card's SPI vBIOS cannot be read normally.

The project is intentionally source-only. Firmware payloads are supplied by
the person testing the hardware.

## Non-negotiable repository rules

1. **Never commit ROM/vBIOS dumps.**
   - `*.rom`, `*.bin`, extracted firmware, and vendor GOP/PE images are local
     test assets.
   - Keep them outside Git or under ignored paths.
2. **Never re-add embedded ROMs to `vbios_loader.c`.**
   - The loader must discover user-supplied ROMs at runtime.
3. **Do not commit `vbios_loader.efi`.**
   - Build artifacts are ignored.
4. Preserve third-party copyright/license notices.
5. Do not claim that vendor firmware is covered by this project's MIT license.
6. Keep user instructions explicit about using a **full SPI dump** when possible.
7. Avoid destructive firmware operations in scripts. This project loads ROM data;
   it should not silently flash a card.

## Architecture

- `vbios_loader.c`
  - **Engine (proven v0.1 code - treat as frozen):** ROM discovery/parsing, PE extraction,
    SPI/MC emulation, `run_cand()` (AtomBIOS ASIC_Init path), `prepare_for_os()`, VFCT,
    register access, trace.
  - **Control layer (v0.2, at the end of the file):** card detection, per-card ROM order,
    `process_card()` (AUTO / manual), reset modes, pages (`status_page`, `card_menu`), `efi_main`.
- `config.c` / `loader.h` - settings table, `key=value` parser, config file, NVRAM overrides
  (same text format as the file, stored in `VbiosLdrCfg`), per-card pins.
- `gfx.c` / `gfx.h` / `font_terminus.h` - software renderer on GOP (back buffer + one Blt).
- `ui.c` - GUI and text front ends (status page, list pages, settings, log).
- `atomlib/`, `atom_support.c` - AMD AtomBIOS-derived interpreter and adapter.
- `install_linux.sh`, `tools/`, `Makefile`.

### Engine rules (the card init path works - do not break it)

- `run_cand()` must keep the same register sequence. Only the card selection, VFCT hook and
  reset hook (`card_reset`) differ from v0.1. Diff against the previous release before merging.
- Per-ROM emulation state is created in `spi_setup()` and freed right after ASIC_Init; keep it
  that way so nothing leaks from one card to the next.
- **Never touch a card (config enable, MMIO) before `run_cand()`.** Detection reads PCI config space only;
  v0.2-beta did early enables/reads and froze a real card. Anything shown on a tile must come from values
  `run_cand`/`regs_line` already read - no extra register reads, especially after `prepare_for_os()`.
- All output goes through `lg()` (ring buffer + console in text mode), never `Print()` in GUI paths.
- The loader never writes `vbios_loader.cfg`.
- Persistent state: one NVRAM variable per card (`VbiosLdrC<bus><dev><fn>`), settings in `VbiosLdrCfg`.

### VFCT handoff (v0.2 fix)

- `run_cand()` only records per-card ROMs (`vfct_set/clear`); `publish_os_roms()` builds ONE VFCT after the last card.
- ROMs are copied into the table (ACPI reclaim memory); never point ACPI at loader pool buffers.
- Table/XSDT logic lives in `vfct.h` (no EFI calls); test with `tools/test_vfct.c` on the host.

### Testing without a GPU

`demo=1` shows five fake cards (no hardware access). QEMU + OVMF can run the GUI,
config parsing and NVRAM paths; two `-device ati-vga` cards exercise real PCI enumeration.
The ASIC init itself can only be tested on real hardware.

## Runtime ROM contract

The loader expects user-supplied files ending in `.rom`.

Recommended layout:

```text
EFI/vbios_loader/
├── vbios_loader.efi
├── vbios_loader.cfg      (optional)
└── vbioses/
    ├── 00_original_full_dump.rom
    ├── 01_matching_stock.rom
    └── 02_fallback.rom
```

The filename does not need a particular model string. Alphabetical ordering is
used for candidate order.

A complete programmer dump is preferable to a trimmed BIOS image because
Polaris initialization may require data beyond the visible BIOS image, including
the memory-controller block.

## How a developer should test

Keep private test firmware outside the repository, for example:

```text
~/rx580-test-firmware/
├── original_full_dump.rom
├── stock_candidate.rom
└── modified_candidate.rom
```

Copy or symlink test ROMs into a local ignored `vbioses/` directory only when
needed.

Before committing:

```bash
git status --short
git diff --check
git ls-files | grep -Ei '\.(rom|bin|efi)$'
```

The final command should return no tracked firmware/build artifacts.

Build:

```bash
make clean && make
```

For installer testing, create a local `vbioses/` directory and place only
private test dumps there.

## Firmware handling

### User dump

If the user can still read the SPI flash:

1. Make a complete backup/dump first.
2. Verify the dump size against the physical flash capacity.
3. Keep the original untouched.
4. Rename a working copy if desired.
5. Place it in `vbioses/`.
6. Give the preferred candidate a `00_` prefix so it is tried first.

Never ask users to overwrite their only backup.

### Stock/downloaded ROM

A stock ROM can be used as a candidate when an original dump is unavailable,
but compatibility must be checked carefully. Useful sources include the
TechPowerUp VGA BIOS Collection. Do not mirror those firmware files into this
repository.

### MC fallback

The loader can search other supplied ROM files for a valid Polaris memory
controller block. Therefore multiple candidate ROMs can be useful, but this
does not make arbitrary ROMs compatible.

## Licensing

The root `LICENSE` applies to original project code.

`atomlib/` contains AMD AtomBIOS-derived code whose source files include their
own permissive AMD copyright/license text. Do not remove those notices.

Firmware is a separate category. ROM/GOP/vendor firmware is not automatically
MIT merely because this repository is MIT. Keep firmware out of the repository.

## Code-change guidance

When changing settings: add the entry to `g_setdef[]` (config.c) and `vbios_loader.cfg.example`,
and the README table. Defaults must keep a config-less install working.

When changing ROM discovery:

- preserve FAT-only assumptions;
- preserve case-insensitive `.rom` matching;
- preserve deterministic filename ordering;
- keep the search bounded so malformed FAT trees cannot cause an unbounded scan;
- do not silently select a random ROM from another removable drive if that
  behavior is changed;
- update README instructions whenever the expected directory layout changes.

When changing initialization:

- prefer bounded waits/timeouts;
- keep trace logging useful after a hard freeze;
- avoid changes that turn a recoverable initialization failure into an
  unconditional machine hang/reset;
- document hardware-specific assumptions near the code that uses them.

## Build outputs

Expected local output:

```text
vbios_loader.efi
```

Do not add it to Git. GitHub users should build it from source.

## Before a release

Check all of the following:

- [ ] No `.rom`, `.bin`, vendor firmware, or extracted GOP files are tracked.
- [ ] No prebuilt `vbios_loader.efi` is tracked.
- [ ] `README.md` explains how to obtain/dump a user's own ROM.
- [ ] `README.md` explains full-dump vs trimmed-image behavior.
- [ ] `LICENSE` is present.
- [ ] AMD AtomBIOS notices remain intact.
- [ ] `git diff --check` passes.
- [ ] `make clean && make` succeeds on a supported Ubuntu/Debian environment.
- [ ] Installer paths match the README.
- [ ] No personal diagnostic logs or hardware-specific secrets are present.
